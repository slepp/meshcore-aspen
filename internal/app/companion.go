package app

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net"
	"path/filepath"
	"sync"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/companion"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/state"
)

// The binding owns the physical source connection, not merely the Node adapter.
type sourceLink interface {
	Radio() (node.Radio, func())
	Close() error
	Online() bool
	RolePresenceStatus() *radio.RolePresenceStatus
	Snapshot() (radio.Telemetry, error)
	AddTXResultHandler(func(radio.TXResult))
	EffectivePHY() (radio.PHYState, bool)
	AirtimeEstimator() node.AirtimeEstimator
}

type companionInstance struct {
	started  time.Time
	server   *companion.Server
	link     sourceLink
	stop     func()
	listener *trackedListener
	done     chan struct{}
	serveErr error
}

func (i *companionInstance) close() error {
	err := i.server.Close()
	_ = i.listener.Close()
	<-i.done
	i.stop()
	err = errors.Join(err, i.link.Close())
	if i.serveErr != nil && !errors.Is(i.serveErr, context.Canceled) && !errors.Is(i.serveErr, net.ErrClosed) {
		err = errors.Join(err, i.serveErr)
	}
	return err
}

type companionOperation bool

const (
	companionRestart companionOperation = false
	companionReset   companionOperation = true
)

// Only this worker replaces the companion. Sibling roles and the observer's
// physical configuration ownership remain outside its lifetime.
type companionWorker struct {
	cfg         Config
	role        string
	open        func(context.Context, meshcore.LocalIdentity) (sourceLink, error)
	logger      *slog.Logger
	ctx         context.Context
	cancel      context.CancelFunc
	done        chan struct{}
	requests    chan companionOperation
	diagnostics *txDiagnostics

	mu       sync.Mutex
	current  *companionInstance
	identity meshcore.LocalIdentity
	address  string
	pending  bool
	phase    string
	err      error
	tx       *roleTX
}

func startCompanion(ctx context.Context, cfg Config, identities map[string]meshcore.LocalIdentity,
	logger *slog.Logger, diagnostics *txDiagnostics, open func(context.Context, meshcore.LocalIdentity) (sourceLink, error),
) (*companionWorker, error) {
	return startCompanionRole(ctx, cfg, "companion", identities, logger, diagnostics, open)
}

func startCompanionRole(ctx context.Context, cfg Config, role string, identities map[string]meshcore.LocalIdentity,
	logger *slog.Logger, diagnostics *txDiagnostics, open func(context.Context, meshcore.LocalIdentity) (sourceLink, error),
) (*companionWorker, error) {
	if role != "companion" && role != "bot_companion" {
		return nil, errors.New("unsupported companion role")
	}
	address := cfg.CompanionListen
	if role == "bot_companion" {
		address = cfg.BotCompanionListen
		if address == "" {
			return nil, errors.New("bot companion listener is disabled")
		}
	}
	ctx, cancel := context.WithCancel(ctx)
	w := &companionWorker{
		cfg: cfg, role: role, open: open, logger: logger,
		diagnostics: diagnostics,
		ctx:         ctx, cancel: cancel, done: make(chan struct{}), requests: make(chan companionOperation, 1),
		identity: identities[role], address: address, phase: "starting",
	}
	instance, err := w.start()
	if err != nil {
		cancel()
		return nil, err
	}
	w.mu.Lock()
	instance.started = time.Now().UTC()
	w.current, w.phase = instance, "running"
	w.mu.Unlock()
	go w.run()
	return w, nil
}

func (w *companionWorker) request(ctx context.Context, op companionOperation) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := w.ctx.Err(); err != nil {
		return err
	}
	if op == companionReset && w.role != "companion" {
		return errors.New("bot companion factory reset is disabled")
	}
	// The legacy transport configures the PHY on every connection. Never
	// restart that binding behind the observer's configuration ownership.
	if !w.cfg.RequireParity {
		return errors.New("companion lifecycle requires readback-only queued parity radio")
	}
	if w.pending || w.current == nil {
		return errors.New("companion lifecycle is busy or stopped")
	}
	if err := w.current.server.ApplicationError(); err != nil {
		return err
	}
	w.pending = true
	w.phase = "restarting"
	if op == companionReset {
		w.phase = "resetting"
	}
	w.requests <- op
	return nil
}

func (w *companionWorker) start() (*companionInstance, error) {
	if err := w.ctx.Err(); err != nil {
		return nil, err
	}
	identity, err := state.Identity(w.cfg.StateDir, w.role)
	if err != nil {
		return nil, err
	}
	w.mu.Lock()
	w.identity = identity
	w.mu.Unlock()
	link, err := w.open(w.ctx, identity)
	if err != nil {
		return nil, err
	}
	tx := newRoleTX(w.role, w.cfg.RequireParity, w.diagnostics)
	link.AddTXResultHandler(tx.record)
	w.mu.Lock()
	w.tx = tx
	w.mu.Unlock()
	baseRadio, stop := link.Radio()
	server, err := companion.New(identity, baseRadio, w.serverConfig(link))
	if err != nil {
		stop()
		return nil, errors.Join(err, link.Close())
	}
	socket, err := net.Listen("tcp", w.address)
	if err != nil {
		closeErr := server.Close()
		stop()
		return nil, errors.Join(err, closeErr, link.Close())
	}
	listener := trackListener(socket)
	i := &companionInstance{server: server, link: link, stop: stop, listener: listener, done: make(chan struct{})}
	// Preserve the selected port even when the initial endpoint used :0.
	w.mu.Lock()
	w.address = listener.Addr().String()
	w.mu.Unlock()
	go func() {
		defer close(i.done)
		i.serveErr = server.Serve(w.ctx, listener)
	}()
	w.logger.Info("endpoint listening", "role", w.role, "address", listener.Addr())
	return i, nil
}

func (w *companionWorker) run() {
	defer close(w.done)
	for {
		w.mu.Lock()
		i := w.current
		w.mu.Unlock()
		var stopped <-chan struct{}
		if i != nil {
			stopped = i.done
		}
		select {
		case <-w.ctx.Done():
			if err := w.stop(); err != nil {
				w.fail(err)
			}
			return
		case <-stopped:
			err := w.stop()
			if w.ctx.Err() == nil {
				err = errors.Join(errors.New(w.role+" listener stopped unexpectedly"), err)
			}
			if err != nil {
				w.fail(err)
			}
		case op := <-w.requests:
			w.mu.Lock()
			// A listener failure may have won the select after admission.
			if w.current == nil {
				w.pending = false
				w.mu.Unlock()
				continue
			}
			w.phase = "restarting"
			if op == companionReset {
				w.phase = "resetting"
			}
			w.mu.Unlock()
			err := w.stop()
			if err == nil {
				err = w.ctx.Err()
			}
			if err == nil && op == companionReset {
				// Close has completed its final save before replacing authority.
				var identity meshcore.LocalIdentity
				identity, err = state.ResetRole(filepath.Join(w.cfg.StateDir, w.role, "companion.json"))
				if err == nil {
					w.mu.Lock()
					w.identity = identity
					w.mu.Unlock()
				}
			}
			var next *companionInstance
			if err == nil {
				next, err = w.start()
			}
			if next != nil && w.ctx.Err() != nil {
				err = errors.Join(w.ctx.Err(), next.close())
				next = nil
			}
			if err != nil {
				w.fail(err)
			}
			w.mu.Lock()
			w.current, w.pending = next, false
			if next != nil {
				next.started = time.Now().UTC()
				w.phase = "running"
			}
			w.mu.Unlock()
		}
	}
}

func (w *companionWorker) stop() error {
	w.mu.Lock()
	i := w.current
	if i != nil {
		w.identity = i.server.Node.Identity()
	}
	w.current = nil
	w.mu.Unlock()
	if i == nil {
		return nil
	}
	err := i.close()
	w.mu.Lock()
	w.identity = i.server.Node.Identity()
	w.phase = "stopped"
	w.mu.Unlock()
	return err
}

func (w *companionWorker) fail(err error) {
	w.mu.Lock()
	w.err, w.phase = err, "stopped"
	w.mu.Unlock()
	w.logger.Error("companion application", "role", w.role, "error", err)
}

func (w *companionWorker) Close() error {
	w.cancel()
	<-w.done
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.err
}

func (w *companionWorker) status() roleStatus {
	w.mu.Lock()
	defer w.mu.Unlock()
	online := false
	entry := roleStatus{
		PublicKey: w.identity.String(), Endpoint: w.address,
		RadioConnected: &online, State: w.phase,
	}
	if w.role == "bot_companion" {
		entry.ApplicationKind = "companion_endpoint"
	}
	retention := w.cfg.retentionStatusFor(w.role, w.current != nil && w.current.listener.active.Load())
	entry.CompanionRetention = &retention
	if w.err != nil {
		entry.ApplicationError = w.err.Error()
	}
	if w.tx != nil {
		tx := w.tx.snapshot()
		entry.RoleTX = &tx
	}
	if i := w.current; i != nil {
		entry.ApplicationStartedAt = &i.started
		entry.setRolePresence(i.link.RolePresenceStatus())
		entry.setPHY(i.link)
		active := i.listener.active.Load()
		entry.ListenerActive = &active
		if err := i.server.ApplicationError(); err != nil {
			entry.State, entry.ApplicationError = "faulted", err.Error()
		}
		if !active {
			entry.State, entry.ApplicationStartedAt = "stopped", nil
		}
		entry.PublicKey = i.server.Node.Identity().String()
		online = i.link.Online()
		snapshot, err := i.link.Snapshot()
		if err != nil {
			entry.TelemetryError = err.Error()
		} else {
			entry.Telemetry = &snapshot
			entry.TelemetryCounterScope = "shared_physical_modem"
		}
	}
	return entry
}

func (w *companionWorker) serverConfig(link sourceLink) companion.Config {
	cfg := w.cfg
	name, policyOverrides, retentionName := cfg.CompanionName, cfg.CompanionPolicy, cfg.CompanionRetention
	if w.role == "bot_companion" {
		name, policyOverrides, retentionName = cfg.BotCompanionName, cfg.BotCompanionPolicy, cfg.BotCompanionRetention
	} else {
		policyOverrides = cfg.withLegacyAdvert(policyOverrides)
	}
	retention := companion.DurableReplay
	if retentionName == "native_queue" {
		retention = companion.NativeQueue
	}
	// SELF_INFO, radio setting checks and RX scoring follow the source's live
	// readback; while it is unverified they use the PHY verified at start.
	radioConfig, txPower := cfg.Radio, int8(cfg.TxPower)
	if state, ok := link.EffectivePHY(); ok {
		radioConfig, txPower = state.Settings.Radio, int8(state.Settings.TxPower)
	}
	effectivePHY := func() (hardware.RadioConfig, int8, bool) {
		state, ok := link.EffectivePHY()
		return state.Settings.Radio, int8(state.Settings.TxPower), ok
	}
	out := companion.Config{
		StateDir: filepath.Join(cfg.StateDir, w.role), Name: name,
		Policy: policyOverrides, Retention: retention,
		RadioConfig: radioConfig, TxPower: txPower, EffectivePHY: effectivePHY,
		AirtimeEstimator: link.AirtimeEstimator(),
		ErrorHandler:     func(err error) { w.logger.Error("role operation", "role", w.role, "error", err) },
		RequestRestart:   func(ctx context.Context) error { return w.request(ctx, companionRestart) },
		Storage: func(ctx context.Context) (uint32, uint32, error) {
			if err := ctx.Err(); err != nil {
				return 0, 0, err
			}
			usage, err := state.Storage(cfg.StateDir)
			return usage.UsedKB, usage.TotalKB, err
		},
		Battery: func(context.Context) (uint16, error) {
			snapshot, err := link.Snapshot()
			if err != nil {
				return 0, err
			}
			if !snapshot.HasBattery {
				return 0, errors.New("battery reading unavailable from physical modem")
			}
			return snapshot.BatteryMilliVolts, nil
		},
		Stats: func(_ context.Context, kind byte) (protocol.StatsResponse, error) {
			response := protocol.StatsResponse{StatsType: kind}
			if kind != protocol.StatsTypePackets {
				return response, nil
			}
			snapshot, err := link.Snapshot()
			if err != nil {
				return response, err
			}
			if !snapshot.HasCounters {
				return response, nil
			}
			response.Packets = &protocol.PacketStats{
				PacketsRecv: snapshot.Counters.PacketsRecv, PacketsSent: snapshot.Counters.PacketsSent,
				RecvErrors: snapshot.Counters.PacketsErrors,
			}
			return response, nil
		},
	}
	if w.role == "companion" && cfg.CompanionFactoryReset {
		out.RequestFactoryReset = func(ctx context.Context) error { return w.request(ctx, companionReset) }
	}
	if w.role == "companion" && cfg.CompanionKeyExport {
		out.ExportIdentity = func(ctx context.Context) ([]byte, error) {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			return state.ExportIdentity(cfg.StateDir, "companion")
		}
	}
	if w.role == "companion" && cfg.CompanionKeyImport ||
		w.role == "bot_companion" && cfg.BotCompanionKeyImport {
		out.ImportIdentity = func(ctx context.Context, key []byte, next json.RawMessage) (meshcore.LocalIdentity, error) {
			if err := ctx.Err(); err != nil {
				return meshcore.LocalIdentity{}, err
			}
			return state.ImportRoleIdentityContext(ctx, filepath.Join(cfg.StateDir, w.role, "companion.json"), key, next)
		}
	}
	return out
}
