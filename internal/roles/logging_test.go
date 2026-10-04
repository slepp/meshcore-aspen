package roles

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"meshcore.local/meshcore/internal/radio"
)

func TestPacketLogLimitStopsCaptureWithoutOverwriting(t *testing.T) {
	cfg := config(t)
	cfg.MaxLogBytes = 1024
	errs := make(chan error, 4)
	cfg.ErrorHandler = func(err error) { errs <- err }
	s, r, id := startRole(t, false, cfg)
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	path := filepath.Join(cfg.StateDir, "packet.log")
	before := bytes.Repeat([]byte{'x'}, 1023)
	if err := os.WriteFile(path, before, 0600); err != nil {
		t.Fatal(err)
	}
	r.inject(t, textWire(t, admin, id, 2, 4, "log start"), false)
	r.next(t)
	select {
	case err := <-errs:
		if !strings.Contains(err.Error(), "log limit") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("full packet log was not diagnosed")
	}
	if s.logging.Load() {
		t.Fatal("capture did not stop at its bound")
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("log limit silently overwrote retained data")
	}
}

func TestPacketLogSeparatesPhysicalTXOutcomes(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "log start"), false)
	r.next(t)
	for _, state := range []radio.TXState{radio.TXAccepted, radio.TXSucceeded, radio.TXRejected, radio.TXFailed, radio.TXUnknown} {
		s.LogTXResult(radio.TXResult{
			Generation: 7, JobID: 100 + uint32(state), State: state, Reason: 3,
			QueueWait: 2 * time.Second, RFAirtime: 120 * time.Millisecond,
			EstimatedAirtime: 200 * time.Millisecond,
		})
	}
	r.inject(t, textWire(t, admin, id, 3, 4, "log stop"), false)
	r.next(t)
	data, err := s.ReadPacketLog()
	if err != nil {
		t.Fatal(err)
	}
	seen := make(map[radio.TXState]int)
	for _, line := range bytes.Split(bytes.TrimSpace(data), []byte{'\n'}) {
		var entry packetLogEntry
		if err := json.Unmarshal(line, &entry); err != nil {
			t.Fatal(err)
		}
		if entry.TXResult == nil {
			continue
		}
		result := entry.TXResult
		if entry.Direction != "TX_RESULT" || entry.SNR != nil || entry.RSSI != nil || entry.Local ||
			result.Generation != 7 || result.JobID != 100+uint32(result.State) || result.Reason != 3 ||
			result.QueueWait != 2*time.Second || result.RFAirtime != 120*time.Millisecond ||
			result.EstimatedAirtime != 200*time.Millisecond {
			t.Fatalf("TX outcome lost attribution or conflated RF/queue/estimate: %+v", entry)
		}
		seen[result.State]++
	}
	if len(seen) != 3 || seen[radio.TXRejected] != 1 || seen[radio.TXFailed] != 1 || seen[radio.TXUnknown] != 1 {
		t.Fatalf("terminal outcomes confused with admission/success: %v", seen)
	}
	s.LogTXResult(radio.TXResult{State: radio.TXUnknown, JobID: 999})
	after, err := s.ReadPacketLog()
	if err != nil || !bytes.Equal(data, after) {
		t.Fatal("disabled packet logging accepted a TX outcome")
	}
}

func TestPacketLogOverflowReportsFromWorker(t *testing.T) {
	cfg := config(t)
	failures := make(chan error, 1)
	cfg.ErrorHandler = func(err error) { failures <- err }
	entered, release := make(chan struct{}), make(chan struct{})
	cfg.Telemetry = func() (Telemetry, error) {
		close(entered)
		<-release
		return Telemetry{HasBattery: true}, nil
	}
	s, r, id := startRole(t, false, cfg)
	released := false
	defer func() {
		if !released {
			close(release)
		}
	}()
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "log start"), false)
	r.next(t)
	r.inject(t, peerWire(t, 0, admin, id, []byte{3, 0, 0, 0, 1}), false)
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("application worker did not reach the telemetry barrier")
	}
	// A busy application worker must not make physical outcome callbacks call
	// the error handler or wait for packet-log disk I/O.
	for i := range cap(s.queue) + 1 {
		s.LogTXResult(radio.TXResult{State: radio.TXUnknown, JobID: uint32(i + 1)})
	}
	select {
	case err := <-failures:
		t.Fatalf("physical callback reported synchronously: %v", err)
	default:
	}
	close(release)
	released = true
	select {
	case err := <-failures:
		if !errors.Is(err, ErrQueueFull) || !strings.Contains(err.Error(), "dropped") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("worker did not surface dropped log records")
	}
}

func TestPacketLogCommandsRetainMeasuredAndLocalDistinction(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, textWire(t, admin, id, 2, 4, "log start"), false)
	body := decrypted(t, r.next(t), admin, id)
	if string(cstring(body[5:])) != "   logging on" {
		t.Fatalf("native log start response %x", body)
	}
	measured := textWire(t, admin, id, 3, 4, "get name")
	r.inject(t, measured, false)
	r.next(t)
	local := textWire(t, admin, id, 4, 4, "clock")
	r.inject(t, local, true)
	r.next(t)
	r.inject(t, textWire(t, admin, id, 5, 4, "log stop"), false)
	r.next(t)
	data, err := s.ReadPacketLog()
	if err != nil {
		t.Fatal(err)
	}
	var sawMeasured, sawLocal bool
	for _, line := range bytes.Split(bytes.TrimSpace(data), []byte{'\n'}) {
		if len(line) == 0 {
			continue
		}
		var entry packetLogEntry
		if err := json.Unmarshal(line, &entry); err != nil {
			t.Fatal(err)
		}
		switch entry.Raw {
		case hex.EncodeToString(measured):
			sawMeasured = entry.Direction == "RX" && !entry.Local && entry.SNR != nil && *entry.SNR == 4.5 && entry.RSSI != nil && *entry.RSSI == -73
		case hex.EncodeToString(local):
			sawLocal = entry.Direction == "RX" && entry.Local && entry.SNR == nil && entry.RSSI == nil
		}
	}
	if !sawMeasured || !sawLocal {
		t.Fatalf("packet log lost signal provenance: %s", data)
	}
	r.inject(t, textWire(t, admin, id, 6, 4, "log erase"), false)
	body = decrypted(t, r.next(t), admin, id)
	if string(cstring(body[5:])) != "   log erased" {
		t.Fatalf("native log erase response %x", body)
	}
	data, err = s.ReadPacketLog()
	if err != nil || len(data) != 0 {
		t.Fatalf("log erase not applied: %s (%v)", data, err)
	}
}
