package app

import (
	"encoding/json"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/radio"
)

func TestApplicationStartedAtJSONDistinguishesActiveAndStopped(t *testing.T) {
	started := time.Date(2026, 9, 22, 19, 41, 58, 123456789, time.UTC)
	for _, tc := range []struct {
		status roleStatus
		want   string
	}{
		{roleStatus{ApplicationStartedAt: &started, State: "running"}, `"2026-09-22T19:41:58.123456789Z"`},
		{roleStatus{State: "stopped", ApplicationError: "source open failed"}, `null`},
	} {
		raw, err := json.Marshal(tc.status)
		if err != nil {
			t.Fatal(err)
		}
		var object map[string]json.RawMessage
		if err := json.Unmarshal(raw, &object); err != nil {
			t.Fatal(err)
		}
		if got := string(object["application_started_at"]); got != tc.want {
			t.Fatalf("application_started_at=%s, want %s", got, tc.want)
		}
	}
}

func TestRadioScoreUsesConfiguredSpreadingFactorAndRealMeasurements(t *testing.T) {
	packet := &meshcore.Packet{SNR: -8, RSSI: -100, HasSignalInfo: true}
	for _, tc := range []struct {
		sf   uint8
		want float32
	}{
		{5, 0},
		{6, 0},
		{7, 0},
		{12, 0.9},
	} {
		score, err := radioScore(hardware.RadioConfig{SF: tc.sf}, nil)(packet, 64, 0)
		if err != nil || score != tc.want {
			t.Fatalf("SF%d: score=%v err=%v, want %v", tc.sf, score, err, tc.want)
		}
	}
	var effective *radio.PHYState
	score := radioScore(hardware.RadioConfig{SF: 7}, func() (radio.PHYState, bool) {
		if effective == nil {
			return radio.PHYState{}, false
		}
		return *effective, true
	})
	if got, _ := score(packet, 64, 0); got != 0 {
		t.Fatalf("unknown effective PHY did not fall back to configured SF: %v", got)
	}
	effective = &radio.PHYState{Settings: radio.PHYSettings{Radio: hardware.RadioConfig{SF: 12}}}
	if got, _ := score(packet, 64, 0); got != 0.9 {
		t.Fatalf("score did not follow the effective SF: %v", got)
	}
	packet.HasSignalInfo = false
	if _, err := radioScore(hardware.RadioConfig{SF: 7}, nil)(packet, 64, 0); err == nil {
		t.Fatal("missing signal metadata was treated as an RF measurement")
	}
}
