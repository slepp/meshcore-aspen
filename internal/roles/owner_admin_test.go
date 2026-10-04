package roles

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/meshcore-go/meshcore-go/hardware"
)

func TestOwnerAdminAllowedPolicyCommandsCommitAndPublish(t *testing.T) {
	for _, room := range []bool{false, true} {
		t.Run(map[bool]string{false: "repeater", true: "room"}[room], func(t *testing.T) {
			s, radio, _ := startRole(t, room, config(t))
			for _, command := range []string{
				"set name Owner", "set repeat on", "set path.hash.mode 2",
				"set advert.interval 60", "set flood.advert.interval 3",
			} {
				reply, err := s.OwnerAdmin(context.Background(), command)
				if err != nil || !strings.HasPrefix(reply, "OK") {
					t.Fatalf("%s: %q %v", command, reply, err)
				}
			}
			s.mu.RLock()
			p := s.state.Preferences
			s.mu.RUnlock()
			published := s.policySnapshot.Load()
			if s.Name() != "Owner" || !p.Repeat || p.PathHashMode != 2 ||
				p.LocalAdvertSeconds != 3600 || p.FloodAdvertSeconds != 10800 ||
				!published.Repeat || published.PathHashMode != 2 ||
				published.LocalAdvertSeconds != 3600 || published.FloodAdvertSeconds != 10800 {
				t.Fatalf("native setter commit/publication: state=%+v published=%+v", p, published)
			}
			for command, want := range map[string]string{
				"get name": "> Owner", "get repeat": "> on", "get path.hash.mode": "> 2",
				"get advert.interval": "> 60", "get flood.advert.interval": "> 3",
			} {
				if reply, err := s.OwnerAdmin(context.Background(), command); err != nil || reply != want {
					t.Fatalf("%s: %q %v", command, reply, err)
				}
			}
			for _, command := range []string{"set repeat off", "set advert.interval 0", "set flood.advert.interval 0"} {
				if reply, err := s.OwnerAdmin(context.Background(), command); err != nil || !strings.HasPrefix(reply, "OK") {
					t.Fatalf("%s: %q %v", command, reply, err)
				}
			}
			if p := s.policySnapshot.Load(); p.Repeat || p.LocalAdvertSeconds != 0 || p.FloodAdvertSeconds != 0 {
				t.Fatalf("disabled policies not published: %+v", p)
			}
			radio.quiet(t)
			if len(s.AccessList()) != 0 {
				t.Fatal("owner configuration granted RF administration")
			}
		})
	}
}

func TestOwnerAdminReadSnapshotsOutsideMutexWithoutWrites(t *testing.T) {
	cfg := config(t)
	var service *Service
	calls := 0
	cfg.Telemetry = func() (Telemetry, error) {
		if service.Name() != "Host" {
			t.Error("telemetry provider could not re-enter role read")
		}
		calls++
		return Telemetry{HasMCUTemperature: true, MCUTemperatureC: 25}, nil
	}
	cfg.CurrentRadio = func() (hardware.RadioConfig, bool) {
		return hardware.RadioConfig{FreqHz: 912525000, BwHz: 250000, SF: 7, CR: 5}, true
	}
	cfg.CurrentPower = func() (uint8, bool) { return 19, true }
	s, radio, _ := startRole(t, false, cfg)
	service = s
	s.mu.Lock()
	writer := s.writeJSON
	writes := 0
	s.writeJSON = func(string, any) error {
		writes++
		return errors.New("read unexpectedly tried to persist role")
	}
	s.mu.Unlock()
	for command, want := range map[string]string{
		"get name": "> Host", "get radio": "> 912.5250244,250,7,5",
		"get freq": "> 912.5250244", "get tx": "> 19",
		"board":         "Host; external KISS modem",
		"stats sensors": "schema=1 scope=modem battery_mv=unavailable mcu_temp_c=25.00",
	} {
		if reply, err := s.OwnerAdmin(context.Background(), command); err != nil || reply != want {
			t.Fatalf("%s: %q %v", command, reply, err)
		}
	}
	for _, command := range []string{"help", "help get", "help set", "help stats", "ver", "clock",
		"stats", "stats role", "stats radio", "stats signal", "stats airtime",
		"get stats", "get owner.info", "get repeat", "get path.hash.mode",
		"get advert.interval", "get flood.advert.interval", "get rxdelay",
		"get txdelay", "get direct.txdelay", "get af", "get dutycycle"} {
		if reply, err := s.OwnerAdmin(context.Background(), command); err != nil || reply == "" {
			t.Fatalf("%s: %q %v", command, reply, err)
		}
	}
	for _, command := range []string{"stats tx", "stats admission", "stats memory"} {
		if reply, err := s.OwnerAdmin(context.Background(), command); err != nil ||
			reply != "Error: unknown stats topic; use stats help" {
			t.Fatalf("unsupported topic invented a reading: %s: %q %v", command, reply, err)
		}
	}
	s.mu.Lock()
	s.writeJSON = writer
	count := writes
	s.mu.Unlock()
	if count != 0 || calls != 4 {
		t.Fatalf("read effects: writes=%d telemetry calls=%d", count, calls)
	}
	radio.quiet(t)
}

func TestOwnerAdminDeniesPrivilegedCommandsAndInjection(t *testing.T) {
	s := &Service{queue: make(chan event, 1), done: make(chan struct{})}
	for _, command := range []string{
		"", "get guest.password", "get public.key", "password secret", "set guest.password secret",
		"set prv.key secret", "setperm key 3", "reboot", "reboot extra", "advert", "advert.zerohop",
		"set radio 912,250,7,5", "set freq 912", "set tx 20", "set af 2", "set dutycycle 50",
		"region load", "set owner.info Secret", "room.post message", "clock sync", "time 123456789",
		"AB|set name Name", "set name Name\nreboot", "set name Name\x00", " set name Name",
		"set  name Name", "set repeat on extra", "set path.hash.mode -1", "set path.hash.mode 1 extra",
		"set advert.interval 1;reboot", "set name " + strings.Repeat("x", 32), "set name é",
	} {
		if reply, err := s.OwnerAdmin(context.Background(), command); reply != "" ||
			!errors.Is(err, ErrOwnerCommandDenied) || len(s.queue) != 0 {
			t.Fatalf("denied command reached worker: %q reply=%q error=%v", command, reply, err)
		}
	}
}

func TestOwnerAdminNativeBoundsRemainAuthoritative(t *testing.T) {
	s, _, _ := startRole(t, false, config(t))
	before := *s.policySnapshot.Load()
	for _, command := range []string{"set path.hash.mode 3", "set advert.interval 59",
		"set advert.interval 241", "set flood.advert.interval 2", "set flood.advert.interval 169",
		"set name Invalid:name"} {
		reply, err := s.OwnerAdmin(context.Background(), command)
		if err != nil || !strings.HasPrefix(reply, "Error") {
			t.Fatalf("native bound did not reject %s: %q %v", command, reply, err)
		}
		p := s.policySnapshot.Load()
		if p.PathHashMode != before.PathHashMode || p.LocalAdvertSeconds != before.LocalAdvertSeconds ||
			p.FloodAdvertSeconds != before.FloodAdvertSeconds || s.Name() != "Host" {
			t.Fatalf("invalid native setter mutated role: %s", command)
		}
	}
}

func TestOwnerAdminReadTimeoutIsNotUnknownMutation(t *testing.T) {
	s := &Service{queue: make(chan event, 1), done: make(chan struct{})}
	ctx, cancel := context.WithCancel(context.Background())
	result := make(chan error, 1)
	go func() {
		_, err := s.OwnerAdmin(ctx, "get name")
		result <- err
	}()
	<-s.queue
	cancel()
	if err := <-result; !errors.Is(err, context.Canceled) || errors.Is(err, ErrOwnerCommandUnknown) {
		t.Fatalf("read timeout classified as unknown mutation: %v", err)
	}
}

func TestOwnerAdminCanceledCommitIsUnknownAndNotRetried(t *testing.T) {
	s, radio, _ := startRole(t, true, config(t))
	started, release := make(chan struct{}), make(chan struct{})
	s.mu.Lock()
	writer := s.writeJSON
	writes := 0
	s.writeJSON = func(path string, data any) error {
		writes++
		close(started)
		<-release
		return writer(path, data)
	}
	s.mu.Unlock()
	ctx, cancel := context.WithCancel(context.Background())
	result := make(chan error, 1)
	go func() {
		_, err := s.OwnerAdmin(ctx, "set repeat on")
		result <- err
	}()
	<-started
	cancel()
	if err := <-result; !errors.Is(err, ErrOwnerCommandUnknown) || !errors.Is(err, context.Canceled) {
		close(release)
		t.Fatalf("canceled admitted commit lost unknown outcome: %v", err)
	}
	close(release)
	if reply, err := s.OwnerAdmin(context.Background(), "get repeat"); err != nil || reply != "> on" {
		t.Fatalf("readback after unknown outcome: %q %v", reply, err)
	}
	s.mu.Lock()
	s.writeJSON = writer
	count := writes
	s.mu.Unlock()
	if count != 1 || !s.policySnapshot.Load().Repeat {
		t.Fatalf("canceled commit replayed or failed publication: writes=%d", count)
	}
	radio.quiet(t)
}
