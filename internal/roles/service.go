// Package roles implements host repeater and room application protocols on the
// meshcore-go node router. It does not own identities or the physical modem.
package roles

import (
	"bytes"
	"context"
	"crypto/rand"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
	hoststate "meshcore.local/meshcore/internal/state"
)

var (
	ErrUnsupported = errors.New("operation unsupported by host role")
	ErrQueueFull   = errors.New("role receive queue full")
)

// Config applies to one role. StateDir must be a private, role-specific directory.
// An empty AdminPassword disables password-based administrative login. Password
// is the room's read/write password, or the repeater's guest password; an empty
// Password deliberately allows public access. ErrorHandler must not block or
// call Close; errors go to slog when it is nil. Provisioned passwords are not
// copied to state; explicit native management password changes are persisted.
type Config struct {
	StateDir         string
	Name             string
	Password         string
	AdminPassword    string
	AdvertInterval   time.Duration         // Deprecated: explicit flood interval; prefer Policy overrides.
	AirtimeEstimator node.AirtimeEstimator // Native integer milliseconds for the full serialized packet.
	ErrorHandler     func(error)
	MaxHistory       int
	MaxMembers       int
	MaxNeighbours    int
	MaxLogBytes      int64
	// Retention selects native volatile room history or the durable replay extension.
	// Unset selects native for new state and preserves durable version-1 state.
	Retention RetentionProfile
	// PreferenceProfile selects native boot-time AF clamping or durable host
	// preferences. Unset uses native for new state and preserves existing state.
	PreferenceProfile PreferenceProfile
	Policy            policy.Overrides
	// RXScore supplies a nonblocking native physical-radio score from cached PHY
	// configuration and measured packet metadata. Nonzero rxdelay requires it.
	RXScore func(*meshcore.Packet, int, uint32) (float32, error)
	// Telemetry returns a cached, nonblocking snapshot. The application worker
	// invokes it outside the state lock. Valid fields may accompany an error.
	Telemetry func() (Telemetry, error)
	// CurrentRadio reads the verified shared PHY without claiming role-local
	// tuning authority. False means the physical profile is unavailable.
	CurrentRadio func() (hardware.RadioConfig, bool)
	CurrentPower func() (uint8, bool)
	// RequestRestart admits a role-only restart after the command is committed.
	// It runs outside mu but on the application worker, with a five-second
	// context. Return promptly after enqueueing supervisor work; never call
	// Close synchronously or wait for restart completion. Nil means admitted.
	// The supervisor must wait for Close before replacing this service/state.
	RequestRestart func(context.Context) error
	// StageIdentity durably stages a validated 64-byte scalar-prefix key without
	// changing the active identity. It runs on the application worker, outside mu,
	// after command-state commit, with a five-second context. Return nil only
	// after confirmed persistence; never Close, restart or activate here. The
	// read-only input is borrowed until return, then cleared. Nil disables import.
	StageIdentity func(context.Context, []byte) error
	BackupCommand func(string, bool, int) string
}

type event struct {
	packet       *meshcore.Packet
	key          string
	plain        []byte
	telemetry    Telemetry
	telemetryErr error
	context      policy.ReceiveContext
	log          *packetLogEntry
	ownerCommand *ownerCommand
}

// Service owns a logical node and its supplied radio. Close drains accepted
// application packets and their persistence, then stops the node and closes
// that radio (not other roles' radios).
// Callers must not replace Node's identity or forwarding callbacks.
type Service struct {
	Node *node.Node

	cfg             Config
	id              meshcore.LocalIdentity
	room            bool
	profile         policy.Profile
	policySnapshot  atomic.Pointer[policy.Preferences]
	adverts         policy.AdvertisementSchedule
	started         time.Time
	ingress         sync.RWMutex
	mu              sync.RWMutex
	state           diskState
	queue           chan event
	done            chan struct{}
	exited          chan struct{}
	close           sync.Once
	closeErr        error
	radioCloseErr   error
	lastErr         error
	commitUncertain bool
	terminalError   atomic.Pointer[error]
	requestRestart  bool
	restartPrefix   string
	identityStage   *identityStage
	restartPending  bool
	anonLimit       rateLimiter
	discoveryLimit  rateLimiter
	neighbours      []neighbour
	discoveryTag    uint32
	discoveryUntil  time.Time
	regionLoad      *regionLoad
	commandOwner    string
	eraseLog        bool
	logging         atomic.Bool
	droppedLogs     atomic.Uint64
	random          func([]byte) (int, error)
	writeJSON       func(string, any) error
	sourcePolicy    func(context.Context, float64) error
	nextPush        time.Time
	nextUser        int
	initialPosted   uint64
	initialPushed   uint64
	// Timing is kept on the service so deterministic tests can drive syncAt.
	postDelay time.Duration

	received   atomic.Uint64
	sent       atomic.Uint64
	sentFlood  atomic.Uint64
	sentDirect atomic.Uint64
	lastRSSI   atomic.Int32
	lastSNR    atomic.Int32
	haveSignal atomic.Bool
}

func NewRepeater(id meshcore.LocalIdentity, radio node.Radio, cfg Config) (*Service, error) {
	return newService(id, radio, cfg, false)
}

func NewRoom(id meshcore.LocalIdentity, radio node.Radio, cfg Config) (*Service, error) {
	return newService(id, radio, cfg, true)
}

func (s *Service) Name() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.state.Name
}

func newService(id meshcore.LocalIdentity, radio node.Radio, cfg Config, room bool) (*Service, error) {
	if radio == nil || id.Identity.IsZero() || cfg.StateDir == "" || cfg.Name == "" {
		return nil, errors.New("role requires identity, radio, state directory and advert name")
	}
	if len(cfg.Name) > 31 || !utf8.ValidString(cfg.Name) || bytes.IndexByte([]byte(cfg.Name), 0) >= 0 {
		return nil, errors.New("advert name must be 1–31 bytes without NUL")
	}
	if cfg.AdvertInterval < 0 || cfg.AdvertInterval%time.Second != 0 || cfg.AdvertInterval > 255*time.Hour {
		return nil, errors.New("advert interval must be nonnegative whole seconds, at most 255 hours")
	}
	if cfg.MaxHistory == 0 {
		cfg.MaxHistory = 32
	}
	if cfg.MaxMembers == 0 {
		cfg.MaxMembers = 20
		if !room {
			cfg.MaxMembers = 32
		}
	}
	if cfg.MaxNeighbours == 0 {
		cfg.MaxNeighbours = 50
	}
	if cfg.MaxNeighbours < 1 || cfg.MaxNeighbours > 1024 {
		return nil, errors.New("neighbour capacity must be 1-1024")
	}
	if cfg.MaxLogBytes == 0 {
		cfg.MaxLogBytes = 4 << 20
	}
	if cfg.MaxLogBytes < 1024 || cfg.MaxLogBytes > 16<<20 {
		return nil, errors.New("packet log limit must be 1 KiB-16 MiB")
	}
	if cfg.MaxHistory < 1 || cfg.MaxHistory > 4096 || cfg.MaxMembers < 1 || cfg.MaxMembers > 256 {
		return nil, errors.New("role limits: history 1–4096, members 1–256")
	}
	if cfg.Retention != "" && cfg.Retention != NativeRetention && cfg.Retention != DurableReplay {
		return nil, errors.New("unknown role retention profile")
	}
	if cfg.PreferenceProfile != "" && cfg.PreferenceProfile != NativePreferences && cfg.PreferenceProfile != DurableHostPreferences {
		return nil, errors.New("unknown role preference profile")
	}
	for _, password := range []string{cfg.Password, cfg.AdminPassword} {
		if len(password) > 63 || bytes.IndexByte([]byte(password), 0) >= 0 {
			return nil, errors.New("password must be at most 63 bytes without NUL")
		}
	}
	s := &Service{
		cfg: cfg, id: id, room: room, started: time.Now(),
		queue: make(chan event, 64), done: make(chan struct{}), exited: make(chan struct{}),
		postDelay: 6 * time.Second,
		random:    rand.Read,
		writeJSON: hoststate.WriteRoleJSON,
	}
	s.profile = policy.Repeater
	if room {
		s.profile = policy.Room
	}
	if err := s.load(); err != nil {
		return nil, err
	}
	s.initialPosted, s.initialPushed = s.state.Posted, s.state.Pushed
	prefs := s.state.Preferences
	overrides := cfg.Policy
	if cfg.AdvertInterval > 0 && overrides.FloodAdvertSeconds == nil {
		seconds := uint32(cfg.AdvertInterval / time.Second)
		overrides.FloodAdvertSeconds = &seconds
	}
	var err error
	s.state.Preferences, err = policy.ApplyOverrides(s.profile, prefs, overrides)
	if err != nil {
		return nil, err
	}
	if cfg.Policy.DefaultScope != nil {
		s.state.ManagedDefaultRegion = false
	}
	if s.state.ManagedDefaultRegion {
		s.state.Preferences.DefaultScope, err = managedDefaultScope(s.state.Preferences, s.state.DefaultRegion)
		if err != nil {
			return nil, err
		}
	}
	if s.state.NextRegionID == 0 {
		s.state.NextRegionID = 1
	}
	for _, region := range s.state.Preferences.Regions {
		if uint32(region.ID) >= s.state.NextRegionID {
			s.state.NextRegionID = uint32(region.ID) + 1
		}
	}
	if err := s.validatePolicyInputs(s.state.Preferences); err != nil {
		return nil, err
	}
	if source, ok := radio.(interface {
		SetSourceAirtimeFactor(context.Context, float64) error
	}); ok {
		s.sourcePolicy = source.SetSourceAirtimeFactor
	} else if s.state.Preferences.AirtimeFactor != 1 {
		return nil, errors.New("nondefault airtime factor requires physical source-policy management")
	}
	if err := s.save(); err != nil {
		return nil, err
	}
	if s.sourcePolicy != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		err := s.sourcePolicy(ctx, float64(s.state.Preferences.AirtimeFactor))
		cancel()
		if err != nil {
			return nil, fmt.Errorf("applying role source policy: %w", err)
		}
	}
	snapshot := s.state.Preferences
	s.policySnapshot.Store(&snapshot)
	s.adverts = policy.NewAdvertisementSchedule(time.Now(), s.state.Preferences)
	observed := &observedRadio{Radio: radio, service: s}
	var input node.Radio = observed
	if tx, ok := radio.(node.TxRadio); ok {
		input = &observedTxRadio{observedRadio: observed, tx: tx}
	}
	radio.AddOutboundHandler(s.observeOutbound)
	s.Node = node.New(id, input,
		node.WithMaxPeers(128),
		node.WithLearnedPathsOnly(),
		node.WithAirtimeEstimator(cfg.AirtimeEstimator),
		node.WithErrorHandler(s.report),
		node.WithAllowForwardHandler(s.allowForward),
		node.WithAllowPacketHandler(localReflection),
		node.WithRxDelay(s.rxDelay),
		node.WithFloodRetransmitDelay(s.retransmitDelay(false)),
		node.WithDirectRetransmitDelay(s.retransmitDelay(true)),
		node.WithExtraAckTransmitCount(func() uint8 {
			s.mu.RLock()
			defer s.mu.RUnlock()
			return s.state.MultiACKs
		}),
	)
	for _, typ := range []byte{meshcore.PayloadTypeAnonReq, meshcore.PayloadTypeReq,
		meshcore.PayloadTypeTxtMsg, meshcore.PayloadTypePath, meshcore.PayloadTypeAck,
		meshcore.PayloadTypeMultiPart, meshcore.PayloadTypeControl, meshcore.PayloadTypeAdvert} {
		s.Node.OnPacket(typ, s.receive)
	}
	startup, err := s.advertisement(false)
	if err == nil {
		err = s.save()
	}
	if err != nil {
		s.Node.Stop()
		return nil, err
	}
	s.transmit(delayed(startup, 16*time.Second), nil)
	go s.run()
	return s, nil
}

func localReflection(p *meshcore.Packet) bool { return p.SNR == -32 && p.RSSI == 127 }

type observedRadio struct {
	node.Radio
	service *Service
}

func (r *observedRadio) Close() error {
	err := r.Radio.Close()
	r.service.mu.Lock()
	r.service.radioCloseErr = err
	r.service.mu.Unlock()
	return err
}

func (r *observedRadio) SetDataHandler(h func(*meshcore.Packet)) {
	r.Radio.SetDataHandler(func(p *meshcore.Packet) {
		r.service.captureLog("RX", p)
		if !localReflection(p) {
			r.service.received.Add(1)
			if p.HasSignalInfo {
				r.service.lastRSSI.Store(int32(p.RSSI))
				r.service.lastSNR.Store(int32(p.SNR * 4))
				r.service.haveSignal.Store(true)
			}
		}
		h(p)
	})
}

type observedTxRadio struct {
	*observedRadio
	tx node.TxRadio
}

func (r *observedTxRadio) Enqueue(b []byte, priority uint8, delay time.Duration) bool {
	return r.tx.Enqueue(b, priority, delay)
}
func (r *observedTxRadio) TxQueueLen() int { return r.tx.TxQueueLen() }

func (s *Service) observeOutbound(data []byte) {
	if len(data) == 0 {
		return
	}
	if s.logging.Load() {
		p, err := meshcore.PacketFromBytes(data)
		if err != nil {
			s.report(fmt.Errorf("logging transmitted packet: %w", err))
		} else {
			s.captureLog("TX", p)
		}
	}
	s.sent.Add(1)
	switch data[0] & meshcore.PacketRouteMask {
	case meshcore.RouteTypeFlood, meshcore.RouteTypeTransportFlood:
		s.sentFlood.Add(1)
	default:
		s.sentDirect.Add(1)
	}
}

func (s *Service) receive(p *meshcore.Packet) {
	s.ingress.RLock()
	defer s.ingress.RUnlock()
	select {
	case <-s.done:
		return
	default:
	}
	ctx, err := s.receiveContext(p)
	if err != nil {
		s.report(err)
		return
	}
	e := event{packet: p, context: ctx}
	switch p.PayloadType() {
	case meshcore.PayloadTypeControl, meshcore.PayloadTypeAdvert:
		if s.room {
			return
		}
		e.plain = bytes.Clone(p.Payload)
	case meshcore.PayloadTypeAck, meshcore.PayloadTypeMultiPart:
		data := p.Payload
		if p.PayloadType() == meshcore.PayloadTypeMultiPart {
			mp, err := meshcore.MultiPartFromBytes(data)
			if err != nil || mp.WrappedType != meshcore.PayloadTypeAck {
				return
			}
			data = mp.WrappedPayload
		}
		if len(data) < 4 {
			return
		}
		e.plain = bytes.Clone(data[:4])
		s.mu.RLock()
		ours := s.pendingACK(u32(data))
		s.mu.RUnlock()
		if !ours {
			return
		}
	case meshcore.PayloadTypeAnonReq:
		a, err := meshcore.AnonReqFromBytes(p.Payload)
		if err != nil || a.Destination != s.id.PublicKey()[0] {
			return
		}
		peer := meshcore.NewIdentity(a.EphemeralPubKey)
		secret, err := s.Node.SharedSecret(peer)
		if err != nil {
			return
		}
		e.plain = a.Decrypt(secret)
		e.key = peer.String()
	default:
		d, err := meshcore.RequestFromBytes(p.Payload)
		if err != nil || d.Destination != s.id.PublicKey()[0] {
			return
		}
		s.mu.RLock()
		keys := make([][32]byte, 0, len(s.state.Members))
		for _, member := range s.state.Members {
			if member.Key[0] == d.Source {
				keys = append(keys, member.Key)
			}
		}
		s.mu.RUnlock()
		for _, key := range keys {
			id := meshcore.NewIdentity(key)
			secret, err := s.Node.SharedSecret(id)
			if err == nil {
				if plain := d.Decrypt(secret); plain != nil {
					e.key, e.plain = id.String(), plain
					break
				}
			}
		}
	}
	if e.plain == nil {
		return
	}
	if p.PayloadType() != meshcore.PayloadTypeAdvert && p.PayloadType() != meshcore.PayloadTypeControl {
		p.MarkDoNotRetransmit()
	}
	e.packet = p.Clone()
	select {
	case s.queue <- e:
	default:
		s.report(ErrQueueFull)
	}
}

func (s *Service) run() {
	defer close(s.exited)
	ticker := time.NewTicker(150 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-s.done:
			for {
				select {
				case e := <-s.queue:
					s.process(e)
				default:
					return
				}
			}
		case e := <-s.queue:
			s.process(e)
		case now := <-ticker.C:
			s.mu.Lock()
			next, kind := s.adverts.Poll(now, s.state.Preferences)
			s.adverts = next
			var adverts []transmission
			var advertErr error
			if kind != policy.AdvertNone && !s.commitUncertain && !s.restartPending {
				var packet *meshcore.Packet
				packet, advertErr = s.advertisement(kind == policy.AdvertFlood)
				adverts = one(packet)
				if advertErr == nil {
					advertErr = s.save()
					if advertErr != nil {
						s.lastErr = advertErr
						if errors.Is(advertErr, ErrCommitUncertain) {
							s.failClosed(advertErr)
						}
					}
				}
			}
			failed := s.commitUncertain || s.restartPending
			s.mu.Unlock()
			s.transmit(adverts, advertErr)
			if s.room && !failed {
				s.mu.Lock()
				packets, err := s.syncAt(now)
				s.mu.Unlock()
				s.transmit(packets, err)
			}
		}
	}
}

func (s *Service) process(e event) {
	if e.ownerCommand != nil {
		e.ownerCommand.err = ErrRoleStopped
		defer func() {
			e.ownerCommand.result <- ownerCommandResult{e.ownerCommand.reply, e.ownerCommand.err}
		}()
		if err := e.ownerCommand.ctx.Err(); err != nil {
			e.ownerCommand.err = err
			return
		}
	}
	defer func() {
		if count := s.droppedLogs.Swap(0); count != 0 {
			s.report(fmt.Errorf("packet log dropped %d records: %w", count, ErrQueueFull))
		}
	}()
	if e.log != nil {
		s.mu.Lock()
		err := s.appendPacketLog(*e.log)
		if err != nil {
			s.state.Logging = false
			s.logging.Store(false)
		}
		s.mu.Unlock()
		if err != nil {
			s.report(err)
		}
		return
	}
	nativeTelemetry := e.packet != nil && e.packet.PayloadType() == meshcore.PayloadTypeReq && len(e.plain) >= 5 &&
		(e.plain[4] == 1 || e.plain[4] == 3)
	cliTelemetry := e.packet != nil && e.packet.PayloadType() == meshcore.PayloadTypeTxtMsg && len(e.plain) >= 5 &&
		e.plain[4]>>2 == 1 && statsNeedsTelemetry(string(cstring(e.plain[5:])))
	if e.ownerCommand != nil && statsNeedsTelemetry(e.ownerCommand.command) {
		cliTelemetry = true
	}
	if (nativeTelemetry || cliTelemetry) && s.cfg.Telemetry != nil {
		var err error
		e.telemetry, err = s.cfg.Telemetry()
		if err != nil {
			s.report(err)
			e.telemetryErr = err
			if cliTelemetry {
				e.telemetry = Telemetry{}
			}
		}
		if ((e.telemetry.HasAirtime || e.telemetry.HasTXAirtime) && e.telemetry.TXAirtime < 0) ||
			((e.telemetry.HasAirtime || e.telemetry.HasRXAirtime) && e.telemetry.RXAirtime < 0) {
			s.report(errors.New("invalid negative measured RF airtime"))
			if !cliTelemetry {
				return
			}
			e.telemetryErr = errors.New("invalid negative measured RF airtime")
			e.telemetry = Telemetry{}
		}
	}
	s.mu.Lock()
	if s.restartPending {
		s.mu.Unlock()
		return
	}
	if s.commitUncertain {
		s.mu.Unlock()
		if e.ownerCommand != nil {
			e.ownerCommand.err = ErrCommitUncertain
		}
		s.report(ErrCommitUncertain)
		return
	}
	before := s.state.clone()
	s.eraseLog = false
	s.requestRestart, s.restartPrefix = false, ""
	s.identityStage = nil
	packets, dirty, err := s.handle(e)
	if err == nil && dirty {
		err = s.save()
		if err != nil {
			s.lastErr = err
		}
	}
	if err != nil {
		if errors.Is(err, ErrCommitUncertain) {
			s.failClosed(err)
		} else {
			s.state = before
		}
		packets = nil
	}
	factor := s.state.Preferences.AirtimeFactor
	needsSourceUpdate := err == nil && dirty && factor != before.Preferences.AirtimeFactor
	eraseLog := err == nil && dirty && s.eraseLog
	restart := err == nil && dirty && s.requestRestart
	restartPrefix := s.restartPrefix
	stage := s.identityStage
	s.identityStage = nil
	if stage != nil {
		defer clear(stage.key)
	}
	s.eraseLog = false
	s.requestRestart, s.restartPrefix = false, ""
	s.mu.Unlock()
	if needsSourceUpdate {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		sourceErr := s.sourcePolicy(ctx, float64(factor))
		cancel()
		if sourceErr != nil {
			err = errors.Join(ErrCommitUncertain, fmt.Errorf("applying role source policy: %w", sourceErr))
			s.mu.Lock()
			s.failClosed(err)
			s.mu.Unlock()
			packets = nil
		}
	}
	if err == nil && eraseLog {
		s.mu.Lock()
		if eraseErr := s.erasePacketLog(); eraseErr != nil {
			err = eraseErr
			if errors.Is(err, ErrCommitUncertain) {
				s.failClosed(err)
			}
			packets = nil
		}
		s.mu.Unlock()
	}
	if err == nil && dirty {
		s.mu.Lock()
		snapshot := s.state.Preferences
		s.policySnapshot.Store(&snapshot)
		s.logging.Store(s.state.Logging)
		if snapshot.LocalAdvertSeconds != before.Preferences.LocalAdvertSeconds {
			s.adverts = s.adverts.RescheduleLocal(time.Now(), snapshot)
		}
		if snapshot.FloodAdvertSeconds != before.Preferences.FloodAdvertSeconds {
			s.adverts = s.adverts.RescheduleFlood(time.Now(), snapshot)
		}
		s.mu.Unlock()
	}
	s.transmit(packets, err)
	if restart && err == nil {
		s.admitRestart(e, restartPrefix)
	}
	if stage != nil && err == nil {
		s.stageIdentity(e, stage)
	}
	if e.ownerCommand != nil {
		e.ownerCommand.err = err
		if err != nil {
			e.ownerCommand.reply = ""
		}
	}
}

func (s *Service) transmit(packets []transmission, err error) {
	if err != nil {
		s.report(err)
		return
	}
	for _, packet := range packets {
		priority, err := policy.OriginatedPriority(policy.Route(packet.RouteType()), packet.PayloadType(), false)
		if err != nil {
			s.report(err)
			return
		}
		if err := s.Node.SendPacketDelayed(packet.Packet, priority, packet.delay); err != nil {
			s.report(err)
			return
		}
	}
}

// ApplicationError returns the sticky terminal fail-closed cause, or nil.
// It is safe to poll without waiting for application locks or storage I/O.
// Recoverable packet, queue, telemetry and known-aborted errors are excluded.
func (s *Service) ApplicationError() error {
	if err := s.terminalError.Load(); err != nil {
		return *err
	}
	return nil
}

// The caller holds mu. Only a new service lifetime clears a terminal fault.
func (s *Service) failClosed(err error) {
	s.commitUncertain, s.lastErr = true, err
	s.terminalError.CompareAndSwap(nil, &err)
}

func (s *Service) report(err error) {
	if s.cfg.ErrorHandler != nil {
		s.cfg.ErrorHandler(fmt.Errorf("host role: %w", err))
	} else {
		slog.Error("host role", "error", err)
	}
}

func (s *Service) Close() error {
	s.close.Do(func() {
		s.ingress.Lock()
		close(s.done)
		s.ingress.Unlock()
		<-s.exited
		s.Node.Stop()
		s.mu.RLock()
		s.closeErr = errors.Join(s.lastErr, s.radioCloseErr)
		s.mu.RUnlock()
	})
	return s.closeErr
}
