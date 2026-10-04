package app

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/state"
)

// Deliberately keep Node/adapter closure separate from source retirement.
type lifecycleRadio struct {
	factor      float64
	policyError atomic.Pointer[error]
}

func (*lifecycleRadio) SendData([]byte) error                               { return nil }
func (*lifecycleRadio) SetDataHandler(func(*meshcore.Packet))               {}
func (*lifecycleRadio) Close() error                                        { return nil }
func (*lifecycleRadio) AddOutboundHandler(func([]byte))                     {}
func (*lifecycleRadio) SetRawDataHandler(func([]byte, float32, int8, bool)) {}
func (*lifecycleRadio) Enqueue([]byte, uint8, time.Duration) bool           { return true }
func (*lifecycleRadio) TxQueueLen() int                                     { return 0 }
func (r *lifecycleRadio) SetSourceAirtimeFactor(_ context.Context, factor float64) error {
	if err := r.policyError.Load(); err != nil {
		return *err
	}
	r.factor = factor
	return nil
}

type lifecycleLink struct {
	radio          lifecycleRadio
	retired        atomic.Bool
	offline        atomic.Bool
	stopped        atomic.Bool
	beforeClose    func()
	txHandler      func(radio.TXResult)
	presence       *radio.RolePresenceStatus
	telemetryError atomic.Pointer[error]
	phy            *radio.PHYState
}

func (l *lifecycleLink) Radio() (node.Radio, func()) {
	return &l.radio, func() { l.stopped.Store(true) }
}
func (l *lifecycleLink) Close() error {
	if l.beforeClose != nil {
		l.beforeClose()
	}
	l.retired.Store(true)
	return nil
}
func (l *lifecycleLink) Online() bool                                    { return !l.retired.Load() && !l.offline.Load() }
func (l *lifecycleLink) RolePresenceStatus() *radio.RolePresenceStatus   { return l.presence }
func (l *lifecycleLink) AddTXResultHandler(handler func(radio.TXResult)) { l.txHandler = handler }
func (l *lifecycleLink) EffectivePHY() (radio.PHYState, bool) {
	if l.phy == nil || !l.Online() {
		return radio.PHYState{}, false
	}
	return *l.phy, true
}
func (*lifecycleLink) AirtimeEstimator() node.AirtimeEstimator {
	return func(int) uint32 { return 17 }
}
func (l *lifecycleLink) Snapshot() (radio.Telemetry, error) {
	if !l.Online() {
		return radio.Telemetry{}, radio.ErrOffline
	}
	if err := l.telemetryError.Load(); err != nil {
		return radio.Telemetry{}, *err
	}
	return radio.Telemetry{HasBattery: true, BatteryMilliVolts: 4000}, nil
}

func lifecycleConfig(t *testing.T) (Config, map[string]meshcore.LocalIdentity) {
	t.Helper()
	cfg := DefaultConfig()
	cfg.StateDir, cfg.CompanionListen = t.TempDir(), "127.0.0.1:0"
	cfg.CompanionFactoryReset, cfg.CompanionKeyExport, cfg.CompanionKeyImport = true, true, true
	zero, factor := uint32(0), float32(7)
	cfg.CompanionPolicy = policy.Overrides{
		FloodAdvertSeconds: &zero, LocalAdvertSeconds: &zero, AirtimeFactor: &factor,
	}
	ids := make(map[string]meshcore.LocalIdentity)
	for _, role := range []string{"companion", "room", "repeater", "bot", "observer"} {
		id, err := state.Identity(cfg.StateDir, role)
		if err != nil {
			t.Fatal(err)
		}
		ids[role] = id
	}
	return cfg, ids
}

func lifecycleStart(t *testing.T, ctx context.Context, cfg Config, ids map[string]meshcore.LocalIdentity,
	open func(context.Context) (sourceLink, error),
) *companionWorker {
	return lifecycleStartWithIdentity(t, ctx, cfg, ids, func(ctx context.Context, _ meshcore.LocalIdentity) (sourceLink, error) {
		return open(ctx)
	})
}

func lifecycleStartWithIdentity(t *testing.T, ctx context.Context, cfg Config, ids map[string]meshcore.LocalIdentity,
	open func(context.Context, meshcore.LocalIdentity) (sourceLink, error),
) *companionWorker {
	t.Helper()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	diagnostics := newTXDiagnostics(logger)
	t.Cleanup(func() { _ = diagnostics.Close() })
	w, err := startCompanion(ctx, cfg, ids, logger, diagnostics, open)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = w.Close() })
	return w
}

func lifecycleDial(t *testing.T, address string) net.Conn {
	t.Helper()
	conn, err := net.DialTimeout("tcp", address, 3*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return conn
}

func lifecycleSend(t *testing.T, conn net.Conn, command []byte) {
	t.Helper()
	wire, err := protocol.FrameEncode(protocol.FrameTypeOutgoing, command)
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err := conn.Write(wire); err != nil {
		t.Fatal(err)
	}
}

func lifecycleCommand(t *testing.T, conn net.Conn, command []byte) []byte {
	t.Helper()
	lifecycleSend(t, conn, command)
	var header [3]byte
	if _, err := io.ReadFull(conn, header[:]); err != nil {
		t.Fatal(err)
	}
	if header[0] != '>' {
		t.Fatalf("invalid frame marker %x", header)
	}
	response := make([]byte, binary.LittleEndian.Uint16(header[1:]))
	if _, err := io.ReadFull(conn, response); err != nil {
		t.Fatal(err)
	}
	return response
}

func lifecycleWait(t *testing.T, condition func() bool) {
	t.Helper()
	deadline := time.NewTimer(4 * time.Second)
	defer deadline.Stop()
	tick := time.NewTicker(time.Millisecond)
	defer tick.Stop()
	for !condition() {
		select {
		case <-deadline.C:
			t.Fatal("companion lifecycle did not reach expected state")
		case <-tick.C:
		}
	}
}

func lifecycleDisconnected(t *testing.T, conn net.Conn) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	var b [1]byte
	if n, err := conn.Read(b[:]); n != 0 || !errors.Is(err, io.EOF) {
		t.Fatalf("accepted lifecycle request must disconnect without completion ACK: n=%d err=%v data=%x", n, err, b[:n])
	}
}

func lifecycleDocument(t *testing.T, cfg Config) struct {
	PublicKey   [32]byte
	Latitude    int32
	Name        string
	Preferences policy.Preferences
} {
	t.Helper()
	var document struct {
		PublicKey   [32]byte
		Latitude    int32
		Name        string
		Preferences policy.Preferences
	}
	data, err := state.ReadRoleJSON(filepath.Join(cfg.StateDir, "companion", "companion.json"))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &document); err != nil {
		t.Fatal(err)
	}
	return document
}

func TestCompanionLifecyclePreservesRebootAndResetsOnlyCompanion(t *testing.T) {
	for _, reset := range []bool{false, true} {
		t.Run(map[bool]string{false: "reboot", true: "factory-reset"}[reset], func(t *testing.T) {
			cfg, ids := lifecycleConfig(t)
			siblingFiles := make(map[string][]byte)
			for _, role := range []string{"room", "repeater", "bot", "observer"} {
				path := filepath.Join(cfg.StateDir, role, role+".json")
				if err := state.WriteRoleJSON(path, map[string]string{"retained": role}); err != nil {
					t.Fatal(err)
				}
				siblingFiles[path], _ = os.ReadFile(path)
			}
			retiring, release := make(chan struct{}), make(chan struct{})
			var once sync.Once
			var started atomic.Bool
			unblock := func() { once.Do(func() { close(release) }) }
			t.Cleanup(unblock)
			first := &lifecycleLink{beforeClose: func() {
				if started.Load() {
					close(retiring)
					<-release
				}
			}}
			second := &lifecycleLink{}
			var opens atomic.Int32
			open := func(context.Context) (sourceLink, error) {
				if opens.Add(1) == 1 {
					return first, nil
				}
				if !first.retired.Load() {
					return nil, errors.New("old source connection was not retired")
				}
				return second, nil
			}
			w := lifecycleStart(t, context.Background(), cfg, ids, open)
			started.Store(true)
			// Cleanup must release a held source before waiting for the worker.
			t.Cleanup(unblock)
			address := w.status().Endpoint
			startedAt := w.status().ApplicationStartedAt
			if startedAt == nil || startedAt.IsZero() {
				t.Fatal("active companion has no application lifetime timestamp")
			}
			first.offline.Store(true)
			if status := w.status(); *status.RadioConnected || status.ApplicationStartedAt == nil || !status.ApplicationStartedAt.Equal(*startedAt) {
				t.Fatal("source disconnect changed companion application lifetime")
			}
			first.offline.Store(false)
			first.txHandler(radio.TXResult{State: radio.TXAccepted, Generation: 1, JobID: 1})
			first.txHandler(radio.TXResult{State: radio.TXSucceeded, Generation: 1, JobID: 1, RFAirtime: 17 * time.Millisecond})
			oldTX := w.status().RoleTX
			if oldTX == nil || oldTX.Accepted != 1 || oldTX.Succeeded != 1 || oldTX.RFAirtime != 17*time.Millisecond {
				t.Fatalf("companion source outcomes not reflected in status: %+v", oldTX)
			}
			conn := lifecycleDial(t, address)
			location := make([]byte, 9)
			location[0] = protocol.CmdSetAdvertLatLon
			binary.LittleEndian.PutUint32(location[1:5], 12345)
			if got := lifecycleCommand(t, conn, location); !bytes.Equal(got, []byte{protocol.RespOk}) {
				t.Fatalf("location update: %x", got)
			}
			// Native import is in-place and must retain both transport and state.
			imported, err := state.Identity(cfg.StateDir, "candidate")
			if err != nil {
				t.Fatal(err)
			}
			key, err := state.ExportIdentity(cfg.StateDir, "candidate")
			if err != nil {
				t.Fatal(err)
			}
			if got := lifecycleCommand(t, conn, append([]byte{protocol.CmdImportPrivateKey}, key...)); !bytes.Equal(got, []byte{protocol.RespOk}) {
				t.Fatalf("identity import: %x", got)
			}
			if got := w.status().PublicKey; got != imported.String() {
				t.Fatalf("status retained stale identity: %s", got)
			}
			if got := lifecycleCommand(t, conn, []byte{protocol.CmdExportPrivateKey}); !bytes.Equal(got, append([]byte{protocol.RespPrivateKey}, key...)) {
				t.Fatalf("identity export changed: %x", got)
			}
			wire := (protocol.RebootCommand{}).ToBytes()
			if reset {
				wire = (protocol.FactoryResetCommand{}).ToBytes()
			}
			lifecycleSend(t, conn, wire)
			lifecycleWait(t, func() bool {
				select {
				case <-retiring:
					return true
				default:
					return false
				}
			})
			lifecycleDisconnected(t, conn)
			if w.status().ApplicationStartedAt != nil {
				t.Fatal("retired companion still claims an active application lifetime")
			}
			if err := w.request(context.Background(), companionRestart); err == nil {
				t.Fatal("accepted another operation during retirement")
			}
			// Final Close/save is complete, but reset must still await source retirement.
			before := lifecycleDocument(t, cfg)
			if before.PublicKey != imported.PublicKey() || before.Latitude != 12345 {
				t.Fatal("old state was reset before old Close and retirement completed")
			}
			if opens.Load() != 1 || !first.stopped.Load() {
				t.Fatal("replacement started before old adapter stopped")
			}
			unblock()
			lifecycleWait(t, func() bool { return opens.Load() == 2 && w.status().State == "running" })
			if w.status().Endpoint != address || w.status().ApplicationError != "" {
				t.Fatalf("endpoint or application state changed: %+v", w.status())
			}
			if now := w.status().ApplicationStartedAt; now == nil || !now.After(*startedAt) {
				t.Fatal("successful companion restart did not publish a new application lifetime")
			}
			newTX := w.status().RoleTX
			if newTX == nil || newTX.Accepted != 0 || newTX.Succeeded != 0 || newTX.RFAirtime != 0 ||
				!newTX.AirtimeComplete || !newTX.Since.After(oldTX.Since) {
				t.Fatalf("logical restart did not reset role-local TX accounting: %+v", newTX)
			}
			reconnected := lifecycleDial(t, address)
			export := lifecycleCommand(t, reconnected, []byte{protocol.CmdExportPrivateKey})
			if len(export) != 65 || export[0] != protocol.RespPrivateKey {
				t.Fatalf("replacement did not retain key API: %x", export)
			}
			active, err := state.Identity(cfg.StateDir, "companion")
			if err != nil {
				t.Fatal(err)
			}
			after := lifecycleDocument(t, cfg)
			if reset {
				if active.PublicKey() == imported.PublicKey() || after.Latitude != 0 {
					t.Fatal("reset retained old key or location")
				}
			} else if active.PublicKey() != imported.PublicKey() || after.Latitude != 12345 {
				t.Fatal("reboot lost saved identity or data")
			}
			if after.PublicKey != active.PublicKey() || after.Name != cfg.CompanionName ||
				after.Preferences.AirtimeFactor != 7 || second.radio.factor != 7 {
				t.Fatal("replacement lost identity coherence or operator policy/source capability")
			}
			if err := w.Close(); err != nil {
				t.Fatal(err)
			}
			if !second.retired.Load() || !second.stopped.Load() {
				t.Fatal("shutdown leaked replacement radio binding")
			}
			lifecycleDisconnected(t, reconnected)
			// A fresh worker must use the new authority, not the retained legacy seed.
			cfg.CompanionListen = address
			reloaded := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
				return &lifecycleLink{}, nil
			})
			if reloaded.status().PublicKey != active.String() || lifecycleDocument(t, cfg).Latitude != after.Latitude {
				t.Fatal("new identity/state failed to survive reload")
			}
			for role, identity := range ids {
				if role == "companion" {
					continue
				}
				got, err := state.Identity(cfg.StateDir, role)
				if err != nil || got.PublicKey() != identity.PublicKey() {
					t.Fatalf("sibling %s identity changed: %v", role, err)
				}
			}
			for path, want := range siblingFiles {
				got, err := os.ReadFile(path)
				if err != nil || !bytes.Equal(got, want) {
					t.Fatalf("sibling document changed: %s: %v", path, err)
				}
			}
		})
	}
}

func TestCompanionLifecycleFailureAndShutdownRetireResources(t *testing.T) {
	for _, shutdown := range []bool{false, true} {
		t.Run(map[bool]string{false: "open-failure", true: "shutdown-during-open"}[shutdown], func(t *testing.T) {
			cfg, ids := lifecycleConfig(t)
			first := &lifecycleLink{}
			entered := make(chan struct{})
			var opens atomic.Int32
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			w := lifecycleStart(t, ctx, cfg, ids, func(ctx context.Context) (sourceLink, error) {
				if opens.Add(1) == 1 {
					return first, nil
				}
				close(entered)
				if shutdown {
					<-ctx.Done()
					return nil, ctx.Err()
				}
				return nil, errors.New("synthetic source open failure")
			})
			address := w.status().Endpoint
			conn := lifecycleDial(t, address)
			lifecycleSend(t, conn, (protocol.FactoryResetCommand{}).ToBytes())
			lifecycleDisconnected(t, conn)
			lifecycleWait(t, func() bool {
				select {
				case <-entered:
					return true
				default:
					return false
				}
			})
			if shutdown {
				cancel()
				lifecycleWait(t, func() bool {
					select {
					case <-w.done:
						return true
					default:
						return false
					}
				})
			} else {
				lifecycleWait(t, func() bool { return w.status().ApplicationError != "" })
			}
			got := w.status()
			if got.State != "stopped" || got.ApplicationError == "" || *got.RadioConnected ||
				got.ApplicationStartedAt != nil ||
				!first.retired.Load() || !first.stopped.Load() || opens.Load() != 2 {
				t.Fatalf("failed operation leaked or reported success: %+v", got)
			}
			if err := w.request(context.Background(), companionReset); err == nil {
				t.Fatal("failed role admitted another destructive reset")
			}
			key, err := state.Identity(cfg.StateDir, "companion")
			if err != nil || key.PublicKey() == ids["companion"].PublicKey() {
				t.Fatalf("committed reset identity was lost: %v", err)
			}
			if _, err := state.ReadRoleJSON(filepath.Join(cfg.StateDir, "companion", "companion.json")); !errors.Is(err, os.ErrNotExist) {
				t.Fatalf("failed startup restored stale role data: %v", err)
			}
			listener, err := net.Listen("tcp", address)
			if err != nil {
				t.Fatalf("failed role leaked listener: %v", err)
			}
			_ = listener.Close()
		})
	}
}

func TestCompanionFactoryResetIsOptIn(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	cfg.CompanionFactoryReset = false
	w := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
		return &lifecycleLink{}, nil
	})
	conn := lifecycleDial(t, w.status().Endpoint)
	got := lifecycleCommand(t, conn, (protocol.FactoryResetCommand{}).ToBytes())
	if !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}) {
		t.Fatalf("disabled reset response: %x", got)
	}
	if w.status().State != "running" || w.status().PublicKey != ids["companion"].String() {
		t.Fatal("disabled reset changed the companion")
	}
}

func TestCompanionUnexpectedListenerFailureStopsOnlyItsBinding(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	link := &lifecycleLink{}
	w := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
		return link, nil
	})
	conn := lifecycleDial(t, w.status().Endpoint)
	if got := lifecycleCommand(t, conn, []byte{protocol.CmdGetDeviceTime}); len(got) != 5 || got[0] != protocol.RespCurrTime {
		t.Fatalf("session did not become ready: %x", got)
	}
	w.mu.Lock()
	listener := w.current.listener
	w.mu.Unlock()
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	lifecycleDisconnected(t, conn)
	lifecycleWait(t, func() bool { return w.status().ApplicationError != "" })
	if !link.retired.Load() || !link.stopped.Load() || *w.status().RadioConnected {
		t.Fatal("unexpected listener failure leaked source binding")
	}
	if err := w.request(context.Background(), companionReset); err == nil {
		t.Fatal("unexpectedly stopped companion admitted a reset")
	}
	if active, err := state.Identity(cfg.StateDir, "companion"); err != nil || active.PublicKey() != ids["companion"].PublicKey() {
		t.Fatalf("listener failure changed identity: %v", err)
	}
}
func TestCompanionLifecycleCloseResetAndListenFailures(t *testing.T) {
	for _, scenario := range []string{"final-save-failure", "reset-commit-failure", "listen-failure", "shutdown-before-reset"} {
		t.Run(scenario, func(t *testing.T) {
			cfg, ids := lifecycleConfig(t)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			first, second := &lifecycleLink{}, &lifecycleLink{}
			var opens atomic.Int32
			w := lifecycleStart(t, ctx, cfg, ids, func(context.Context) (sourceLink, error) {
				if opens.Add(1) == 1 {
					return first, nil
				}
				return second, nil
			})
			address := w.status().Endpoint
			conn := lifecycleDial(t, address)
			// Synchronize hook installation with the worker's request mutex.
			w.mu.Lock()
			var occupied net.Listener
			var hookErr error
			first.beforeClose = func() {
				switch scenario {
				case "reset-commit-failure":
					hookErr = os.Mkdir(filepath.Join(cfg.StateDir, "companion", "identity-state.json"), 0700)
				case "listen-failure":
					occupied, hookErr = net.Listen("tcp", address)
				case "shutdown-before-reset":
					cancel()
				}
			}
			w.mu.Unlock()
			if scenario == "final-save-failure" {
				path := filepath.Join(cfg.StateDir, "companion", "companion.json")
				if err := os.Remove(path); err != nil {
					t.Fatal(err)
				}
				if err := os.Mkdir(path, 0700); err != nil {
					t.Fatal(err)
				}
			}
			lifecycleSend(t, conn, (protocol.FactoryResetCommand{}).ToBytes())
			lifecycleDisconnected(t, conn)
			lifecycleWait(t, func() bool { return w.status().ApplicationError != "" })
			if err := w.Close(); err == nil {
				t.Fatal("failed operation was reported as successful")
			}
			// Close waits for the worker and synchronizes hook results.
			if occupied != nil {
				defer occupied.Close()
			}
			if hookErr != nil {
				t.Fatal(hookErr)
			}
			if !first.retired.Load() || !first.stopped.Load() || w.status().State != "stopped" {
				t.Fatal("failure did not retire old companion resources")
			}
			if scenario == "listen-failure" {
				if opens.Load() != 2 || !second.retired.Load() || !second.stopped.Load() {
					t.Fatal("listener failure leaked newly constructed companion resources")
				}
				active, err := state.Identity(cfg.StateDir, "companion")
				if err != nil || active.PublicKey() == ids["companion"].PublicKey() ||
					lifecycleDocument(t, cfg).PublicKey != active.PublicKey() {
					t.Fatalf("failed activation lost coherent reset authority: %v", err)
				}
			} else {
				if opens.Load() != 1 {
					t.Fatal("failure or shutdown still started another source")
				}
				// Even a failed Close must prevent the destructive storage reset.
				if scenario != "reset-commit-failure" {
					active, err := state.Identity(cfg.StateDir, "companion")
					if err != nil || active.PublicKey() != ids["companion"].PublicKey() {
						t.Fatalf("reset ran after failed Close or shutdown: %v", err)
					}
				}
			}
		})
	}
}
