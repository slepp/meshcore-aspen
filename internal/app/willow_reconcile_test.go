package app

import (
	"bufio"
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/state"
)

// The Python lifecycle supplies a NEW reconciled candidate and authenticated
// requests. Run the actual Go application against its existing localhost mast.
func TestWillowReconciledApplication(t *testing.T) {
	path := os.Getenv("MESHCORE_WILLOW_RECONCILED")
	if path == "" {
		t.Skip("set MESHCORE_WILLOW_RECONCILED to the isolated lifecycle transcript")
	}
	var fixture struct {
		Root, Worker, Advert, AdminPassword string
		Requests                            []struct {
			Raw, Want string
			Seed      byte
		}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err = json.Unmarshal(data, &fixture); err != nil {
		t.Fatal(err)
	}
	cfg := DefaultConfig()
	cfg.StateDir, cfg.StatusListen = fixture.Root, freeBotStatusAddress(t)
	cfg.EnabledRoles = []string{"repeater", "room", "bot"}
	cfg.BotRuntime, cfg.BotNativeWorker = "native_lua", fixture.Worker
	cfg.BotListen, cfg.RoomPassword = "not-a-listener", "room"
	cfg.AdminPasswordEnv = "WILLOW_RECONCILE_TEST_PASSWORD"
	t.Setenv(cfg.AdminPasswordEnv, fixture.AdminPassword)
	for _, role := range []string{"repeater", "room"} {
		id, err := state.Identity(cfg.StateDir, role)
		if err != nil {
			t.Fatal(err)
		}
		raw, err := state.ReadRoleJSON(filepath.Join(cfg.StateDir, role, "state.json"))
		if err != nil {
			t.Fatal(err)
		}
		var doc struct {
			Identity, Retention string
		}
		if err := json.Unmarshal(raw, &doc); err != nil || doc.Identity != id.String() || doc.Retention != "durable-replay" {
			t.Fatal("reconciled state identity/retention mismatch", err)
		}
	}
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.completeTX, mast.fastAirtime = true, true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	ctx, cancel := context.WithCancel(context.Background())
	stopped := make(chan error, 1)
	go func() { stopped <- Run(ctx, cfg, slog.New(slog.NewTextHandler(io.Discard, nil))) }()
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-stopped:
			if err != nil {
				t.Error(err)
			}
		case <-time.After(8 * time.Second):
			t.Error("reconciled Go application did not stop")
		}
	})
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	waitAuthority(t, func() bool {
		conn, err := net.DialTimeout("unix", socket, time.Second)
		if err != nil {
			return false
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(time.Second))
		fmt.Fprintln(conn, "status")
		reply, err := bufio.NewReader(conn).ReadString('\n')
		return err == nil && bytes.Contains([]byte(reply), []byte("ready=1"))
	})
	decode := func(value string) []byte {
		t.Helper()
		raw, err := hex.DecodeString(value)
		if err != nil {
			t.Fatal(err)
		}
		return raw
	}
	inject := func(raw []byte) {
		t.Helper()
		mast.mu.Lock()
		connections := append([]net.Conn(nil), mast.active...)
		mast.mu.Unlock()
		wire := append(hardware.EncodeFrame(0, hardware.KISS_CMD_DATA, raw),
			hardware.EncodeHardwareFrame(0, hardware.HW_RESP_RX_META, []byte{0x14, 0xb8})...)
		for _, conn := range connections {
			if _, err := conn.Write(wire); err != nil {
				t.Fatal(err)
			}
		}
	}
	inject(decode(fixture.Advert))
	time.Sleep(100 * time.Millisecond)
	peer := meshcore.NewLocalIdentityFromSeed([32]byte(bytes.Repeat([]byte{2}, 32)))
	for _, request := range fixture.Requests {
		t.Logf("reconciled role seed=%d expected=%q", request.Seed, request.Want)
		mast.mu.Lock()
		start := len(mast.txPackets)
		mast.mu.Unlock()
		inject(decode(request.Raw))
		recipient := meshcore.NewLocalIdentityFromSeed([32]byte(bytes.Repeat([]byte{request.Seed}, 32)))
		secret, err := peer.SharedSecret(recipient.Identity)
		if err != nil {
			t.Fatal(err)
		}
		waitAuthority(t, func() bool {
			mast.mu.Lock()
			defer mast.mu.Unlock()
			for _, raw := range mast.txPackets[start:] {
				packet, err := meshcore.PacketFromBytes(raw)
				if err != nil || len(packet.Payload) < 4 {
					continue
				}
				plain, err := meshcore.MACThenDecrypt(secret, packet.Payload[2:])
				if err != nil {
					continue
				}
				if request.Want == "" && (packet.PayloadType() == meshcore.PayloadTypeResponse || packet.PayloadType() == meshcore.PayloadTypePath) {
					return true
				}
				if packet.PayloadType() == meshcore.PayloadTypeTxtMsg && len(plain) >= 5 && string(bytes.TrimRight(plain[5:], "\x00")) == request.Want {
					return true
				}
			}
			return false
		})
	}
	t.Log("reconciled relay/room loaded; authenticated management and both retained native notes succeeded")
}
