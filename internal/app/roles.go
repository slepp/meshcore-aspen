package app

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/roles"
	"meshcore.local/meshcore/internal/state"
)

type roleInstance struct {
	started time.Time
	service *roles.Service
	link    sourceLink
	stop    func()
}

func (i *roleInstance) close() error {
	err := i.service.Close()
	i.stop()
	return errors.Join(err, i.link.Close())
}

// Room and repeater have the same service lifetime, unlike companion's
// listener and reset authority. This worker only replaces one such service.
type roleWorker struct {
	cfg         Config
	role        string
	config      roles.Config
	logger      *slog.Logger
	diagnostics *txDiagnostics
	open        func(context.Context, meshcore.LocalIdentity) (sourceLink, error)
	ctx         context.Context
	cancel      context.CancelFunc
	done        chan struct{}
	requests    chan struct{}

	mu       sync.Mutex
	current  *roleInstance
	identity meshcore.LocalIdentity
	tx       *roleTX
	pending  bool
	phase    string
	err      error
}

func startRole(ctx context.Context, cfg Config, role string, config roles.Config,
	logger *slog.Logger, diagnostics *txDiagnostics, open func(context.Context, meshcore.LocalIdentity) (sourceLink, error),
) (*roleWorker, error) {
	ctx, cancel := context.WithCancel(ctx)
	w := &roleWorker{
		cfg: cfg, role: role, config: config, logger: logger, diagnostics: diagnostics, open: open,
		ctx: ctx, cancel: cancel, done: make(chan struct{}), requests: make(chan struct{}, 1),
		phase: "starting",
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

func (w *roleWorker) request(ctx context.Context) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := w.ctx.Err(); err != nil {
		return err
	}
	if !w.cfg.RequireParity {
		return errors.New("role restart requires readback-only queued parity radio")
	}
	if w.pending || w.current == nil {
		return errors.New("role restart is busy or stopped")
	}
	if err := w.current.service.ApplicationError(); err != nil {
		return err
	}
	w.pending = true
	w.phase = "restarting"
	w.requests <- struct{}{}
	return nil
}

func (w *roleWorker) start() (*roleInstance, error) {
	if err := w.ctx.Err(); err != nil {
		return nil, err
	}
	if err := state.ActivateRoleIdentity(w.cfg.StateDir, w.role, roles.RebindStateIdentity); err != nil {
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
	if err := w.ctx.Err(); err != nil {
		return nil, errors.Join(err, link.Close())
	}
	tx := newRoleTX(w.role, w.cfg.RequireParity, w.diagnostics)
	link.AddTXResultHandler(tx.record)
	w.mu.Lock()
	w.tx = tx
	w.mu.Unlock()
	config := w.config
	if w.cfg.nodeBackup != nil {
		config.BackupCommand = w.cfg.nodeBackup.Command
	}
	// Timing and RX scoring follow this source's verified PHY across retunes.
	if estimate := link.AirtimeEstimator(); estimate != nil {
		config.AirtimeEstimator = estimate
	}
	config.RXScore = radioScore(w.cfg.Radio, link.EffectivePHY)
	config.CurrentRadio = func() (hardware.RadioConfig, bool) {
		phy, ok := link.EffectivePHY()
		return phy.Settings.Radio, ok
	}
	config.CurrentPower = func() (uint8, bool) {
		phy, ok := link.EffectivePHY()
		return phy.Settings.TxPower, ok
	}
	config.RequestRestart = w.request
	config.StageIdentity = nil
	if w.cfg.RoleKeyImport {
		config.StageIdentity = func(ctx context.Context, key []byte) error {
			if err := ctx.Err(); err != nil {
				return err
			}
			return state.StageRoleIdentity(ctx, w.cfg.StateDir, w.role, key)
		}
	}
	config.ErrorHandler = func(err error) { w.logger.Error("role operation", "role", w.role, "error", err) }
	config.Telemetry = func() (roles.Telemetry, error) {
		snapshot, err := link.Snapshot()
		return roleTelemetry(snapshot, tx.snapshot()), err
	}
	constructor := roles.NewRepeater
	if w.role == "room" {
		constructor = roles.NewRoom
	}
	baseRadio, stop := link.Radio()
	service, err := constructor(identity, baseRadio, config)
	if err != nil {
		stop()
		return nil, errors.Join(err, link.Close())
	}
	link.AddTXResultHandler(service.LogTXResult)
	return &roleInstance{service: service, link: link, stop: stop}, nil
}

func (w *roleWorker) run() {
	defer close(w.done)
	for {
		select {
		case <-w.ctx.Done():
			if err := w.stop(); err != nil {
				w.fail(err)
			}
			return
		case <-w.requests:
			w.mu.Lock()
			w.phase = "restarting"
			w.mu.Unlock()
			err := w.stop()
			var next *roleInstance
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

func (w *roleWorker) stop() error {
	w.mu.Lock()
	i := w.current
	w.current = nil
	w.mu.Unlock()
	if i == nil {
		return nil
	}
	err := i.close()
	w.mu.Lock()
	w.phase = "stopped"
	w.mu.Unlock()
	return err
}

func (w *roleWorker) fail(err error) {
	w.mu.Lock()
	w.err, w.phase = err, "stopped"
	w.mu.Unlock()
	w.logger.Error("role application", "role", w.role, "error", err)
}

func (w *roleWorker) Close() error {
	w.cancel()
	<-w.done
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.err
}

func (w *roleWorker) status() roleStatus {
	w.mu.Lock()
	defer w.mu.Unlock()
	online := false
	entry := roleStatus{PublicKey: w.identity.String(), State: w.phase, RadioConnected: &online}
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
		if err := i.service.ApplicationError(); err != nil {
			entry.State, entry.ApplicationError = "faulted", err.Error()
		}
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
