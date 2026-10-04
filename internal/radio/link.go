package radio

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/hardware/transport"
	"github.com/meshcore-go/meshcore-go/node"
)

var ErrOffline = errors.New("physical KISS modem is offline")

type Config struct {
	Address            string
	Radio              hardware.RadioConfig
	TxPower            uint8
	Logger             *slog.Logger
	RequireParity      bool
	ConfigurationOwner bool
	PHYProfile         *PHYProfile
	// PHYTracking applies to queued mode. PHYFollow ignores Radio, TxPower
	// and PHYProfile as expectations and requires a nonowner link.
	PHYTracking PHYTracking
	// PHYGroup, when set, maintains the modem's airtime model for this link's
	// effective PHY and shares it with other links to the same modem.
	PHYGroup         *PHYGroup
	RoleAnnouncement *RoleAnnouncement
	TXResultHandler  func(TXResult)
	Session          *Session
	SessionPort      int
}

// Link preserves role state across connections, but never replays an uncertain TX.
type Link struct {
	config         Config
	ctx            context.Context
	cancel         context.CancelFunc
	done           chan struct{}
	fatal          chan error
	reset          chan *hardware.KissModem
	mu             sync.RWMutex
	request        sync.Mutex
	modem          *hardware.KissModem
	sessionPort    *sessionTransport
	data           hardware.DataFrameHandler
	sent           []func([]byte)
	telemetry      Telemetry
	sampled        time.Time
	submitMu       sync.Mutex
	generation     uint32
	nextJob        uint32
	sourceFactor   float64
	pending        map[uint32]*pendingTX
	txHandlers     []func(TXResult)
	configured     bool
	presenceStatus *RolePresenceStatus
	skipPresence   bool
	// settling is a connection whose CONFIG readback cleared the modem's
	// stale fence but has not been adopted; it admits no submissions.
	settling        *hardware.KissModem
	refresh         chan struct{}
	phy             *PHYState
	airtime         atomic.Pointer[airtimeTable]
	transitions     []PHYTransition
	transitionCount uint64
	phyError        string
}

func Open(ctx context.Context, cfg Config) (*Link, error) {
	if !cfg.RequireParity && (cfg.ConfigurationOwner || cfg.PHYProfile != nil) {
		return nil, errors.New("PHY ownership/profile requires queued parity mode")
	}
	if cfg.PHYProfile != nil {
		if err := cfg.PHYProfile.Validate(); err != nil {
			return nil, fmt.Errorf("PHY profile: %w", err)
		}
	}
	switch cfg.PHYTracking {
	case PHYFixed:
	case PHYFollow:
		if !cfg.RequireParity || cfg.ConfigurationOwner {
			return nil, errors.New("following the modem PHY requires a queued nonowner link")
		}
	default:
		return nil, errors.New("unknown PHY tracking mode")
	}
	if cfg.RoleAnnouncement != nil {
		if !cfg.RequireParity || cfg.RoleAnnouncement.Role > ObserverRole {
			return nil, errors.New("role announcement requires queued parity and a valid mast role")
		}
		announcement := *cfg.RoleAnnouncement
		cfg.RoleAnnouncement = &announcement
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.Session != nil && (!cfg.RequireParity || cfg.SessionPort < 0 || cfg.SessionPort >= sessionPorts) {
		return nil, errors.New("KISS session requires queued parity and port 0..3")
	}
	ctx, cancel := context.WithCancel(ctx)
	link := &Link{
		config: cfg, ctx: ctx, cancel: cancel,
		done: make(chan struct{}), fatal: make(chan error, 1), reset: make(chan *hardware.KissModem, 1),
		refresh:      make(chan struct{}, 1),
		sourceFactor: 1, pending: make(map[uint32]*pendingTX),
	}
	if cfg.TXResultHandler != nil {
		link.txHandlers = append(link.txHandlers, cfg.TXResultHandler)
	}
	modem, err := link.connect()
	if err != nil {
		cancel()
		return nil, err
	}
	link.mu.Lock()
	link.modem = modem
	link.mu.Unlock()
	link.markSessionOnline(modem)
	if cfg.PHYGroup != nil {
		cfg.PHYGroup.join(link)
	}
	go link.run(modem)
	return link, nil
}

// connect establishes one negotiated connection. If optional role discovery
// leaves control correlation uncertain, that stream is discarded and one fresh
// connection is attempted without discovery; HELLO and PHY checks stay strict.
func (l *Link) connect() (*hardware.KissModem, error) {
	modem, err := l.dial()
	if errors.Is(err, errPresenceUncertain) && l.ctx.Err() == nil {
		l.config.Logger.Warn("role presence uncertain; reconnecting without discovery", "error", err)
		l.mu.Lock()
		l.skipPresence = true
		l.mu.Unlock()
		modem, err = l.dial()
	}
	return modem, err
}

func (l *Link) dial() (*hardware.KissModem, error) {
	ctx, cancel := context.WithTimeout(l.ctx, 15*time.Second)
	defer cancel()
	var t hardware.Transport = transport.NewTCPTransport(transport.TCPConfig{
		Address: l.config.Address, ReadIdleTimeout: -1, WriteTimeout: 5 * time.Second,
	})
	if l.config.Session != nil {
		var err error
		t, err = l.config.Session.Transport(l.config.SessionPort)
		if err != nil {
			return nil, err
		}
		l.sessionPort = t.(*sessionTransport)
	}
	modem := hardware.NewKissModem(t,
		hardware.WithSignalReport(true), hardware.WithInboundBuffer(128),
		hardware.WithLogger(l.config.Logger),
		hardware.WithTxFlowControl(30*time.Second),
	)
	modem.SetErrorHandler(func(err error) {
		l.config.Logger.Error("KISS transport", "error", err)
	})
	modem.SetDataHandler(func(data []byte, snr float32, rssi int8, signal bool) {
		l.mu.RLock()
		handler := l.data
		ready := l.modem == modem
		l.mu.RUnlock()
		if ready && handler != nil {
			handler(data, snr, rssi, signal)
		}
	})
	modem.OnHwResponse(hwTXEvent, func(_ byte, data []byte) {
		l.receiveTXEvent(modem, data)
	})
	replies := make(chan error, 4)
	modem.OnHwResponse(hardware.HW_RESP_OK, func(_ byte, _ []byte) {
		select {
		case replies <- nil:
		default:
		}
	})
	modem.OnHwResponse(hardware.HW_RESP_ERROR, func(_ byte, data []byte) {
		select {
		case replies <- fmt.Errorf("modem rejected configuration: %x", data):
		default:
		}
	})
	fail := func(err error) (*hardware.KissModem, error) {
		return nil, errors.Join(err, modem.Close())
	}
	if err := modem.Connect(ctx); err != nil {
		return fail(err)
	}
	if l.config.RequireParity {
		state, err := l.negotiate(ctx, modem)
		if err != nil {
			return fail(err)
		}
		l.mu.RLock()
		cause := "connect"
		if l.phy != nil {
			cause = "reconnect"
		}
		l.mu.RUnlock()
		// A new modulation needs a full airtime sweep, which gets its own bound.
		settleCtx, cancelSettle := context.WithTimeout(l.ctx, airtimeReadTimeout)
		err = l.settlePHY(settleCtx, modem, state, cause)
		cancelSettle()
		if err != nil {
			return fail(err)
		}
		if err := l.sampleTelemetry(ctx, modem); err != nil {
			return fail(err)
		}
		l.config.Logger.Info("queued physical KISS connected", "address", l.config.Address)
		return modem, nil
	}
	for _, command := range []func() error{
		func() error { return modem.SetRadio(&l.config.Radio) },
		func() error { return modem.SetTxPower(l.config.TxPower) },
	} {
		if err := command(); err != nil {
			return fail(err)
		}
		select {
		case err := <-replies:
			if err != nil {
				return fail(err)
			}
		case <-ctx.Done():
			return fail(ctx.Err())
		case <-modem.Dead():
			return fail(ErrOffline)
		}
	}
	actual, err := modem.RadioConfiguration(ctx)
	if err != nil {
		return fail(err)
	}
	if *actual != l.config.Radio {
		return fail(fmt.Errorf("radio configuration mismatch: got %+v", *actual))
	}
	power, err := modem.TxPowerLevel(ctx)
	if err != nil {
		return fail(err)
	}
	if power != l.config.TxPower {
		return fail(fmt.Errorf("radio power mismatch: got %d", power))
	}
	if group := l.config.PHYGroup; group != nil {
		airtimeCtx, cancelAirtime := context.WithTimeout(l.ctx, airtimeReadTimeout)
		table, err := group.airtime(airtimeCtx, modulationOf(l.config.Radio), func() (*airtimeTable, error) {
			return l.readAirtime(airtimeCtx, modem)
		})
		cancelAirtime()
		if err != nil {
			return fail(err)
		}
		l.airtime.Store(table)
	}
	if err := l.sampleTelemetry(ctx, modem); err != nil {
		return fail(err)
	}
	l.config.Logger.Info("physical KISS connected", "address", l.config.Address)
	return modem, nil
}

func (l *Link) markSessionOnline(modem *hardware.KissModem) {
	if l.sessionPort == nil || l.ctx.Err() != nil {
		return
	}
	select {
	case <-modem.Dead():
		return
	default:
		l.sessionPort.markOnline()
	}
}

func (l *Link) run(modem *hardware.KissModem) {
	defer close(l.done)
	if l.config.PHYGroup != nil {
		defer l.config.PHYGroup.leave(l)
	}
	// A heartbeat can be waiting behind a caller's control request. Closing
	// the connection independently of that wait lets cancellation unblock both.
	stop := context.AfterFunc(l.ctx, func() {
		l.mu.RLock()
		current := l.modem
		l.mu.RUnlock()
		if current != nil {
			_ = current.Close()
		}
	})
	defer stop()
	heartbeat := time.NewTicker(15 * time.Second)
	defer heartbeat.Stop()
	for {
		select {
		case <-l.ctx.Done():
			l.disconnect(modem)
			return
		case <-modem.Dead():
		case failed := <-l.reset:
			if failed != modem {
				continue
			}
		case <-l.refresh:
			if !l.config.RequireParity {
				continue
			}
			err := l.control(airtimeReadTimeout, func(ctx context.Context) error { return l.refreshPHY(ctx, modem) })
			if err == nil {
				continue
			}
			l.setPHYError(err)
			l.config.Logger.Error("shared PHY readback failed; reconnecting", "error", err)
		case <-heartbeat.C:
			err := l.control(10*time.Second, func(ctx context.Context) error {
				if err := modem.PingWait(ctx); err != nil {
					return err
				}
				return l.sampleTelemetry(ctx, modem)
			})
			if err == nil && l.config.RequireParity {
				err = l.control(airtimeReadTimeout, func(ctx context.Context) error { return l.refreshPHY(ctx, modem) })
			}
			if err == nil {
				continue
			}
			l.setPHYError(err)
			l.config.Logger.Error("KISS heartbeat failed", "error", err)
		}
		l.disconnect(modem)
		retryDelay := 2 * time.Second
		for {
			select {
			case <-l.ctx.Done():
				return
			case <-time.After(retryDelay):
			}
			var err error
			modem, err = l.connect()
			if err != nil {
				l.setPHYError(err)
				if l.config.Session != nil && l.config.SessionPort == 0 && errors.Is(err, ErrSubportsUnavailable) {
					l.config.Logger.Error("KISS session subports lost; host supervisor restart required", "error", err)
					l.fatal <- err
					return
				}
				retryDelay = min(retryDelay*2, 30*time.Second)
				if errors.Is(err, ErrPHYProfileMismatch) || errors.Is(err, ErrInvalidPHYProfile) {
					retryDelay = 30 * time.Second
				}
				l.config.Logger.Error("KISS reconnect failed; link remains offline",
					"error", err, "retry_in", retryDelay)
				continue
			}
			l.mu.Lock()
			l.modem = modem
			l.mu.Unlock()
			l.markSessionOnline(modem)
			break
		}
	}
}

// airtimeReadTimeout bounds a CONFIG readback that may need 256 airtime queries.
const airtimeReadTimeout = 30 * time.Second

// control serializes one bounded modem exchange with other control requests.
func (l *Link) control(timeout time.Duration, exchange func(context.Context) error) error {
	l.request.Lock()
	defer l.request.Unlock()
	ctx, cancel := context.WithTimeout(l.ctx, timeout)
	defer cancel()
	return exchange(ctx)
}

func (l *Link) disconnect(modem *hardware.KissModem) {
	l.mu.Lock()
	l.modem = nil
	if l.settling == modem {
		l.settling = nil
	}
	l.mu.Unlock()
	l.failPending()
	if err := modem.Close(); err != nil {
		l.config.Logger.Error("closing KISS connection", "error", err)
	}
}

func (l *Link) Online() bool {
	l.mu.RLock()
	defer l.mu.RUnlock()
	return l.modem != nil
}

// Fatal reports a terminal reconnect failure that requires restarting the host.
func (l *Link) Fatal() <-chan error { return l.fatal }

func (l *Link) SendData(data []byte) error {
	if l.config.RequireParity {
		job, err := l.submit(data, node.PrioritySend, 0, 0)
		if err != nil {
			return err
		}
		select {
		case result := <-job.done:
			if result.State != TXSucceeded {
				return result.Err()
			}
			return nil
		case <-l.ctx.Done():
			return l.ctx.Err()
		}
	}
	l.request.Lock()
	l.mu.RLock()
	modem := l.modem
	handlers := append([]func([]byte){}, l.sent...)
	l.mu.RUnlock()
	if modem == nil {
		l.request.Unlock()
		return ErrOffline
	}
	err := modem.SendData(data)
	l.request.Unlock()
	if err != nil {
		l.requestReset(modem)
		return fmt.Errorf("transmit failed; not replaying uncertain packet: %w", err)
	}
	for _, handler := range handlers {
		handler(data)
	}
	return nil
}

func (l *Link) requestReset(modem *hardware.KissModem) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.modem != modem {
		return
	}
	// Replace an obsolete queued failure instead of dropping the current one.
	select {
	case <-l.reset:
	default:
	}
	l.reset <- modem
}

func (l *Link) SetDataHandler(handler hardware.DataFrameHandler) {
	l.mu.Lock()
	l.data = handler
	l.mu.Unlock()
}

func (l *Link) AddOutboundHandler(handler func([]byte)) {
	l.mu.Lock()
	l.sent = append(l.sent, handler)
	l.mu.Unlock()
}

func (l *Link) Close() error {
	l.cancel()
	<-l.done
	return nil
}

type LocalRadio struct{ node.MuxRadio }

func (r LocalRadio) SetDataHandler(handler func(*meshcore.Packet)) {
	r.MuxRadio.SetDataHandler(func(packet *meshcore.Packet) {
		if packet.HasSignalInfo && packet.SNR == -32 && packet.RSSI == 127 {
			packet.MarkDoNotRetransmit()
		}
		if handler != nil {
			handler(packet)
		}
	})
}

func (l *Link) Radio() (node.Radio, func()) {
	if l.config.RequireParity {
		r := newQueuedRadio(l)
		return r, func() { _ = r.Close() }
	}
	mux := node.NewRadioMux(l,
		node.WithMuxAirtimeEstimator(hardware.LoRaAirtimeEstimator(&l.config.Radio)),
		node.WithMuxMaxTxQueue(32), node.WithMuxLogger(l.config.Logger),
		node.WithMuxErrorHandler(func(err error) { l.config.Logger.Error("radio queue", "error", err) }),
		node.WithMuxRetryable(func(error) bool { return false }),
	)
	return LocalRadio{mux.NewRadio()}, mux.Stop
}
