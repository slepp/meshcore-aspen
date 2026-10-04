package app

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/observer"
	"meshcore.local/meshcore/internal/radio"
)

func TestOwnerAutoFallsBackOnOldFirmwareWithoutReusingDiscoverySocket(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{}
	cfg.RadioSession = "auto"
	mast := newAuthorityMast(t, cfg)
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if err != nil {
		t.Fatal(err)
	}

	defer link.Close()
	if session != nil {
		t.Fatal("old firmware was treated as a multiplexed session")
	}

	mast.mu.Lock()
	accepted := len(mast.active)
	mast.mu.Unlock()
	if accepted != 2 {
		t.Fatalf("auto did not retire unsupported negotiation socket: %d", accepted)
	}
	if evidence, err := cfg.checkRadioCapacity(context.Background(), link, logger); err != nil || evidence.external != 8 {
		t.Fatalf("legacy per-role CAPACITY changed after fallback: %+v, %v", evidence, err)
	}

	cfg.RadioSession = "required"
	link, session, err = cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if link != nil || session != nil || !errors.Is(err, radio.ErrSubportsUnavailable) {
		t.Fatalf("required subports silently fell back: %v", err)
	}
}

func TestOwnerAutoFallsBackOnSilentInitialProbeWithReason(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{}
	cfg.RadioSession = "auto"
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.silentExtended = true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	var logs bytes.Buffer
	logger := slog.New(slog.NewTextHandler(&logs, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	mast.mu.Lock()
	accepted := len(mast.active)
	mast.mu.Unlock()
	if session != nil || accepted != 2 || !strings.Contains(logs.String(), radio.ErrSubportsProbeTimeout.Error()) {
		t.Fatalf("silent initial probe did not fall back on a new socket with a reason: session=%v accepts=%d log=%q",
			session != nil, accepted, logs.String())
	}
}

func TestOwnerUsesThreePortSessionWhenCapacityFitsRoles(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{}
	cfg.RadioSession = "auto"
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.extendedSubports = true
	mast.subports = 3
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	var logs bytes.Buffer
	logger := slog.New(slog.NewTextHandler(&logs, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	mast.mu.Lock()
	accepted := len(mast.active)
	mast.mu.Unlock()
	if session == nil || accepted != 1 || cfg.sessionMinPorts() != 1 {
		t.Fatalf("three-port auto mode did not use negotiated session: session=%v accepts=%d log=%q",
			session != nil, accepted, logs.String())
	}
	capacity, err := cfg.checkRadioCapacity(context.Background(), sessionCapacityReader{session}, logger)
	if err != nil || capacity.ports != 3 || len(cfg.sessionRolePorts(capacity.ports)) != 0 {
		t.Fatalf("three-port session capacity or role plan: %+v, %v", capacity, err)
	}
	if err := link.Close(); err != nil {
		t.Fatal(err)
	}
	cfg.RadioSession = "required"
	link, session, err = cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if err != nil || session == nil {
		t.Fatalf("required mode rejected sufficient ports: %v", err)
	}
	defer link.Close()
}

func TestSessionFourSlotsIncludeOwnerAndOverflowRoleUsesDirectSocket(t *testing.T) {
	cfg := DefaultConfig()
	cfg.RadioSession = "required"
	cfg.PHYAuthority = "modem"
	cfg.BotCompanionListen = "127.0.0.1:5001"
	if cfg.sessionMinPorts() != 4 || cfg.sessionPhysicalSources() != 2 || cfg.reservedClients(true) != 4 {
		t.Fatalf("four-slot radio cannot fit host roles plus two bot clients: %+v", cfg)
	}
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.extendedSubports = true
	mast.externalSlots = 4
	mast.subports = 4
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	cfg.sessionActive = true
	capacity, err := cfg.checkRadioCapacity(context.Background(), sessionCapacityReader{session}, logger)
	if err != nil || capacity.ports != 4 {
		t.Fatalf("actual four-port capacity: %+v, %v", capacity, err)
	}
	if ports := cfg.sessionRolePorts(capacity.ports); len(ports) != 3 || ports["bot_companion"] != 0 {
		t.Fatalf("overflow role was incorrectly assigned port 0 or omitted: %v", ports)
	}
}

func TestSessionThreePortsPreserveSelectedRolesAndDirectBotCapacity(t *testing.T) {
	cfg := DefaultConfig()
	cfg.RadioSession = "required"
	cfg.PHYAuthority = "modem"
	cfg.EnabledRoles = []string{"repeater", "room", "bot"}
	if cfg.sessionMinPorts() != 2 {
		t.Fatalf("three sources and two direct bot clients require only two logical ports: %d", cfg.sessionMinPorts())
	}
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.extendedSubports, mast.externalSlots, mast.subports = true, 4, 3
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	cfg.sessionActive = true
	capacity, err := cfg.checkRadioCapacity(context.Background(), sessionCapacityReader{session}, logger)
	if err != nil || capacity.ports != 3 || capacity.external != 4 {
		t.Fatalf("three-port role capacity: %+v, %v", capacity, err)
	}
	if ports := cfg.sessionRolePorts(capacity.ports); len(ports) != 2 || ports["repeater"] != 1 || ports["room"] != 2 {
		t.Fatalf("selected roles not mapped to actual three-port response: %v", ports)
	}
}

func TestHostRunsSelectedRolesOnThreePortRadioWithoutOpeningOverflowSockets(t *testing.T) {
	for _, roles := range [][]string{
		{"repeater", "companion"},
		{"repeater", "room"},
		{"repeater", "observer"},
		{"repeater", "room", "bot"},
	} {
		t.Run(strings.Join(roles, "-"), func(t *testing.T) {
			cfg := DefaultConfig()
			cfg.RadioSession = "required"
			cfg.PHYAuthority = "modem"
			cfg.EnabledRoles = roles
			cfg.RoomPassword = "session-test-room-credential"
			cfg.StateDir = t.TempDir()
			cfg.BotListen = "127.0.0.1:0"
			cfg.CompanionListen = "127.0.0.1:0"
			logger := slog.New(slog.NewTextHandler(io.Discard, nil))
			if cfg.RoleEnabled("observer") {
				broker, err := observer.StartBroker(observer.BrokerConfig{Address: "127.0.0.1:0", Logger: logger})
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { _ = broker.Close() })
				cfg.MQTT.BrokerListen = ""
				cfg.MQTT.URL = "tcp://" + broker.Addr().String()
			}
			mast := newAuthorityMast(t, cfg)
			mast.mu.Lock()
			mast.extendedSubports, mast.externalSlots, mast.subports = true, 4, 3
			mast.mu.Unlock()
			cfg.RadioAddress = mast.listener.Addr().String()
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			cfg.StatusListen = listener.Addr().String()
			if err := listener.Close(); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithCancel(context.Background())
			outcome := make(chan error, 1)
			go func() { outcome <- Run(ctx, cfg, logger) }()
			t.Cleanup(func() {
				cancel()
				select {
				case err := <-outcome:
					if err != nil {
						t.Errorf("host shutdown: %v", err)
					}
				case <-time.After(5 * time.Second):
					t.Error("host did not shut down")
				}
			})
			client := &http.Client{Timeout: time.Second}
			waitAuthority(t, func() bool {
				response, err := client.Get("http://" + cfg.StatusListen + "/status")
				if err != nil {
					return false
				}
				defer response.Body.Close()
				var status map[string]roleStatus
				if err := json.NewDecoder(response.Body).Decode(&status); err != nil {
					return false
				}
				for _, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
					want := "disabled"
					if cfg.RoleEnabled(role) {
						want = "running"
					}
					if status[role].State != want {
						return false
					}
				}
				if cfg.RoleEnabled("bot") && status["bot"].ApplicationKind != "kiss_proxy" {
					return false
				}
				ready, err := client.Get("http://" + cfg.StatusListen + "/readyz")
				if err != nil {
					return false
				}
				defer ready.Body.Close()
				return ready.StatusCode == http.StatusOK
			})
			mast.mu.Lock()
			sockets := len(mast.active)
			mast.mu.Unlock()
			if sockets != 1 {
				t.Fatalf("roles %v opened %d physical sockets, want one aggregate socket", roles, sockets)
			}
		})
	}
}

func TestSessionInsufficientPortsFailsWhenOverflowCannotFit(t *testing.T) {
	cfg := DefaultConfig()
	cfg.RadioSession = "required"
	cfg.PHYAuthority = "modem"
	cfg.BotCompanionListen = "127.0.0.1:5001"
	if cfg.sessionMinPorts() != 4 {
		t.Fatalf("five host sources and two direct clients require all four logical ports: %d", cfg.sessionMinPorts())
	}
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.extendedSubports, mast.externalSlots, mast.subports = true, 4, 3
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if link != nil || session != nil || !errors.Is(err, radio.ErrSubportsInsufficient) {
		t.Fatalf("three logical ports falsely admitted four physical sockets: %v", err)
	}
}

func TestAutoRefusesPerRoleFallbackWhenOptionalSourcesExceedPhysicalSlots(t *testing.T) {
	cfg := DefaultConfig()
	cfg.RadioSession = "auto"
	cfg.PHYAuthority = "modem"
	cfg.BotCompanionListen = "127.0.0.1:5001"
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.extendedSubports, mast.externalSlots, mast.subports = true, 4, 3
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	link, session, err := cfg.openOwnerRadio(context.Background(), cfg.configurationLink("controller", logger, nil, nil), logger)
	if link != nil || session != nil || !errors.Is(err, radio.ErrSubportsInsufficient) ||
		!strings.Contains(err.Error(), "per-role fallback needs 7 connections") {
		t.Fatalf("overbooked auto mode silently reopened a direct session: %v", err)
	}
	mast.mu.Lock()
	opened := len(mast.active)
	mast.mu.Unlock()
	if opened != 1 {
		t.Fatalf("overbooked fallback opened %d physical connections, want only the refused probe", opened)
	}
}

func TestAutoDowngradeFailsHostThenStartsLegacyWithoutServiceConfigChange(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{}
	cfg.StateDir = t.TempDir()
	cfg.RadioSession = "auto"
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.extendedSubports = true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}

	cfg.StatusListen = listener.Addr().String()
	_ = listener.Close()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	outcome := make(chan error, 1)
	go func() { outcome <- Run(ctx, cfg, logger) }()
	readyURL := "http://" + cfg.StatusListen + "/readyz"
	waitReady := func() {
		t.Helper()
		deadline := time.After(5 * time.Second)
		for {
			response, err := (&http.Client{Timeout: time.Second}).Get(readyURL)
			if err == nil {
				_ = response.Body.Close()
				if response.StatusCode == http.StatusOK {
					return
				}
			}
			select {
			case <-deadline:
				t.Fatal("host did not become ready")
			case <-time.After(10 * time.Millisecond):
			}
		}
	}
	waitReady()
	mast.mu.Lock()
	if len(mast.active) != 1 {
		mast.mu.Unlock()
		t.Fatal("initial virtual session did not use exactly one socket")
	}
	mast.extendedSubports = false
	conn := mast.active[0]
	mast.mu.Unlock()
	_ = conn.Close()
	select {
	case err := <-outcome:
		if !errors.Is(err, radio.ErrSubportsUnavailable) || !strings.Contains(err.Error(), "supervisor restart required") {
			t.Fatalf("host did not fail-stop on runtime subport loss: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("host remained running after firmware downgrade")
	}
	fallbackCtx, fallbackCancel := context.WithCancel(context.Background())
	fallback := make(chan error, 1)
	go func() { fallback <- Run(fallbackCtx, cfg, logger) }()
	waitReady()
	mast.mu.Lock()
	accepted := len(mast.active)
	mast.mu.Unlock()
	if accepted < 4 {
		t.Fatalf("fresh host did not retire discovery socket before legacy connection: %d total accepts", accepted)
	}
	fallbackCancel()
	select {
	case err := <-fallback:
		if err != nil {
			t.Fatalf("legacy host shutdown: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("legacy host did not shut down")
	}
}

func TestSessionCompanionCommandsRecoverAfterChildPortRetirement(t *testing.T) {
	for _, tc := range []struct {
		name, role string
		reset      bool
	}{
		{"CMD19 reboot", "companion", false},
		{"CMD51 factory reset", "companion", true},
		{"bot_companion CMD19 reboot", "bot_companion", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := DefaultConfig()
			cfg.EnabledRoles = []string{"companion"}
			cfg.CompanionFactoryReset = true
			cfg.CompanionListen = "127.0.0.1:0"
			if tc.role == "bot_companion" {
				cfg.EnabledRoles = []string{}
				cfg.BotCompanionListen = "127.0.0.1:0"
			}
			cfg.StateDir = t.TempDir()
			cfg.RadioSession = "required"
			mast := newAuthorityMast(t, cfg)
			mast.mu.Lock()
			mast.extendedSubports = true
			mast.mu.Unlock()
			cfg.RadioAddress = mast.listener.Addr().String()
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			cfg.StatusListen = listener.Addr().String()
			_ = listener.Close()
			logger := slog.New(slog.NewTextHandler(io.Discard, nil))
			ctx, cancel := context.WithCancel(context.Background())
			outcome := make(chan error, 1)
			go func() { outcome <- Run(ctx, cfg, logger) }()
			t.Cleanup(func() {
				cancel()
				select {
				case err := <-outcome:
					if err != nil {
						t.Errorf("host shutdown: %v", err)
					}
				case <-time.After(5 * time.Second):
					t.Error("host did not shut down")
				}
			})
			client := &http.Client{Timeout: time.Second}
			statusURL := "http://" + cfg.StatusListen + "/status"
			readReady := func() (map[string]roleStatus, bool) {
				response, err := client.Get("http://" + cfg.StatusListen + "/readyz")
				if err != nil {
					return nil, false
				}
				_ = response.Body.Close()
				if response.StatusCode != http.StatusOK {
					return nil, false
				}
				response, err = client.Get(statusURL)
				if err != nil {
					return nil, false
				}
				defer response.Body.Close()
				var status map[string]roleStatus
				if err := json.NewDecoder(response.Body).Decode(&status); err != nil {
					return nil, false
				}
				return status, status[tc.role].State == "running"
			}
			var before map[string]roleStatus
			waitAuthority(t, func() bool {
				var ready bool
				before, ready = readReady()
				return ready
			})
			address := before[tc.role].Endpoint
			key := before[tc.role].PublicKey
			conn := lifecycleDial(t, address)
			command := (protocol.RebootCommand{}).ToBytes()
			if tc.reset {
				command = (protocol.FactoryResetCommand{}).ToBytes()
			}
			lifecycleSend(t, conn, command)
			lifecycleDisconnected(t, conn)
			var after map[string]roleStatus
			waitAuthority(t, func() bool {
				var ready bool
				after, ready = readReady()
				return ready && after[tc.role].ApplicationStartedAt != nil &&
					after[tc.role].ApplicationStartedAt.After(*before[tc.role].ApplicationStartedAt)
			})
			if after[tc.role].ApplicationError != "" || after[tc.role].Endpoint != address ||
				after["controller"].SharedPHY == nil || !after["controller"].SharedPHY.Valid {
				t.Fatalf("companion or sibling owner failed to recover: companion=%+v controller=%+v",
					after[tc.role], after["controller"])
			}
			if (after[tc.role].PublicKey != key) != tc.reset {
				t.Fatal("reboot/reset changed the wrong role identity")
			}
			reconnected := lifecycleDial(t, address)
			if got := lifecycleCommand(t, reconnected, []byte{protocol.CmdGetStats, 0}); len(got) == 0 {
				t.Fatal("restarted companion listener stopped serving commands")
			}
			mast.mu.Lock()
			connections := len(mast.active)
			mast.mu.Unlock()
			if connections != 2 {
				t.Fatalf("one lifecycle request triggered %d physical connections, want 2", connections)
			}
		})
	}
}
