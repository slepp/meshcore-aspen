package radio

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

func TestSessionNativeFirmwareGoConnect(t *testing.T) {
	binary := os.Getenv("MKISS_NATIVE_HARNESS")
	if binary == "" {
		t.Skip("run make -f test_support/phy_parity/Makefile mkiss-go-test")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, binary, "--serve")
	output, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cancel()
		_ = cmd.Wait()
	})
	scanner := bufio.NewScanner(output)
	var address string
	for scanner.Scan() {
		line := scanner.Text()
		if port, found := strings.CutPrefix(line, "MKISS_PORT="); found {
			n, err := strconv.Atoi(port)
			if err != nil || n <= 0 || n > 65535 {
				t.Fatalf("invalid native harness port: %q", line)
			}
			address = fmt.Sprintf("127.0.0.1:%d", n)
			break
		}
	}
	if address == "" {
		t.Fatalf("native firmware did not start: %v", scanner.Err())
	}
	checkSessionEndpoints(t, ctx, address, false)
}

func TestSessionDeviceEndpoints(t *testing.T) {
	address := os.Getenv("MKISS_DEVICE_ADDRESS")
	if address == "" {
		t.Skip("run make test-mkiss-device MKISS_DEVICE_ADDRESS=host:8001")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	checkSessionEndpoints(t, ctx, address, true)
}

func checkSessionEndpoints(t *testing.T, ctx context.Context, address string, device bool) {
	t.Helper()
	config := func(session *Session, port int) Config {
		cfg := Config{
			Address: address,
			Radio: hardware.RadioConfig{
				FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5,
			},
			TxPower: 20, Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
			RequireParity: true, ConfigurationOwner: port == 0,
			Session: session, SessionPort: port,
		}
		if device {
			cfg.ConfigurationOwner = false
			cfg.PHYTracking = PHYFollow
		}
		return cfg
	}
	session := NewSession(address, 2)
	owner, err := Open(ctx, config(session, 0))
	if err != nil {
		t.Fatalf("owner HELLO and CAPACITY against native firmware: %v", err)
	}
	defer owner.Close()
	child, err := Open(ctx, config(session, 1))
	if err != nil {
		t.Fatalf("child Go modem preamble, HELLO and CONFIG: %v", err)
	}
	defer child.Close()
	if !owner.Online() || !child.Online() {
		t.Fatal("native aggregate session did not connect")
	}
	reply, err := child.modem.Request(ctx, hardware.HW_CMD_GET_SIGNAL_REPORT, nil)
	if err != nil || len(reply) != 1 || reply[0] != 1 {
		t.Fatalf("child lost Go modem signal-report preamble: %x, %v", reply, err)
	}
	links := []*Link{owner, child}
	for port := 2; port < sessionPorts; port++ {
		link, err := Open(ctx, config(session, port))
		if err != nil {
			t.Fatalf("virtual port %d: %v", port, err)
		}
		defer link.Close()
		links = append(links, link)
	}
	if err := links[2].modem.SetSignalReport(false); err != nil {
		t.Fatalf("change virtual port 2 signal reports: %v", err)
	}
	for port, link := range links {
		expected := byte(1)
		if port == 2 {
			expected = 0
		}
		reply, err := link.modem.Request(ctx, hardware.HW_CMD_GET_SIGNAL_REPORT, nil)
		if err != nil || len(reply) != 1 || reply[0] != expected {
			t.Fatalf("virtual port %d signal setting leaked: %x, %v", port, reply, err)
		}
		if !link.Online() {
			t.Fatalf("virtual port %d went offline", port)
		}
	}
	second := NewSession(address, 2)
	secondConfig := config(second, 0)
	secondConfig.ConfigurationOwner = false
	_, err = Open(ctx, secondConfig)
	if !errors.Is(err, ErrSubportsBusy) {
		t.Fatalf("native busy session did not report retryable busy: %v", err)
	}
	if _, err := second.Capacity(); !errors.Is(err, ErrOffline) {
		t.Fatalf("busy connection remained available: %v", err)
	}
	direct, err := Open(ctx, config(nil, 1))
	if err != nil {
		t.Fatalf("direct KISS connection blocked by aggregate session: %v", err)
	}
	defer direct.Close()
	t.Log("four independent virtual endpoints share one TCP session; busy session denied; direct KISS coexists")
}
