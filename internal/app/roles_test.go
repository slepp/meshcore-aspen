package app

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/roles"
	"meshcore.local/meshcore/internal/state"
)

type routingRadio struct {
	lifecycleRadio
	mu       sync.Mutex
	handler  func(*meshcore.Packet)
	output   chan []byte
	closed   atomic.Bool
	closeErr error
}

func (r *routingRadio) SetDataHandler(handler func(*meshcore.Packet)) {
	r.mu.Lock()
	r.handler = handler
	r.mu.Unlock()
}
func (r *routingRadio) Enqueue(raw []byte, _ uint8, _ time.Duration) bool {
	if r.closed.Load() {
		return false
	}
	select {
	case r.output <- bytes.Clone(raw):
		return true
	default:
		return false
	}
}
func (r *routingRadio) Close() error { r.closed.Store(true); return r.closeErr }
func (r *routingRadio) inject(p *meshcore.Packet) {
	r.mu.Lock()
	handler := r.handler
	r.mu.Unlock()
	p.HasSignalInfo, p.SNR, p.RSSI = true, 8, -70
	handler(p)
}

type routingLink struct {
	lifecycleLink
	nodeRadio routingRadio
	mu        sync.Mutex
	handlers  []func(radio.TXResult)
}

func newRoutingLink() *routingLink {
	return &routingLink{nodeRadio: routingRadio{output: make(chan []byte, 64)}}
}
func (l *routingLink) Radio() (node.Radio, func()) {
	return &l.nodeRadio, func() { l.stopped.Store(true) }
}
func (l *routingLink) AddTXResultHandler(handler func(radio.TXResult)) {
	l.mu.Lock()
	l.handlers = append(l.handlers, handler)
	l.mu.Unlock()
}
func (l *routingLink) result(result radio.TXResult) {
	l.mu.Lock()
	handlers := append([]func(radio.TXResult){}, l.handlers...)
	l.mu.Unlock()
	for _, handler := range handlers {
		handler(result)
	}
}

func routingStart(t *testing.T, ctx context.Context, cfg Config, role string, open func(context.Context) (sourceLink, error)) *roleWorker {
	return routingStartWithIdentity(t, ctx, cfg, role, func(ctx context.Context, _ meshcore.LocalIdentity) (sourceLink, error) {
		return open(ctx)
	})
}

func routingStartWithIdentity(t *testing.T, ctx context.Context, cfg Config, role string,
	open func(context.Context, meshcore.LocalIdentity) (sourceLink, error),
) *roleWorker {
	t.Helper()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	diagnostics := newTXDiagnostics(logger)
	t.Cleanup(func() { _ = diagnostics.Close() })
	worker, err := startRole(ctx, cfg, role, roles.Config{
		StateDir: filepath.Join(cfg.StateDir, role), Name: "Initial " + role,
		Password: "test-room", AdminPassword: "test-admin",
		Policy: cfg.CompanionPolicy, RXScore: radioScore(cfg.Radio, nil),
		AirtimeEstimator: func(int) uint32 { return 17 },
	}, logger, diagnostics, open)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = worker.Close() })
	return worker
}

func routingNext(t *testing.T, link *routingLink) *meshcore.Packet {
	t.Helper()
	select {
	case raw := <-link.nodeRadio.output:
		p, err := meshcore.PacketFromBytes(raw)
		if err != nil {
			t.Fatal(err)
		}
		return p
	case <-time.After(3 * time.Second):
		t.Fatal("no role packet")
		return nil
	}
}

func routingEncrypt(t *testing.T, peer, server meshcore.LocalIdentity, plain []byte) []byte {
	t.Helper()
	key, err := peer.SharedSecret(server.Identity)
	if err != nil {
		t.Fatal(err)
	}
	sealed, err := meshcore.EncryptThenMAC(key, plain)
	if err != nil {
		t.Fatal(err)
	}
	return sealed
}

func routingLogin(t *testing.T, link *routingLink, role string, peer, server meshcore.LocalIdentity) {
	t.Helper()
	plain := binary.LittleEndian.AppendUint32(nil, 1)
	if role == "room" {
		plain = binary.LittleEndian.AppendUint32(plain, 0)
	}
	plain = append(plain, []byte("test-admin\x00")...)
	payload := append([]byte{server.PublicKey()[0]}, peer.PublicKeyBytes()...)
	payload = append(payload, routingEncrypt(t, peer, server, plain)...)
	link.nodeRadio.inject(&meshcore.Packet{Header: meshcore.PayloadTypeAnonReq<<2 | 2, Payload: payload})
	routingNext(t, link)
}

func routingCommand(t *testing.T, link *routingLink, peer, server meshcore.LocalIdentity, stamp uint32, command string) {
	t.Helper()
	plain := binary.LittleEndian.AppendUint32(nil, stamp)
	plain = append(plain, 4)
	plain = append(plain, command...)
	payload := append([]byte{server.PublicKey()[0], peer.PublicKey()[0]}, routingEncrypt(t, peer, server, plain)...)
	link.nodeRadio.inject(&meshcore.Packet{Header: meshcore.PayloadTypeTxtMsg<<2 | 2, Payload: payload})
}

func TestRoleManagementReadsPhysicalProfileWithoutOwningRetune(t *testing.T) {
	for _, role := range []string{"room", "repeater"} {
		t.Run(role, func(t *testing.T) {
			cfg, identities := lifecycleConfig(t)
			link := newRoutingLink()
			link.phy = &radio.PHYState{Settings: radio.PHYSettings{Radio: cfg.Radio}}
			worker := routingStart(t, context.Background(), cfg, role,
				func(context.Context) (sourceLink, error) { return link, nil })
			routingNext(t, link)
			peer := meshcore.NewLocalIdentityFromSeed([32]byte{85})
			routingLogin(t, link, role, peer, identities[role])
			routingCommand(t, link, peer, identities[role], 2, "get radio")
			packet := routingNext(t, link)
			key, err := peer.SharedSecret(identities[role].Identity)
			if err != nil {
				t.Fatal(err)
			}
			plain, err := meshcore.MACThenDecrypt(key, packet.Payload[2:])
			if err != nil || !bytes.Contains(plain, []byte("> 910.525")) {
				t.Fatalf("role %s did not report verified physical radio: %q: %v", role, plain, err)
			}
			if worker.status().State != "running" {
				t.Fatal("read-only radio query stopped the role")
			}
		})
	}
}

func TestRoutingRoleNativeRestartRetiresSourceAndPreservesOnlyItsState(t *testing.T) {
	for _, role := range []string{"room", "repeater"} {
		t.Run(role, func(t *testing.T) {
			cfg, identities := lifecycleConfig(t)
			first, second, siblingLink := newRoutingLink(), newRoutingLink(), newRoutingLink()
			siblingRole := "room"
			if role == "room" {
				siblingRole = "repeater"
			}
			sibling := routingStart(t, context.Background(), cfg, siblingRole, func(context.Context) (sourceLink, error) {
				return siblingLink, nil
			})
			routingNext(t, siblingLink)
			siblingLink.result(radio.TXResult{State: radio.TXSucceeded, RFAirtime: 99 * time.Millisecond})
			siblingBefore := sibling.status()
			retiring, release := make(chan struct{}), make(chan struct{})
			var once sync.Once
			unblock := func() { once.Do(func() { close(release) }) }
			var started atomic.Bool
			first.beforeClose = func() {
				if started.Load() {
					close(retiring)
					<-release
				}
			}
			var opens atomic.Int32
			worker := routingStart(t, context.Background(), cfg, role, func(context.Context) (sourceLink, error) {
				if opens.Add(1) == 1 {
					return first, nil
				}
				if !first.retired.Load() || !first.nodeRadio.closed.Load() {
					return nil, errors.New("old source/service still active")
				}
				return second, nil
			})
			t.Cleanup(unblock)
			started.Store(true)
			routingNext(t, first)
			peer := meshcore.NewLocalIdentityFromSeed([32]byte{77})
			routingLogin(t, first, role, peer, identities[role])
			routingCommand(t, first, peer, identities[role], 2, "set name Retained")
			routingNext(t, first)
			routingCommand(t, first, peer, identities[role], 3, "log start")
			routingNext(t, first)
			first.result(radio.TXResult{Generation: 1, JobID: 1, State: radio.TXUnknown, Reason: 9})
			before := worker.status()
			if before.ApplicationStartedAt == nil || before.ApplicationStartedAt.IsZero() {
				t.Fatal("active role has no application lifetime timestamp")
			}
			first.offline.Store(true)
			offline := worker.status()
			if *offline.RadioConnected || offline.ApplicationStartedAt == nil ||
				!offline.ApplicationStartedAt.Equal(*before.ApplicationStartedAt) {
				t.Fatal("source disconnect changed the role application lifetime")
			}
			first.offline.Store(false)
			if reconnected := worker.status(); !*reconnected.RadioConnected ||
				!reconnected.ApplicationStartedAt.Equal(*before.ApplicationStartedAt) {
				t.Fatal("source reconnect changed the role application lifetime")
			}
			if before.RoleTX.Unknown != 1 || before.RoleTX.AirtimeComplete {
				t.Fatal("physical outcome callback was not bound to original role")
			}
			// The callback runs on the real roles application worker, not this test.
			routingCommand(t, first, peer, identities[role], 4, "reboot")
			lifecycleWait(t, func() bool {
				select {
				case <-retiring:
					return true
				default:
					return false
				}
			})
			if !first.nodeRadio.closed.Load() || !first.stopped.Load() || opens.Load() != 1 {
				t.Fatal("source retirement preceded old service quiescence or overlapped replacement")
			}
			if worker.status().ApplicationStartedAt != nil {
				t.Fatal("retired role still claims an active application lifetime")
			}
			if err := worker.request(context.Background()); err == nil {
				t.Fatal("another restart was accepted while retirement was pending")
			}
			select {
			case raw := <-first.nodeRadio.output:
				t.Fatalf("native reboot produced a completion packet: %x", raw)
			default:
			}
			data, err := state.ReadRoleJSON(filepath.Join(cfg.StateDir, role, "state.json"))
			if err != nil {
				t.Fatal(err)
			}
			var saved struct {
				Name    string
				Members map[string]json.RawMessage
			}
			if err := json.Unmarshal(data, &saved); err != nil || saved.Name != "Retained" || len(saved.Members) != 1 {
				t.Fatalf("old role did not durably save before retirement: %s: %v", data, err)
			}
			log, err := os.ReadFile(filepath.Join(cfg.StateDir, role, "packet.log"))
			if err != nil || bytes.Count(log, []byte(`"direction":"TX_RESULT"`)) != 1 {
				t.Fatalf("outcome logging missing or duplicated for old source: %s: %v", log, err)
			}
			unblock()
			lifecycleWait(t, func() bool { return opens.Load() == 2 && worker.status().State == "running" })
			advert := routingNext(t, second)
			if !bytes.Contains(advert.Payload, []byte("Retained")) {
				t.Fatal("replacement advertisement did not retain saved name")
			}
			after := worker.status()
			if after.PublicKey != identities[role].String() || after.ApplicationError != "" ||
				after.ApplicationStartedAt == nil || !after.ApplicationStartedAt.After(*before.ApplicationStartedAt) ||
				after.RoleTX.Unknown != 0 || !after.RoleTX.AirtimeComplete || !after.RoleTX.Since.After(before.RoleTX.Since) ||
				second.nodeRadio.factor != 7 {
				t.Fatalf("replacement lost role identity, source policy or fresh accounting: %+v", after)
			}
			siblingAfter := sibling.status()
			if siblingAfter.PublicKey != siblingBefore.PublicKey || siblingAfter.RoleTX.Since != siblingBefore.RoleTX.Since ||
				siblingAfter.ApplicationStartedAt == nil || !siblingAfter.ApplicationStartedAt.Equal(*siblingBefore.ApplicationStartedAt) ||
				siblingAfter.RoleTX.RFAirtime != 99*time.Millisecond || siblingLink.retired.Load() || siblingLink.nodeRadio.closed.Load() {
				t.Fatal("role restart disturbed its running sibling")
			}
			routingCommand(t, second, peer, identities[role], 5, "log start")
			routingNext(t, second)
			second.result(radio.TXResult{Generation: 2, JobID: 1, State: radio.TXFailed, Reason: 7})
			if err := worker.Close(); err != nil {
				t.Fatal(err)
			}
			log, err = os.ReadFile(filepath.Join(cfg.StateDir, role, "packet.log"))
			if err != nil || bytes.Count(log, []byte(`"direction":"TX_RESULT"`)) != 2 || !second.retired.Load() {
				t.Fatalf("new lifetime lost/duplicated result logging or leaked source: %s: %v", log, err)
			}
			for name, expected := range identities {
				got, err := state.Identity(cfg.StateDir, name)
				if err != nil || got.PublicKey() != expected.PublicKey() {
					t.Fatalf("%s identity changed: %v", name, err)
				}
			}
		})
	}
}

func TestRoutingRoleRestartRejectsLegacyAndCancelledAdmission(t *testing.T) {
	for _, legacy := range []bool{false, true} {
		t.Run(map[bool]string{false: "cancelled", true: "legacy"}[legacy], func(t *testing.T) {
			cfg, _ := lifecycleConfig(t)
			cfg.RequireParity = !legacy
			link := newRoutingLink()
			worker := routingStart(t, context.Background(), cfg, "repeater", func(context.Context) (sourceLink, error) {
				return link, nil
			})
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			if !legacy {
				cancel()
			}
			if err := worker.request(ctx); err == nil {
				t.Fatal("unavailable restart was admitted")
			}
			if worker.status().State != "running" || link.retired.Load() || link.nodeRadio.closed.Load() {
				t.Fatal("rejected admission disturbed the running role")
			}
		})
	}
}

func TestRoutingRoleRestartFailureAndShutdownRetireResources(t *testing.T) {
	for _, scenario := range []string{"close-failure", "open-failure", "construction-failure", "shutdown-before-open", "shutdown-during-open", "shutdown-after-open"} {
		t.Run(scenario, func(t *testing.T) {
			cfg, identities := lifecycleConfig(t)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			first, second := newRoutingLink(), newRoutingLink()
			if scenario == "close-failure" {
				first.nodeRadio.closeErr = errors.New("synthetic adapter close failure")
			}
			entered := make(chan struct{})
			var opens atomic.Int32
			var hookErr error
			first.beforeClose = func() {
				if scenario == "shutdown-before-open" {
					cancel()
				}
				if scenario == "construction-failure" {
					// Old Close is complete: corrupt only this private test authority.
					path := filepath.Join(cfg.StateDir, "room", "state.json")
					hookErr = os.WriteFile(path, []byte("{broken"), 0600)
				}
			}
			worker := routingStart(t, ctx, cfg, "room", func(ctx context.Context) (sourceLink, error) {
				if opens.Add(1) == 1 {
					return first, nil
				}
				close(entered)
				if scenario == "open-failure" {
					return nil, errors.New("synthetic role source open failure")
				}
				if scenario == "shutdown-during-open" {
					<-ctx.Done()
					return nil, ctx.Err()
				}
				if scenario == "shutdown-after-open" {
					cancel()
				}
				return second, nil
			})
			if err := worker.request(context.Background()); err != nil {
				t.Fatal(err)
			}
			if scenario == "shutdown-during-open" {
				select {
				case <-entered:
				case <-time.After(3 * time.Second):
					t.Fatal("restart did not enter replacement source open")
				}
				cancel()
			}
			lifecycleWait(t, func() bool { return worker.status().ApplicationError != "" })
			if err := worker.Close(); err == nil {
				t.Fatal("failed operation returned success")
			}
			if hookErr != nil {
				t.Fatal(hookErr)
			}
			status := worker.status()
			if status.State != "stopped" || *status.RadioConnected || status.Telemetry != nil ||
				status.ApplicationStartedAt != nil ||
				status.PublicKey != identities["room"].String() || !first.retired.Load() || !first.nodeRadio.closed.Load() {
				t.Fatalf("failed restart exposed stale service or leaked resources: %+v", status)
			}
			if scenario == "construction-failure" && (!second.retired.Load() || !second.stopped.Load()) {
				t.Fatal("construction failure leaked the newly opened source")
			}
			if scenario == "shutdown-after-open" && (!second.retired.Load() || len(second.nodeRadio.output) != 0) {
				t.Fatal("shutdown leaked or activated newly opened source")
			}
			if (scenario == "close-failure" || scenario == "shutdown-before-open") && opens.Load() != 1 {
				t.Fatal("replacement opened after failed close or shutdown")
			}
			if err := worker.request(context.Background()); err == nil {
				t.Fatal("failed role admitted another restart")
			}
		})
	}
}
