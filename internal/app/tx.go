package app

import (
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/roles"
)

type txProblem struct {
	State      string        `json:"state"`
	Generation uint32        `json:"generation"`
	JobID      uint32        `json:"job_id"`
	Reason     uint8         `json:"reason"`
	QueueWait  time.Duration `json:"queue_wait_ns"`
	RFAirtime  time.Duration `json:"reported_rf_airtime_ns"`
}

type txSnapshot struct {
	Since           time.Time     `json:"since"`
	Available       bool          `json:"available"`
	Accepted        uint64        `json:"accepted"`
	Rejected        uint64        `json:"rejected"`
	Succeeded       uint64        `json:"succeeded"`
	Failed          uint64        `json:"failed"`
	Unknown         uint64        `json:"unknown"`
	RFAirtime       time.Duration `json:"reported_rf_airtime_ns"`
	AirtimeComplete bool          `json:"airtime_complete"`
	DiagnosticDrops uint64        `json:"diagnostic_drops"`
	LastProblem     *txProblem    `json:"last_problem,omitempty"`
}

type txDiagnostic struct {
	role    string
	problem txProblem
}

type txDiagnostics struct {
	queue chan txDiagnostic
	done  chan struct{}
	once  sync.Once
}

func newTXDiagnostics(logger *slog.Logger) *txDiagnostics {
	d := &txDiagnostics{queue: make(chan txDiagnostic, 64), done: make(chan struct{})}
	go func() {
		defer close(d.done)
		for event := range d.queue {
			p := event.problem
			logger.Error("physical TX outcome", "role", event.role, "state", p.State,
				"generation", p.Generation, "job_id", p.JobID, "reason", p.Reason,
				"queue_wait", p.QueueWait, "reported_rf_airtime", p.RFAirtime)
		}
	}()
	return d
}

// Close drains admitted diagnostics after every source link has been retired.
func (d *txDiagnostics) Close() error {
	d.once.Do(func() { close(d.queue) })
	<-d.done
	return nil
}

type roleTX struct {
	role        string
	diagnostics *txDiagnostics
	state       atomic.Pointer[txSnapshot]
}

func newRoleTX(role string, available bool, diagnostics *txDiagnostics) *roleTX {
	tx := &roleTX{role: role, diagnostics: diagnostics}
	tx.state.Store(&txSnapshot{Since: time.Now(), Available: available, AirtimeComplete: available})
	return tx
}

func (tx *roleTX) snapshot() txSnapshot { return *tx.state.Load() }

// Radio already correlates terminal events by source generation/job. Keep one
// coherent lifetime snapshot, with no ingress-side logging or job cache.
func (tx *roleTX) record(result radio.TXResult) {
	var problem *txProblem
	switch result.State {
	case radio.TXRejected, radio.TXFailed, radio.TXUnknown:
		state := "rejected"
		if result.State == radio.TXFailed {
			state = "failed"
		} else if result.State == radio.TXUnknown {
			state = "unknown"
		}
		problem = &txProblem{
			State: state, Generation: result.Generation, JobID: result.JobID,
			Reason: result.Reason, QueueWait: result.QueueWait, RFAirtime: result.RFAirtime,
		}
	}
	dropped := false
	if problem != nil {
		select {
		case tx.diagnostics.queue <- txDiagnostic{tx.role, *problem}:
		default:
			dropped = true
		}
	}
	for {
		old := tx.state.Load()
		next := *old
		switch result.State {
		case radio.TXAccepted:
			next.Accepted++
		case radio.TXRejected:
			next.Rejected++
		case radio.TXSucceeded:
			next.Succeeded++
			next.RFAirtime += result.RFAirtime
		case radio.TXFailed:
			next.Failed++
			next.RFAirtime += result.RFAirtime
		case radio.TXUnknown:
			next.Unknown++
			next.AirtimeComplete = false
		}
		if problem != nil {
			next.LastProblem = problem
		}
		if dropped {
			next.DiagnosticDrops++
		}
		if tx.state.CompareAndSwap(old, &next) {
			return
		}
	}
}

func roleTelemetry(physical radio.Telemetry, tx txSnapshot) roles.Telemetry {
	return roles.Telemetry{
		BatteryMilliVolts: physical.BatteryMilliVolts, HasBattery: physical.HasBattery,
		NoiseFloorDBm: physical.NoiseFloorDBm, HasNoiseFloor: physical.HasNoiseFloor,
		CurrentRSSIDBm: physical.CurrentRSSIDBm, HasCurrentRSSI: physical.HasCurrentRSSI,
		MCUTemperatureC: physical.MCUTemperatureC, HasMCUTemperature: physical.HasMCUTemperature,
		Counters: physical.Counters, HasCounters: physical.HasCounters,
		TXAirtime: tx.RFAirtime, HasTXAirtime: tx.AirtimeComplete,
	}
}
