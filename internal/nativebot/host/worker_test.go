package nativebot

import (
	"context"
	"encoding/binary"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/radio"
)

func TestHelloUsesVerifiedProfileAndFullMastAirtimeTable(t *testing.T) {
	table := make([]uint32, 256)
	for n := 1; n < len(table); n++ {
		table[n] = uint32(n * 7)
	}
	profile := radio.PHYState{ConfigurationGeneration: 42, Settings: radio.PHYSettings{
		Radio: hardware.RadioConfig{FreqHz: 912525000, BwHz: 250000, SF: 7, CR: 5}, TxPower: 18,
	}}
	identity := make([]byte, 64)
	identity[0] = 0x18
	wire, err := helloFrame(identity, profile, table, -113)
	if err != nil {
		t.Fatal(err)
	}
	if len(wire) != 1108 || binary.LittleEndian.Uint32(wire) != 1104 ||
		wire[4] != 1 || binary.LittleEndian.Uint16(wire[5:]) != 1 ||
		wire[7] != identity[0] || binary.LittleEndian.Uint32(wire[71:]) != 912525000 ||
		binary.LittleEndian.Uint32(wire[75:]) != 250000 ||
		wire[79] != 7 || wire[80] != 5 || wire[81] != 18 ||
		int16(binary.LittleEndian.Uint16(wire[82:])) != -113 ||
		binary.LittleEndian.Uint32(wire[84+255*4:]) != 1785 {
		t.Fatalf("HELLO has incorrect framing or real PHY fields: %x", wire[:96])
	}
	table[254] = 0
	if _, err := helloFrame(identity, profile, table, 0); err == nil {
		t.Fatal("incomplete mast airtime table admitted")
	}
}

func TestRadioResultsSeparateAdmissionAndTerminalOutcome(t *testing.T) {
	for _, tc := range []struct {
		state  radio.TXState
		reason uint8
		hasRF  byte
	}{
		{radio.TXAccepted, 0, 0},
		{radio.TXSucceeded, 0, 1},
		{radio.TXFailed, 8, 1},
		{radio.TXUnknown, 9, 0},
		{radio.TXRejected, 2, 0},
	} {
		wire := txResult(42, tc.state, tc.reason, 12, 13, 14)
		if binary.LittleEndian.Uint32(wire) != 20 ||
			wire[4] != 3 || binary.LittleEndian.Uint32(wire[5:]) != 42 ||
			wire[9] != byte(tc.state) || wire[10] != tc.reason ||
			binary.LittleEndian.Uint32(wire[11:]) != 12 ||
			binary.LittleEndian.Uint32(wire[15:]) != 13 ||
			binary.LittleEndian.Uint32(wire[19:]) != 14 || wire[23] != tc.hasRF {
			t.Fatalf("terminal result corrupted or accepted mistaken for RF success: %x", wire)
		}
	}
}

func TestPhysicalSnapshotPreservesSeparateSourceAndAggregateCounters(t *testing.T) {
	stats := radio.PHYStatistics{
		ConfigurationGeneration: 12, AggregateCredit: 120 * time.Millisecond,
		SourceCredit: 30 * time.Millisecond, AggregateRFAirtime: 90 * time.Millisecond,
		SourceRFAirtime: 20 * time.Millisecond, AggregateSuccesses: 7,
		AggregateFailures: 1, SourceSuccesses: 2, SourceFailures: 3, Queued: 4,
		Transmitting: true,
	}
	frame, err := statsFrame(11, stats)
	if err != nil {
		t.Fatal(err)
	}
	if len(frame) != 47 || binary.LittleEndian.Uint32(frame) != 43 || frame[4] != 5 ||
		binary.LittleEndian.Uint32(frame[5:]) != 11 ||
		binary.LittleEndian.Uint32(frame[9:]) != 12 ||
		binary.LittleEndian.Uint32(frame[13:]) != 120 ||
		binary.LittleEndian.Uint32(frame[17:]) != 30 ||
		binary.LittleEndian.Uint32(frame[21:]) != 90 ||
		binary.LittleEndian.Uint32(frame[25:]) != 20 ||
		binary.LittleEndian.Uint32(frame[29:]) != 7 ||
		binary.LittleEndian.Uint32(frame[33:]) != 1 ||
		binary.LittleEndian.Uint32(frame[37:]) != 2 ||
		binary.LittleEndian.Uint32(frame[41:]) != 3 || frame[45] != 4 || frame[46] != 1 {
		t.Fatalf("actual physical counters changed in native bridge: %x", frame)
	}
}

func TestOwnerCommandsAreBoundedAndCorrelated(t *testing.T) {
	w := &Worker{ctx: context.Background(), send: make(chan []byte, 1), pendingAdmin: make(map[uint32]chan string)}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() {
		reply, err := w.Admin(ctx, "source status")
		if reply != "gen=2 active=3" || err != nil {
			done <- errors.New("reply failed to correlate to owner request")
			return
		}
		done <- nil
	}()
	select {
	case wire := <-w.send:
		if binary.LittleEndian.Uint32(wire) != uint32(5+len("source status")) ||
			wire[4] != 4 || binary.LittleEndian.Uint32(wire[5:]) != 1 ||
			string(wire[9:]) != "source status" {
			t.Fatalf("invalid owner frame: %x", wire)
		}
		w.adminMu.Lock()
		w.pendingAdmin[1] <- "gen=2 active=3"
		w.adminMu.Unlock()
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	for _, command := range []string{"", "source\nremove", strings.Repeat("a", 161)} {
		if _, err := w.Admin(ctx, command); err == nil {
			t.Fatalf("invalid owner request %q admitted", command)
		}
	}
}
