package radio

import (
	"context"
	"encoding/binary"
	"errors"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

func airtimeReply(ms uint32) []byte {
	response := make([]byte, 5)
	response[0] = hardware.HwResp(hardware.HW_CMD_GET_AIRTIME)
	binary.LittleEndian.PutUint32(response[1:], ms)
	return response
}

func TestAirtimeEstimatorSnapshotsModemValues(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity = true
	phy.airtimeRequests = make(chan []byte, 256)
	var expected [256]uint32
	for length := range expected {
		expected[length] = uint32(1000 + length)
	}
	expected[20], expected[149], expected[255] = 40, 135, 212
	phy.airtimeResponse = func(length byte) []byte { return airtimeReply(expected[length]) }
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cfg := queuedConfig(phy, true)
	cfg.PHYGroup = NewPHYGroup()
	link, err := Open(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	estimate := link.AirtimeEstimator()
	link.Close()
	for length, want := range expected {
		if got := estimate(length); got != want {
			t.Fatalf("length %d: got %d ms, want modem value %d", length, got, want)
		}
		select {
		case request := <-phy.airtimeRequests:
			if len(request) != 1 || request[0] != byte(length) {
				t.Fatalf("request %d: %x", length, request)
			}
		default:
			t.Fatalf("missing request for length %d", length)
		}
	}
	select {
	case request := <-phy.airtimeRequests:
		t.Fatalf("unexpected extra query: %x", request)
	default:
	}
	if estimate(-1) != 0 || estimate(256) != 0 {
		t.Fatal("out-of-range length was not rejected")
	}
	if table := link.AirtimeTable(); len(table) != 256 || table[149] != 135 {
		t.Fatalf("status table: %v", table)
	}
}

func TestAirtimeEstimatorDiscardsIncompleteSnapshot(t *testing.T) {
	for _, failure := range []string{"rpc", "short", "long", "cancelled"} {
		t.Run(failure, func(t *testing.T) {
			phy := newTestPHY(t)
			phy.parity = true
			phy.airtimeRequests = make(chan []byte, 512)
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			capture, cancelCapture := context.WithCancel(ctx)
			defer cancelCapture()
			var recovered atomic.Bool
			phy.airtimeResponse = func(length byte) []byte {
				if length == 7 && !recovered.Load() {
					switch failure {
					case "rpc":
						return []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}
					case "short":
						return []byte{hardware.HwResp(hardware.HW_CMD_GET_AIRTIME), 40}
					case "long":
						return []byte{hardware.HwResp(hardware.HW_CMD_GET_AIRTIME), 40, 0, 0, 0, 0}
					case "cancelled":
						cancelCapture()
						return nil
					}
				}
				return airtimeReply(40)
			}
			cfg := queuedConfig(phy, true)
			cfg.PHYGroup = NewPHYGroup()
			link, err := Open(capture, cfg)
			if link != nil {
				link.Close()
			}
			if err == nil || !strings.Contains(err.Error(), "length 7") {
				t.Fatalf("partial snapshot accepted: link=%v error=%v", link != nil, err)
			}
			if failure == "rpc" && !errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_INVALID_PARAM)) {
				t.Fatalf("lost RPC error: %v", err)
			}
			if failure == "cancelled" && !errors.Is(err, context.Canceled) {
				t.Fatalf("lost cancellation: %v", err)
			}
			if len(phy.airtimeRequests) != 8 {
				t.Fatalf("queried %d lengths after failure, want 8", len(phy.airtimeRequests))
			}
			deadline := time.Now().Add(2 * time.Second)
			for phy.disconnects.Load() == 0 {
				if time.Now().After(deadline) {
					t.Fatal("failed snapshot left its connection open")
				}
				time.Sleep(time.Millisecond)
			}
			// A failed sweep is not cached: the next link reads its own table.
			recovered.Store(true)
			for len(phy.airtimeRequests) > 0 {
				<-phy.airtimeRequests
			}
			link, err = Open(ctx, cfg)
			if err != nil {
				t.Fatal(err)
			}
			defer link.Close()
			if len(phy.airtimeRequests) != 256 || link.AirtimeEstimator()(7) != 40 {
				t.Fatalf("recovery queried %d lengths", len(phy.airtimeRequests))
			}
		})
	}
}

func TestAirtimeEstimatorWithoutGroup(t *testing.T) {
	if estimate := (&Link{}).AirtimeEstimator(); estimate != nil {
		t.Fatal("link without a PHY group claimed an airtime model")
	}
	estimate := (&Link{config: Config{PHYGroup: NewPHYGroup()}}).AirtimeEstimator()
	if estimate == nil || estimate(10) != 0 {
		t.Fatal("unread airtime model returned an estimate")
	}
}
