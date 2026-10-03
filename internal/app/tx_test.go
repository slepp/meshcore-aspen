package app

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"sync"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/radio"
)

func TestPhysicalTXOutcomesUseOnlyReportedRFDuration(t *testing.T) {
	var logs bytes.Buffer
	diagnostics := newTXDiagnostics(slog.New(slog.NewJSONHandler(&logs, nil)))
	t.Cleanup(func() { _ = diagnostics.Close() })
	tx := newRoleTX("repeater", true, diagnostics)
	sibling := newRoleTX("room", true, diagnostics)
	started := tx.snapshot().Since
	tx.record(radio.TXResult{Generation: 1, JobID: 1, State: radio.TXAccepted,
		QueueWait: time.Minute, EstimatedAirtime: 5 * time.Second})
	admission := tx.snapshot()
	if admission.Accepted != 1 || admission.Succeeded != 0 || admission.RFAirtime != 0 {
		t.Fatalf("admission became RF success or airtime: %+v", admission)
	}
	tx.record(radio.TXResult{Generation: 1, JobID: 1, State: radio.TXSucceeded,
		QueueWait: time.Minute, EstimatedAirtime: 5 * time.Second, RFAirtime: 1250 * time.Millisecond})
	tx.record(radio.TXResult{Generation: 1, JobID: 2, State: radio.TXRejected, Reason: 4})
	tx.record(radio.TXResult{Generation: 1, JobID: 3, State: radio.TXFailed, Reason: 7,
		QueueWait: time.Hour, EstimatedAirtime: 10 * time.Second, RFAirtime: 250 * time.Millisecond})
	physical := radio.Telemetry{
		HasBattery: true, BatteryMilliVolts: 4000, HasCounters: true,
		HasNoiseFloor: true, NoiseFloorDBm: -118,
		HasCurrentRSSI: true, CurrentRSSIDBm: -70,
		HasMCUTemperature: true, MCUTemperatureC: 42,
		Counters: hardware.FirmwareStats{PacketsSent: 900, PacketsRecv: 800, PacketsErrors: 70},
	}
	complete := roleTelemetry(physical, tx.snapshot())
	if complete.TXAirtime != 1500*time.Millisecond || !complete.HasTXAirtime ||
		complete.HasRXAirtime || complete.HasAirtime || complete.RXAirtime != 0 ||
		complete.ErrorEvents != 0 || complete.HasErrorEvents ||
		!complete.HasBattery || complete.BatteryMilliVolts != 4000 ||
		!complete.HasNoiseFloor || complete.NoiseFloorDBm != -118 ||
		!complete.HasCurrentRSSI || complete.CurrentRSSIDBm != -70 ||
		!complete.HasMCUTemperature || complete.MCUTemperatureC != 42 ||
		!complete.HasCounters || complete.Counters != physical.Counters {
		t.Fatalf("role telemetry conflated outcomes, RF, RX or shared counters: %+v", complete)
	}
	tx.record(radio.TXResult{Generation: 1, JobID: 4, State: radio.TXUnknown, Reason: 9})
	// A new source generation is a TCP reconnect, not a new role lifetime.
	tx.record(radio.TXResult{Generation: 2, JobID: 1, State: radio.TXSucceeded, RFAirtime: 300 * time.Millisecond})
	snapshot := tx.snapshot()
	if snapshot.Accepted != 1 || snapshot.Rejected != 1 || snapshot.Succeeded != 2 ||
		snapshot.Failed != 1 || snapshot.Unknown != 1 || snapshot.AirtimeComplete ||
		snapshot.RFAirtime != 1800*time.Millisecond || snapshot.Since != started {
		t.Fatalf("completion loss or reconnect misreported lifetime: %+v", snapshot)
	}
	partial := roleTelemetry(radio.Telemetry{}, snapshot)
	if partial.HasTXAirtime || partial.HasRXAirtime || partial.HasAirtime ||
		partial.HasNoiseFloor || partial.HasCurrentRSSI || partial.HasMCUTemperature ||
		partial.HasCounters || partial.HasBattery || partial.ErrorEvents != 0 || partial.HasErrorEvents {
		t.Fatalf("unknown completion or unavailable physical readings claimed valid: %+v", partial)
	}
	if sibling.snapshot().Rejected != 0 || sibling.snapshot().RFAirtime != 0 || !sibling.snapshot().AirtimeComplete {
		t.Fatal("one source changed another role's accounting")
	}
	_ = diagnostics.Close()
	decoder := json.NewDecoder(&logs)
	for _, state := range []string{"rejected", "failed", "unknown"} {
		var event struct {
			Role       string
			State      string
			Generation uint32
			JobID      uint32 `json:"job_id"`
			Reason     uint8
		}
		if err := decoder.Decode(&event); err != nil {
			t.Fatal(err)
		}
		if event.Role != "repeater" || event.State != state || event.Generation != 1 || event.JobID == 0 || event.Reason == 0 {
			t.Fatalf("diagnostic lost physical job context: %+v", event)
		}
	}
	if decoder.Decode(new(any)) != io.EOF {
		t.Fatal("admission or success was logged as an error")
	}
}

type blockedTXLog struct {
	bytes.Buffer
	entered chan struct{}
	release chan struct{}
	once    sync.Once
}

func (w *blockedTXLog) Write(p []byte) (int, error) {
	w.once.Do(func() { close(w.entered) })
	<-w.release
	return w.Buffer.Write(p)
}

func TestPhysicalTXDiagnosticsNeverWaitForLoggerAndExposeDrops(t *testing.T) {
	writer := &blockedTXLog{entered: make(chan struct{}), release: make(chan struct{})}
	diagnostics := newTXDiagnostics(slog.New(slog.NewJSONHandler(writer, nil)))
	t.Cleanup(func() { _ = diagnostics.Close() })
	var release sync.Once
	unblock := func() { release.Do(func() { close(writer.release) }) }
	t.Cleanup(unblock)
	tx := newRoleTX("room", true, diagnostics)
	tx.record(radio.TXResult{State: radio.TXRejected, JobID: 1, Reason: 4})
	select {
	case <-writer.entered:
	case <-time.After(3 * time.Second):
		t.Fatal("physical outcome diagnostic was not delivered")
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for job := uint32(2); job <= 201; job++ {
			tx.record(radio.TXResult{State: radio.TXRejected, JobID: job, Reason: 4})
		}
	}()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("radio ingress callback waited for blocked diagnostic output")
	}
	snapshot := tx.snapshot()
	if snapshot.Rejected != 201 || snapshot.DiagnosticDrops == 0 ||
		snapshot.LastProblem == nil || snapshot.LastProblem.JobID != 201 {
		t.Fatalf("diagnostic saturation lost role accounting or latest problem: %+v", snapshot)
	}
	unblock()
	_ = diagnostics.Close()
	if uint64(bytes.Count(writer.Bytes(), []byte{'\n'}))+snapshot.DiagnosticDrops != snapshot.Rejected {
		t.Fatal("diagnostic loss was not accounted for")
	}
}

func TestPhysicalTXSnapshotIsCoherentAcrossConcurrentCallbacks(t *testing.T) {
	diagnostics := newTXDiagnostics(slog.New(slog.NewTextHandler(io.Discard, nil)))
	defer diagnostics.Close()
	tx := newRoleTX("repeater", true, diagnostics)
	var writers sync.WaitGroup
	for range 4 {
		writers.Go(func() {
			for range 500 {
				tx.record(radio.TXResult{State: radio.TXSucceeded, RFAirtime: 7 * time.Millisecond})
			}
		})
	}
	done := make(chan struct{})
	go func() { writers.Wait(); close(done) }()
	deadline := time.NewTimer(3 * time.Second)
	defer deadline.Stop()
	for {
		snapshot := tx.snapshot()
		if snapshot.RFAirtime != time.Duration(snapshot.Succeeded)*7*time.Millisecond {
			t.Fatalf("snapshot mixed counters from different callback updates: %+v", snapshot)
		}
		select {
		case <-deadline.C:
			t.Fatal("concurrent physical outcome callbacks did not finish")
		case <-done:
			final := tx.snapshot()
			if final.Succeeded != 2000 || final.RFAirtime != 14*time.Second {
				t.Fatalf("concurrent physical completions lost: %+v", final)
			}
			return
		default:
		}
	}
}

func TestRoleTelemetryDoesNotEncodeOutcomeCountsAsNativeFaultFlags(t *testing.T) {
	snapshot := txSnapshot{Available: true, Rejected: 65535, Failed: 3, Unknown: 4}
	if got := roleTelemetry(radio.Telemetry{}, snapshot); got.ErrorEvents != 0 || got.HasErrorEvents {
		t.Fatalf("physical outcome counts became native fault flags: %+v", got)
	}
	legacy := newRoleTX("repeater", false, nil)
	if got := roleTelemetry(radio.Telemetry{}, legacy.snapshot()); got.HasTXAirtime || got.HasRXAirtime || got.HasErrorEvents {
		t.Fatal("legacy source without physical outcomes fabricated valid zero accounting")
	}
}
