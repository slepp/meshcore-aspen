package roles

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
)

func TestStatsCLIUsesModemSnapshotAndPreservesPrefix(t *testing.T) {
	for _, room := range []bool{false, true} {
		cfg := config(t)
		var calls atomic.Int32
		cfg.Telemetry = func() (Telemetry, error) {
			calls.Add(1)
			return Telemetry{HasMCUTemperature: true, MCUTemperatureC: -12.5}, nil
		}
		s, radio, id := startRole(t, room, cfg)
		admin := fixedID(2)
		loggedIn(t, s, radio, admin, id, room, 1, "admin")
		radio.inject(t, textWire(t, admin, id, 2, 4, "ab|stats  sensors"), false)
		reply := string(cstring(decrypted(t, radio.next(t), admin, id)[5:]))
		if reply != "ab|schema=1 scope=modem battery_mv=unavailable mcu_temp_c=-12.50" || calls.Load() != 1 {
			t.Fatalf("stats did not use its hardware snapshot: %q calls=%d", reply, calls.Load())
		}
		radio.inject(t, textWire(t, admin, id, 3, 4, "stats role"), false)
		reply = string(cstring(decrypted(t, radio.next(t), admin, id)[5:]))
		if !strings.HasPrefix(reply, "schema=1 scope=role uptime_s=") || calls.Load() != 1 {
			t.Fatalf("role stats queried physical telemetry: %q calls=%d", reply, calls.Load())
		}
	}
}

func TestStatsUnavailableAndMaximumCounters(t *testing.T) {
	s := &Service{started: time.Now()}
	if got := s.statsCommand("sensors", Telemetry{}, nil); got != "Error: modem statistics snapshot unavailable" {
		t.Fatal(got)
	}
	s.cfg.Telemetry = func() (Telemetry, error) { return Telemetry{}, nil }
	if got := s.statsCommand("sensors", Telemetry{}, errors.New("offline")); got != "Error: modem statistics snapshot unavailable" {
		t.Fatal(got)
	}
	if got := s.statsCommand("sensors", Telemetry{}, nil); got != "schema=1 scope=modem battery_mv=unavailable mcu_temp_c=unavailable" {
		t.Fatal(got)
	}
	if got := s.statsCommand("sensors", Telemetry{HasMCUTemperature: true, MCUTemperatureC: float32(math.NaN())}, nil); !strings.HasPrefix(got, "Error:") {
		t.Fatal(got)
	}
	for _, topic := range []string{"radio", "signal", "airtime"} {
		got := s.statsCommand(topic, Telemetry{
			HasCounters: true, Counters: hardware.FirmwareStats{PacketsRecv: math.MaxUint32, PacketsSent: math.MaxUint32, PacketsErrors: math.MaxUint32},
			HasAirtime: true, TXAirtime: time.Duration(math.MaxInt64), RXAirtime: time.Duration(math.MaxInt64),
			HasNoiseFloor: true, NoiseFloorDBm: math.MinInt16, HasCurrentRSSI: true, CurrentRSSIDBm: math.MinInt8,
		}, nil)
		if len(got) > 130 || !strings.HasPrefix(got, "schema=1 scope=modem ") {
			t.Fatalf("%s stats exceed RF capacity: %q", topic, got)
		}
	}
	if got := s.statsCommand("reset", Telemetry{}, nil); got != "Error: unknown stats topic; use stats help" {
		t.Fatal(got)
	}
}

func TestNativeLPPSinglePrecisionQuantization(t *testing.T) {
	for _, tc := range []struct {
		battery     uint16
		temperature float32
		want        []byte
	}{
		{4020, 21.3, []byte{1, 0x74, 1, 0x92, 1, 0x67, 0, 0xd5}},
		{1150, -21.3, []byte{1, 0x74, 0, 0x73, 1, 0x67, 0xff, 0x2b}},
		{0, -3276.8, []byte{1, 0x74, 0, 0, 1, 0x67, 0x80, 0}},
	} {
		got, err := telemetryPayload(Telemetry{HasBattery: true, BatteryMilliVolts: tc.battery, HasMCUTemperature: true, MCUTemperatureC: tc.temperature})
		if err != nil || !bytes.Equal(got, tc.want) {
			t.Fatalf("native Cayenne float scaling: %x (%v), want %x", got, err, tc.want)
		}

	}
}

func TestRoleStatusDoesNotCountLocalReflectionAsRFReception(t *testing.T) {
	s, r, id := startRole(t, false, config(t))
	peer := fixedID(3)
	app, err := (&meshcore.AdvertAppData{Type: "REPEATER", Name: "Nearby"}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	advert := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: 123, RawAppData: app}
	advert.SignWith(peer)
	payload, err := advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	raw := append([]byte{0x12, 0}, payload...)
	r.inject(t, raw, true)
	if s.received.Load() != 0 {
		t.Fatal("local modem reflection counted as RF reception")
	}
	r.inject(t, raw, false)
	admin := fixedID(2)
	loggedIn(t, s, r, admin, id, false, 1, "admin")
	r.inject(t, peerWire(t, 0, admin, id, []byte{2, 0, 0, 0, 1}), false)
	body := decrypted(t, r.next(t), admin, id)
	if got := binary.LittleEndian.Uint32(body[12:16]); got != 3 {
		t.Fatalf("status reported %d RF receives including local delivery, want 3", got)
	}
}

func TestModemSnapshotStatusAndTelemetryWireFields(t *testing.T) {
	for _, room := range []bool{false, true} {
		name := "repeater"
		if room {
			name = "room"
		}
		t.Run(name, func(t *testing.T) {
			cfg := config(t)
			cfg.Telemetry = func() (Telemetry, error) {
				return Telemetry{
					BatteryMilliVolts: 4200, HasBattery: true,
					NoiseFloorDBm: -111, HasNoiseFloor: true,
					CurrentRSSIDBm: -95, HasCurrentRSSI: true,
					MCUTemperatureC: -12.5, HasMCUTemperature: true,
					Counters:    hardware.FirmwareStats{PacketsRecv: 400, PacketsSent: 250, PacketsErrors: 7},
					HasCounters: true,
				}, nil
			}
			s, r, id := startRole(t, room, cfg)
			client := fixedID(2)
			loggedIn(t, s, r, client, id, room, 1, "room")
			r.inject(t, peerWire(t, 0, client, id, []byte{2, 0, 0, 0, 1}), false)
			body := decrypted(t, r.next(t), client, id)
			if !bytes.Equal(body[4:6], []byte{0x68, 0x10}) ||
				!bytes.Equal(body[8:10], []byte{0x91, 0xff}) ||
				int16(binary.LittleEndian.Uint16(body[10:])) != -73 ||
				!bytes.Equal(body[12:20], []byte{0x90, 1, 0, 0, 0xfa, 0, 0, 0}) {
				t.Fatalf("status did not use genuine snapshot/packet metadata: %x", body)
			}
			if !room && !bytes.Equal(body[56:60], []byte{7, 0, 0, 0}) {
				t.Fatalf("firmware RX errors missing: %x", body)
			}
			r.inject(t, peerWire(t, 0, client, id, []byte{3, 0, 0, 0, 3, 0, 0, 0, 0}), false)
			body = decrypted(t, r.next(t), client, id)
			// Firmware Cayenne LPP: channel 1, voltage in 0.01 V; channel 1,
			// temperature in signed 0.1 C, both big-endian. Native float32
			// scaling truncates 4200 mV to 419 centivolts.
			want := []byte{3, 0, 0, 0, 1, 0x74, 1, 0xa3, 1, 0x67, 0xff, 0x83}
			if !bytes.Equal(body[:len(want)], want) {
				t.Fatalf("telemetry ABI %x want %x", body, want)
			}
		})
	}
}

func TestTelemetrySnapshotRunsOutsideReceiveHandlerAndStateLock(t *testing.T) {
	cfg := config(t)
	entered, release := make(chan struct{}), make(chan struct{})
	var releaseOnce sync.Once
	unblock := func() { releaseOnce.Do(func() { close(release) }) }
	cfg.Telemetry = func() (Telemetry, error) {
		close(entered)
		<-release
		return Telemetry{HasBattery: true, BatteryMilliVolts: 3900}, nil
	}
	s, r, id := startRole(t, false, cfg)
	t.Cleanup(unblock)
	client := fixedID(2)
	loggedIn(t, s, r, client, id, false, 1, "admin")
	r.inject(t, peerWire(t, 0, client, id, []byte{2, 0, 0, 0, 1}), false)
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("snapshot callback not invoked")
	}
	received := make(chan struct{})
	go func() {
		r.inject(t, textWire(t, client, id, 3, 4, "board"), false)
		close(received)
	}()
	select {
	case <-received:
	case <-time.After(time.Second):
		unblock()
		t.Fatal("snapshot callback blocked peer receive/state lookup")
	}
	unblock()
	status := decrypted(t, r.next(t), client, id)
	if binary.LittleEndian.Uint16(status[4:]) != 3900 {
		t.Fatalf("status %x", status)
	}
	reply := decrypted(t, r.next(t), client, id)
	if string(cstring(reply[5:])) != "Host; external KISS modem" {
		t.Fatalf("queued command not handled: %x", reply)
	}
}

func TestTelemetryPartialErrorAndValidZeroValues(t *testing.T) {
	cfg := config(t)
	wantErr := errors.New("temperature cache unavailable")
	errs := make(chan error, 2)
	cfg.ErrorHandler = func(err error) { errs <- err }
	cfg.Telemetry = func() (Telemetry, error) { return Telemetry{HasBattery: true}, wantErr }
	s, r, id := startRole(t, true, cfg)
	client := fixedID(2)
	loggedIn(t, s, r, client, id, true, 1, "room")
	r.inject(t, peerWire(t, 0, client, id, []byte{2, 0, 0, 0, 3, 0, 0, 0, 0}), false)
	body := decrypted(t, r.next(t), client, id)
	if !bytes.Equal(body[:8], []byte{2, 0, 0, 0, 1, 0x74, 0, 0}) || !bytes.Equal(body[8:], make([]byte, len(body)-8)) {
		t.Fatalf("valid zero voltage omitted or missing sensor fabricated: %x", body)
	}
	select {
	case err := <-errs:
		if !errors.Is(err, wantErr) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("partial snapshot error hidden")
	}
}

func TestInvalidTemperatureHasNoSuccessResponse(t *testing.T) {
	cfg := config(t)
	errs := make(chan error, 2)
	cfg.ErrorHandler = func(err error) { errs <- err }
	cfg.Telemetry = func() (Telemetry, error) {
		return Telemetry{HasMCUTemperature: true, MCUTemperatureC: float32(math.NaN())}, nil
	}
	s, r, id := startRole(t, true, cfg)
	client := fixedID(2)
	loggedIn(t, s, r, client, id, true, 1, "room")
	r.inject(t, peerWire(t, 0, client, id, []byte{2, 0, 0, 0, 3, 0, 0, 0, 0}), false)
	select {
	case <-errs:
	case <-time.After(time.Second):
		t.Fatal("invalid hardware reading not reported")
	}
	r.quiet(t)
}
