package radio

import (
	"context"
	"encoding/binary"
	"errors"
	"math"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
)

func queuedConfig(phy *testPHY, owner bool) Config {
	return Config{
		Address: phy.listener.Addr().String(), Radio: hardware.RadioConfig{
			FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5,
		},
		TxPower: 20, RequireParity: true, ConfigurationOwner: owner,
	}
}

func TestRoleAnnouncementValidationWithoutPHYProfile(t *testing.T) {
	for _, cfg := range []Config{
		{RoleAnnouncement: &RoleAnnouncement{Role: RepeaterRole}},
		{RequireParity: true, RoleAnnouncement: &RoleAnnouncement{Role: ObserverRole + 1}},
	} {
		if link, err := Open(context.Background(), cfg); link != nil || err == nil {
			if link != nil {
				_ = link.Close()
			}
			t.Fatalf("invalid claim accepted without PHY profile: %v", err)
		}
	}
}

func TestQueuedRadioPreservesSchedulingAndControlLiveness(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 4)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	conn := <-phy.conns
	radio, stop := link.Radio()
	defer stop()
	tx := radio.(node.TxRadio)
	results := make(chan TXResult, 4)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	if !tx.Enqueue([]byte{0x99, 0xc0}, 2, 123*time.Millisecond) {
		t.Fatal("submission failed")
	}
	var submitted []byte
	select {
	case submitted = <-phy.jobs:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if submitted[9] != 2 || binary.LittleEndian.Uint32(submitted[10:]) != 123 ||
		string(submitted[18:]) != string([]byte{0x99, 0xc0}) {
		t.Fatalf("lost scheduling information: %x", submitted)
	}
	select {
	case result := <-results:
		if result.State != TXAccepted {
			t.Fatalf("result=%+v", result)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if tx.TxQueueLen() != 1 {
		t.Fatal("accepted job disappeared before terminal outcome")
	}
	link.mu.RLock()
	modem := link.modem
	link.mu.RUnlock()
	if err := modem.PingWait(ctx); err != nil {
		t.Fatalf("control blocked by pending TX: %v", err)
	}
	event := make([]byte, 23)
	event[0] = 1
	copy(event[1:9], submitted[1:9])
	event[9] = byte(TXSucceeded)
	binary.LittleEndian.PutUint32(event[11:], 40000)
	binary.LittleEndian.PutUint32(event[15:], 123)
	binary.LittleEndian.PutUint32(event[19:], 120)
	if err := conn.send(hardware.EncodeHardwareFrame(0, hwTXEvent, event)); err != nil {
		t.Fatal(err)
	}
	select {
	case result := <-results:
		if result.State != TXSucceeded || result.QueueWait != 40*time.Second ||
			result.RFAirtime != 123*time.Millisecond || tx.TxQueueLen() != 0 {
			t.Fatalf("incorrect physical result: %+v", result)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
}

func TestParityJoinDoesNotRetuneAndMissingCapabilityFails(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity = true
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	owner, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close()
	join, err := Open(ctx, queuedConfig(phy, false))
	if err != nil {
		t.Fatal(err)
	}
	defer join.Close()
	if phy.configurationWrites.Load() != 1 {
		t.Fatal("join retuned shared radio")
	}
	if err := join.SetSourceAirtimeFactor(ctx, 2.25); err != nil {
		t.Fatal(err)
	}
	legacy := newTestPHY(t)
	_, err = Open(ctx, queuedConfig(legacy, false))
	if !errors.Is(err, ErrParityUnavailable) {
		t.Fatalf("incapable PHY: %v", err)
	}
	if legacy.configurationWrites.Load() != 0 {
		t.Fatal("incapable modem was reconfigured")
	}
}

func TestQueuedDisconnectIsUnknownWithoutReplay(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 4)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}

	defer link.Close()
	conn := <-phy.conns
	radio, stop := link.Radio()
	defer stop()
	results := make(chan TXResult, 4)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	if !radio.(node.TxRadio).Enqueue([]byte{1}, 0, 0) {
		t.Fatal("enqueue")
	}
	<-phy.jobs
	if result := <-results; result.State != TXAccepted {
		t.Fatal(result)
	}
	conn.Close()
	select {
	case result := <-results:
		if result.State != TXUnknown {
			t.Fatal(result)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	select {
	case <-phy.jobs:
		t.Fatal("uncertain TX replayed")
	default:
	}
	select {
	case <-phy.conns:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	ticker := time.NewTicker(time.Millisecond)
	defer ticker.Stop()
	for !link.Online() {
		select {
		case <-ticker.C:
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
	}
	if phy.configurationWrites.Load() != 1 {
		t.Fatal("owner reconnect retuned PHY")
	}
}

func TestProfileMismatchStaysOfflineAndBacksOff(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity = true
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	conn := <-phy.conns
	phy.mu.Lock()
	phy.profile[0] ^= 1
	phy.mu.Unlock()
	conn.Close()
	select {
	case <-phy.conns:
	case <-ctx.Done():
		t.Fatal("initial reconnect did not occur")
	}
	select {
	case <-phy.conns:
		t.Fatal("profile mismatch caused another rapid reconnect")
	case <-time.After(3 * time.Second):
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if link.Online() || phy.configurationWrites.Load() != 1 {
		t.Fatal("mismatched reconnect went online or retuned the PHY")
	}
}

func TestSourceFactorPreservesNativeFloatPrecision(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity, phy.sourcePolicies = true, make(chan []byte, 4)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	<-phy.sourcePolicies
	factor := float32(2.0004)
	if err := link.SetSourceAirtimeFactor(ctx, float64(factor)); err != nil {
		t.Fatal(err)
	}
	submitted := <-phy.sourcePolicies
	if len(submitted) != 9 || binary.LittleEndian.Uint32(submitted[5:]) != math.Float32bits(factor) {
		t.Fatalf("source factor quantized: %x", submitted)
	}
	if wire, err := factorWire(float64(float32(0.3))); err != nil || wire != math.Float32bits(0.3) {
		t.Fatalf("float32 representation rejected: %d %v", wire, err)
	}
	if wire, err := factorWire(float64(factor)); err != nil || wire != math.Float32bits(factor) {
		t.Fatal("global factor lost native float precision")
	}
	if err := link.SetSourceAirtimeFactor(ctx, math.NaN()); err == nil {
		t.Fatal("NaN accepted")
	}
	if err := link.SetSourceAirtimeFactor(ctx, 99); err != nil {
		t.Fatalf("native one-percent duty cycle rejected: %v", err)
	}
	submitted = <-phy.sourcePolicies
	if binary.LittleEndian.Uint32(submitted[5:]) != math.Float32bits(99) {
		t.Fatalf("native factor 99 clamped: %x", submitted)
	}
	if err := link.SetSourceAirtimeFactor(ctx, 0); err != nil {
		t.Fatalf("factor zero rejected: %v", err)
	}
	submitted = <-phy.sourcePolicies
	if binary.LittleEndian.Uint32(submitted[5:]) != 0 {
		t.Fatalf("factor zero replaced with default: %x", submitted)
	}
}

func TestPHYProfileDefaultsAndNativeRuntimeRange(t *testing.T) {
	for _, tc := range []struct {
		name    string
		profile *PHYProfile
		factor  float32
	}{
		{"nil-default", nil, 1},
		{"explicit-zero", &PHYProfile{}, 0},
		{"native-one-percent", &PHYProfile{AirtimeFactor: 99}, 99},
		{"native-precision", &PHYProfile{AirtimeFactor: 2.0004}, 2.0004},
		{"native-maximum", &PHYProfile{AirtimeFactor: math.MaxFloat32}, math.MaxFloat32},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if tc.profile != nil {
				if err := tc.profile.Validate(); err != nil {
					t.Fatal(err)
				}
			}
			link := &Link{config: Config{PHYProfile: tc.profile}}
			wire, err := link.profileBytes()
			if err != nil {
				t.Fatal(err)
			}
			if len(wire) != 18 || binary.LittleEndian.Uint32(wire[11:]) != math.Float32bits(tc.factor) {
				t.Fatalf("profile factor/layout: %x", wire)
			}
		})
	}
	for _, invalid := range []float64{-1, math.NaN(), math.Inf(1), math.Inf(-1), math.MaxFloat64} {
		link := &Link{config: Config{PHYProfile: &PHYProfile{AirtimeFactor: invalid}}}
		if err := link.config.PHYProfile.Validate(); err == nil {
			t.Fatalf("public validation accepted invalid factor %v", invalid)
		}
		if _, err := link.profileBytes(); err == nil {
			t.Fatalf("accepted invalid factor %v", invalid)
		}
	}
}
