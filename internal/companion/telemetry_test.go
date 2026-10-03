package companion

import (
	"context"
	"errors"
	"sync/atomic"
	"testing"
	"time"

	protocol "github.com/meshcore-go/meshcore-go/companion"
)

func TestOptionalTelemetryUsesProviderValuesAndSurfacesFailures(t *testing.T) {
	var unavailable atomic.Bool
	cfg := testConfig("")
	cfg.Battery = func(context.Context) (uint16, error) {
		if unavailable.Load() {
			return 0, errors.New("modem disconnected")
		}
		return 3971, nil
	}
	core := protocol.CoreStats{BatteryMV: 3971, UptimeSecs: 12345, ErrFlags: 0x12, QueueLen: 3}
	radio := protocol.RadioStats{NoiseFloor: -116, LastRSSI: -93, LastSNR: -2.25, TxAirSecs: 14, RxAirSecs: 28}
	packets := protocol.PacketStats{PacketsRecv: 99, PacketsSent: 65, SentFlood: 60, SentDirect: 5, RecvFlood: 80, RecvDirect: 19, RecvErrors: 2}
	cfg.Stats = func(_ context.Context, kind byte) (protocol.StatsResponse, error) {
		if unavailable.Load() {
			return protocol.StatsResponse{}, errors.New("no fresh snapshot")
		}
		return protocol.StatsResponse{StatsType: kind, Core: &core, Radio: &radio, Packets: &packets}, nil
	}
	_, _, addr := startTestServer(t, testIdentity(1), cfg)
	c := testClient(t, addr)
	ctx := testContext(t)
	battery, err := c.GetBattAndStorage(ctx)
	if err != nil || battery.BatteryMilliVolts != 3971 {
		t.Fatalf("battery: %+v %v", battery, err)
	}
	for _, kind := range []byte{protocol.StatsTypeCore, protocol.StatsTypeRadio, protocol.StatsTypePackets} {
		got, err := c.GetStats(ctx, kind)
		if err != nil {
			t.Fatal(err)
		}
		switch kind {
		case protocol.StatsTypeCore:
			if got.Core == nil || *got.Core != core {
				t.Fatalf("core stats: %+v", got.Core)
			}
		case protocol.StatsTypeRadio:
			if got.Radio == nil || *got.Radio != radio {
				t.Fatalf("radio stats: %+v", got.Radio)
			}
		case protocol.StatsTypePackets:
			if got.Packets == nil || *got.Packets != packets {
				t.Fatalf("packet stats: %+v", got.Packets)
			}
		}
	}
	unavailable.Store(true)
	if _, err := c.GetBattAndStorage(ctx); err == nil {
		t.Fatal("failed battery read reported success")
	}
	if _, err := c.GetStats(ctx, protocol.StatsTypeRadio); err == nil {
		t.Fatal("failed stats read reported success")
	}
}

func TestCloseCancelsInFlightTelemetry(t *testing.T) {
	entered := make(chan struct{})
	cfg := testConfig("")
	cfg.Battery = func(ctx context.Context) (uint16, error) {
		close(entered)
		<-ctx.Done()
		return 0, ctx.Err()
	}
	s, _, addr := startTestServer(t, testIdentity(1), cfg)
	c := testClient(t, addr)
	callCtx, cancel := context.WithCancel(context.Background())
	defer cancel()
	callDone := make(chan struct{})
	go func() { defer close(callDone); _, _ = c.GetBattAndStorage(callCtx) }()
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("telemetry callback not entered")
	}
	closed := make(chan error, 1)
	go func() { closed <- s.Close() }()
	select {
	case err := <-closed:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("Close did not cancel telemetry")
	}
	cancel()
	<-callDone
}
