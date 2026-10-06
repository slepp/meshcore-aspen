package radio

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"log/slog"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
)

var mastSettings = PHYSettings{
	Radio:   hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5},
	TxPower: 20, Profile: PHYProfile{AirtimeFactor: 1},
}

func TestReportedPHYSpreadingAndPowerBoundaries(t *testing.T) {
	for _, tc := range []struct {
		sf, power uint8
		valid     bool
	}{
		{5, 22, true}, {6, 23, true}, {7, 20, true}, {12, 30, true},
		{4, 22, false}, {13, 22, false}, {7, 31, false},
	} {
		settings := mastSettings
		settings.Radio.SF, settings.TxPower = tc.sf, tc.power
		if err := settings.Validate(); (err == nil) != tc.valid {
			t.Fatalf("reported SF%d TX%d: %v, valid=%t", tc.sf, tc.power, err, tc.valid)
		}
		decoded, err := parsePHYSettings(settings.wire())
		if (err == nil) != tc.valid || tc.valid && decoded != settings {
			t.Fatalf("reported SF%d TX%d readback: %+v %v", tc.sf, tc.power, decoded, err)
		}
	}
}

func TestNativeNarrowBandwidthsSurviveVerifiedPHYReadback(t *testing.T) {
	for _, bandwidth := range []uint32{7810, 10420, 15630, 20830} {
		settings := mastSettings
		settings.Radio.BwHz = bandwidth
		if err := settings.Validate(); err != nil {
			t.Fatalf("native %d Hz bandwidth was rejected: %v", bandwidth, err)
		}
		decoded, err := parsePHYSettings(settings.wire())
		if err != nil || decoded != settings {
			t.Fatalf("native %d Hz bandwidth readback: %+v: %v", bandwidth, decoded, err)
		}
	}
}

// followPHY is a mast with a committed profile whose airtime model depends on
// the current spreading factor, so a stale table is detectable.
func followPHY(t *testing.T) *testPHY {
	t.Helper()
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 16)
	phy.profile, phy.configGeneration = mastSettings.wire(), 1
	phy.airtimeRequests = make(chan []byte, 2048)
	phy.airtimeResponse = func(length byte) []byte {
		phy.mu.Lock()
		sf := phy.profile[8]
		phy.mu.Unlock()
		return airtimeReply(uint32(sf)*1000 + uint32(length))
	}
	return phy
}

// The configured tuple deliberately differs from the mast: follow mode must
// not treat it as an expectation.
func followConfig(phy *testPHY, group *PHYGroup) Config {
	return Config{
		Address: phy.listener.Addr().String(),
		Radio:   hardware.RadioConfig{FreqHz: 915000000, BwHz: 250000, SF: 11, CR: 8},
		TxPower: 2, RequireParity: true, PHYTracking: PHYFollow, PHYGroup: group,
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
}

func waitUntil(t *testing.T, what string, predicate func() bool) {
	t.Helper()
	deadline := time.Now().Add(8 * time.Second)
	for !predicate() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(2 * time.Millisecond)
	}
}

func drain(ch chan []byte) int {
	n := 0
	for {
		select {
		case <-ch:
			n++
		default:
			return n
		}
	}
}

func nextResult(t *testing.T, results chan TXResult) TXResult {
	t.Helper()
	select {
	case result := <-results:
		return result
	case <-time.After(5 * time.Second):
		t.Fatal("missing TX lifecycle result")
		return TXResult{}
	}
}

func nextJob(t *testing.T, phy *testPHY, want []byte) []byte {
	t.Helper()
	select {
	case job := <-phy.jobs:
		if !bytes.Equal(job[18:], want) {
			t.Fatalf("submitted %x, want %x (replayed or reordered)", job[18:], want)
		}
		return job
	case <-time.After(5 * time.Second):
		t.Fatalf("packet %x was not submitted", want)
		return nil
	}
}

func noMoreJobs(t *testing.T, phy *testPHY) {
	t.Helper()
	select {
	case job := <-phy.jobs:
		t.Fatalf("unexpected submission (replay?): %x", job[18:])
	case <-time.After(100 * time.Millisecond):
	}
}

func effective(link *Link) (PHYState, bool) { return link.EffectivePHY() }

func settledPHY(link *Link, generation uint32) bool {
	link.mu.RLock()
	defer link.mu.RUnlock()
	return link.modem != nil && link.phy != nil &&
		link.phy.ConfigurationGeneration == generation && link.settling == nil
}

func TestFollowRetuneAndReturnKeepsConnectionWithoutReplay(t *testing.T) {
	phy := followPHY(t)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	link, err := Open(ctx, followConfig(phy, NewPHYGroup()))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	conn := <-phy.conns
	state, ok := effective(link)
	if !ok || state.ConfigurationGeneration != 1 || state.Settings != mastSettings {
		t.Fatalf("initial readback not adopted: %+v %v", state, ok)
	}
	estimate := link.AirtimeEstimator()
	if drain(phy.airtimeRequests) != 256 || estimate(10) != 7010 {
		t.Fatalf("initial airtime model: %d", estimate(10))
	}
	results := make(chan TXResult, 16)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	radio, stop := link.Radio()
	defer stop()
	tx := radio.(node.TxRadio)

	if !tx.Enqueue([]byte{0x11}, 1, 0) {
		t.Fatal("enqueue before retune")
	}
	inFlight := nextJob(t, phy, []byte{0x11})
	if result := nextResult(t, results); result.State != TXAccepted {
		t.Fatalf("pre-retune job: %+v", result)
	}

	phy.retune(t, func(p []byte) { p[8], p[10] = 9, 14 })
	if !tx.Enqueue([]byte{0x22}, 1, 0) {
		t.Fatal("enqueue after retune")
	}
	nextJob(t, phy, []byte{0x22})
	if result := nextResult(t, results); result.State != TXRejected || result.Reason != txReasonStale ||
		!bytes.Equal(result.PacketBytes(), []byte{0x22}) {
		t.Fatalf("stale submission: %+v", result)
	}
	waitUntil(t, "retune readback", func() bool {
		return settledPHY(link, 2)
	})
	state, _ = effective(link)
	if state.Settings.Radio.SF != 9 || state.Settings.TxPower != 14 || estimate(10) != 9010 ||
		drain(phy.airtimeRequests) != 256 {
		t.Fatalf("retune not followed: %+v airtime %d", state, estimate(10))
	}
	status := link.PHYStatus()
	if !status.Online || status.Tracking != PHYFollow || status.TransitionCount != 1 ||
		len(status.Transitions) != 1 || status.Transitions[0].Cause != "retune" ||
		status.Transitions[0].FromGeneration != 1 || status.Transitions[0].ToGeneration != 2 ||
		status.Transitions[0].From != mastSettings || status.Transitions[0].To.Radio.SF != 9 {
		t.Fatalf("transition status: %+v", status)
	}

	if !tx.Enqueue([]byte{0x33}, 1, 0) {
		t.Fatal("enqueue after readback")
	}
	nextJob(t, phy, []byte{0x33})
	if result := nextResult(t, results); result.State != TXAccepted {
		t.Fatalf("post-retune job: %+v", result)
	}
	// The job accepted before the retune completes on the same connection.
	event := make([]byte, 23)
	event[0] = 1
	copy(event[1:9], inFlight[1:9])
	event[9] = byte(TXSucceeded)
	if err := conn.send(hardware.EncodeHardwareFrame(0, hwTXEvent, event)); err != nil {
		t.Fatal(err)
	}
	if result := nextResult(t, results); result.State != TXSucceeded ||
		!bytes.Equal(result.PacketBytes(), []byte{0x11}) || tx.TxQueueLen() != 1 {
		t.Fatalf("pre-retune terminal outcome: %+v queue %d", result, tx.TxQueueLen())
	}

	// Temporary switch returns: the original modulation's table is reused.
	phy.retune(t, func(p []byte) { copy(p, mastSettings.wire()) })
	if !tx.Enqueue([]byte{0x44}, 1, 0) {
		t.Fatal("enqueue after return")
	}
	nextJob(t, phy, []byte{0x44})
	if result := nextResult(t, results); result.State != TXRejected {
		t.Fatalf("stale submission after return: %+v", result)
	}
	waitUntil(t, "return readback", func() bool {
		return settledPHY(link, 3)
	})
	state, _ = effective(link)
	status = link.PHYStatus()
	if state.Settings != mastSettings || estimate(10) != 7010 || drain(phy.airtimeRequests) != 0 ||
		status.TransitionCount != 2 || status.Transitions[1].To != mastSettings {
		t.Fatalf("return not followed: %+v %+v", state, status)
	}
	noMoreJobs(t, phy)
	if phy.configurationWrites.Load() != 0 || phy.generation.Load() != 1 {
		t.Fatal("follower retuned the mast or reconnected")
	}
}

func TestFollowReconnectAdoptsChangedProfileAndRollback(t *testing.T) {
	phy := followPHY(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	link, err := Open(ctx, followConfig(phy, NewPHYGroup()))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	conn := <-phy.conns
	drain(phy.airtimeRequests)
	results := make(chan TXResult, 16)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	radio, stop := link.Radio()
	defer stop()
	tx := radio.(node.TxRadio)
	if !tx.Enqueue([]byte{0x11}, 1, 0) {
		t.Fatal("enqueue")
	}
	nextJob(t, phy, []byte{0x11})
	if result := nextResult(t, results); result.State != TXAccepted {
		t.Fatal(result)
	}

	phy.retune(t, func(p []byte) {
		binary.LittleEndian.PutUint32(p, 912525000)
		p[8] = 8
	})
	conn.Close()
	if result := nextResult(t, results); result.State != TXUnknown || result.Reason != 9 {
		t.Fatalf("in-flight job across retune/reconnect: %+v", result)
	}
	conn = <-phy.conns
	waitUntil(t, "reconnect", link.Online)
	state, ok := effective(link)
	status := link.PHYStatus()
	if !ok || state.ConfigurationGeneration != 2 || state.Settings.Radio.FreqHz != 912525000 ||
		state.Settings.Radio.SF != 8 || link.AirtimeEstimator()(0) != 8000 ||
		status.TransitionCount != 1 || status.Transitions[0].Cause != "reconnect" {
		t.Fatalf("reconnect did not follow: %+v %+v", state, status)
	}
	if drain(phy.airtimeRequests) != 256 {
		t.Fatal("new modulation did not read the modem airtime model")
	}
	if !tx.Enqueue([]byte{0x22}, 1, 0) {
		t.Fatal("enqueue after reconnect")
	}
	nextJob(t, phy, []byte{0x22})
	if result := nextResult(t, results); result.State != TXAccepted {
		t.Fatalf("post-reconnect submission: %+v", result)
	}

	// A reboot ends the temporary change and restarts configuration numbering.
	phy.mu.Lock()
	copy(phy.profile, mastSettings.wire())
	phy.configGeneration = 1
	phy.mu.Unlock()
	conn.Close()
	if result := nextResult(t, results); result.State != TXUnknown {
		t.Fatalf("second in-flight job: %+v", result)
	}
	<-phy.conns
	waitUntil(t, "rollback reconnect", func() bool {
		state, ok := effective(link)
		return ok && state.Settings == mastSettings
	})
	status = link.PHYStatus()
	if status.TransitionCount != 2 || status.Effective.ConfigurationGeneration != 1 ||
		drain(phy.airtimeRequests) != 0 || link.AirtimeEstimator()(0) != 7000 {
		t.Fatalf("rollback status: %+v", status)
	}
	noMoreJobs(t, phy)
}

func TestFixedTrackingGoesOfflineOnRetune(t *testing.T) {
	phy := followPHY(t)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	cfg := followConfig(phy, NewPHYGroup())
	cfg.PHYTracking, cfg.Radio, cfg.TxPower = PHYFixed, mastSettings.Radio, mastSettings.TxPower
	link, err := Open(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	<-phy.conns
	results := make(chan TXResult, 16)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	radio, stop := link.Radio()
	defer stop()
	tx := radio.(node.TxRadio)
	if !tx.Enqueue([]byte{0x11}, 1, 0) {
		t.Fatal("enqueue")
	}
	nextJob(t, phy, []byte{0x11})
	nextResult(t, results)

	phy.retune(t, func(p []byte) { p[8] = 9 })
	if !tx.Enqueue([]byte{0x22}, 1, 0) {
		t.Fatal("enqueue after retune")
	}
	nextJob(t, phy, []byte{0x22})
	if result := nextResult(t, results); result.State != TXRejected {
		t.Fatalf("stale submission: %+v", result)
	}
	if result := nextResult(t, results); result.State != TXUnknown ||
		!bytes.Equal(result.PacketBytes(), []byte{0x11}) {
		t.Fatalf("pending job after mismatch: %+v", result)
	}
	waitUntil(t, "mismatch disconnect", func() bool { return !link.Online() })
	status := link.PHYStatus()
	if status.Effective != nil || status.Tracking != PHYFixed ||
		!strings.Contains(status.Error, ErrPHYProfileMismatch.Error()) {
		t.Fatalf("fixed mismatch status: %+v", status)
	}
	if tx.Enqueue([]byte{0x33}, 1, 0) {
		t.Fatal("offline fixed link accepted a submission")
	}
	<-phy.conns
	time.Sleep(time.Second)
	if link.Online() || phy.configurationWrites.Load() != 0 {
		t.Fatal("fixed link rejoined or retuned a mismatched mast")
	}
	select {
	case <-phy.conns:
		t.Fatal("mismatch did not select the long retry delay")
	case <-time.After(2 * time.Second):
	}
	noMoreJobs(t, phy)
}

func TestFollowRejectsInvalidAndMalformedReadback(t *testing.T) {
	profile := func(change func([]byte)) func([]byte) []byte {
		return func(response []byte) []byte {
			response = bytes.Clone(response)
			change(response[7:])
			return response
		}
	}
	for _, tc := range []struct {
		name    string
		corrupt func([]byte) []byte
		invalid bool
	}{
		{"sf13", profile(func(p []byte) { p[8] = 13 }), true},
		{"cr9", profile(func(p []byte) { p[9] = 9 }), true},
		{"bandwidth", profile(func(p []byte) { binary.LittleEndian.PutUint32(p[4:], 100000) }), true},
		{"frequency", profile(func(p []byte) { binary.LittleEndian.PutUint32(p, 0) }), true},
		{"power", profile(func(p []byte) { p[10] = 31 }), true},
		{"nan-factor", profile(func(p []byte) {
			binary.LittleEndian.PutUint32(p[11:], math.Float32bits(float32(math.NaN())))
		}), true},
		{"negative-factor", profile(func(p []byte) { binary.LittleEndian.PutUint32(p[11:], math.Float32bits(-1)) }), true},
		{"cad", profile(func(p []byte) { p[15] = 2 }), true},
		{"reason", func(r []byte) []byte { r = bytes.Clone(r); r[2] = 10; return r }, false},
		{"zero-generation", func(r []byte) []byte { r = bytes.Clone(r); clear(r[3:7]); return r }, false},
		{"short", func(r []byte) []byte { return r[:24] }, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			phy := followPHY(t)
			phy.configResponse = tc.corrupt
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			link, err := Open(ctx, followConfig(phy, NewPHYGroup()))
			if link != nil {
				link.Close()
				t.Fatal("invalid PHY readback was followed")
			}
			if tc.invalid != errors.Is(err, ErrInvalidPHYProfile) || err == nil {
				t.Fatalf("classification: %v", err)
			}
			if phy.configurationWrites.Load() != 0 || len(phy.airtimeRequests) != 0 {
				t.Fatal("invalid readback retuned or modelled the PHY")
			}
		})
	}

	phy := followPHY(t)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	link, err := Open(ctx, followConfig(phy, NewPHYGroup()))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	<-phy.conns
	drain(phy.airtimeRequests)
	phy.retune(t, func(p []byte) { p[8] = 13 })
	radio, stop := link.Radio()
	defer stop()
	radio.(node.TxRadio).Enqueue([]byte{0x11}, 1, 0)
	waitUntil(t, "invalid retune disconnect", func() bool { return !link.Online() })
	if status := link.PHYStatus(); status.Effective != nil ||
		!strings.Contains(status.Error, ErrInvalidPHYProfile.Error()) {
		t.Fatalf("invalid retune status: %+v", status)
	}
	if drain(phy.airtimeRequests) != 0 || link.AirtimeEstimator()(1) != 7001 {
		t.Fatal("invalid retune replaced the verified airtime model")
	}
}

func TestPHYGroupSharesAirtimeAndPromptsMembers(t *testing.T) {
	phy := followPHY(t)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	group := NewPHYGroup()
	first, err := Open(ctx, followConfig(phy, group))
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := Open(ctx, followConfig(phy, group))
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	if n := drain(phy.airtimeRequests); n != 256 {
		t.Fatalf("two members read %d airtime lengths, want one sweep", n)
	}
	phy.retune(t, func(p []byte) { p[8] = 10 })
	radio, stop := first.Radio()
	defer stop()
	radio.(node.TxRadio).Enqueue([]byte{0x11}, 1, 0)
	waitUntil(t, "prompted member readback", func() bool {
		state, ok := second.EffectivePHY()
		return ok && state.Settings.Radio.SF == 10
	})
	if second.AirtimeEstimator()(3) != 10003 || drain(phy.airtimeRequests) != 256 {
		t.Fatal("prompted member did not share the new airtime model")
	}
	if phy.jobs != nil && len(phy.jobs) != 1 {
		t.Fatal("member prompt submitted traffic")
	}
}

func TestPHYTrackingConfiguration(t *testing.T) {
	for _, cfg := range []Config{
		{PHYTracking: PHYFollow},
		{RequireParity: true, ConfigurationOwner: true, PHYTracking: PHYFollow},
		{RequireParity: true, PHYTracking: PHYFollow + 1},
	} {
		if link, err := Open(context.Background(), cfg); link != nil || err == nil {
			if link != nil {
				link.Close()
			}
			t.Fatalf("invalid tracking accepted: %+v", cfg)
		}
	}
}
