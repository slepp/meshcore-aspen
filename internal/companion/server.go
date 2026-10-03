package companion

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/policy"
	hoststate "meshcore.local/meshcore/internal/state"
)

const (
	maxContacts        = 350
	maxChannels        = 40
	maxMessages        = 256
	maxClients         = 32
	maxPendingRequests = 128
	frameReadTimeout   = 10 * time.Second
)

type RetentionProfile string

const (
	NativeQueue   RetentionProfile = "native-queue"
	DurableReplay RetentionProfile = "durable-replay"
)

type Config struct {
	StateDir       string
	Name           string
	AdvertInterval time.Duration
	RadioConfig    hardware.RadioConfig
	TxPower        int8
	// EffectivePHY optionally reports the shared PHY currently verified on the
	// companion's source. When it returns true, SELF_INFO, radio setting checks
	// and RX scoring use it; otherwise RadioConfig/TxPower apply.
	EffectivePHY        func() (hardware.RadioConfig, int8, bool)
	AirtimeEstimator    node.AirtimeEstimator
	ErrorHandler        func(error)
	Battery             func(context.Context) (uint16, error)
	Stats               func(context.Context, byte) (protocol.StatsResponse, error)
	SensorTelemetry     func(context.Context, byte) ([]byte, error)
	Storage             func(context.Context) (usedKB, totalKB uint32, err error)
	ExportIdentity      func(context.Context) ([]byte, error)
	ImportIdentity      func(context.Context, []byte, json.RawMessage) (meshcore.LocalIdentity, error)
	RequestRestart      func(context.Context) error
	RequestFactoryReset func(context.Context) error
	Policy              policy.Overrides
	Retention           RetentionProfile
}

type contact struct {
	protocol.ContactResponse
	Advert       []byte
	SyncSince    uint32
	HeardPath    []byte
	HeardPathLen byte
	HeardAt      uint32
}

type message struct {
	Sequence uint64
	Frame    []byte // Stored in V3 form; converted for each client's negotiated version.
	Digest   [32]byte
}

type state struct {
	Version              int
	PublicKey            [32]byte
	Name                 string
	Latitude, Longitude  int32
	ClockOffset          int64
	ManualAdd            byte
	AdvertLocationPolicy byte
	AutoAddConfig        byte
	AutoAddMaxHops       byte
	TelemetryModes       byte
	MultiACKs            byte
	Contacts             []contact
	Channels             [maxChannels]*meshcore.ChannelEntry
	Messages             []message
	Sequence             uint64
	LastModified         uint32
	Preferences          policy.Preferences
	Retention            RetentionProfile
}

type pending struct {
	Client  *session
	Key     [32]byte
	Kind    byte
	Expires time.Time
}

type loginFence struct {
	Key     [32]byte
	Expires time.Time
}

type receivedPacket struct {
	Packet  *meshcore.Packet
	Context policy.ReceiveContext
}

type connection struct {
	Interval               time.Duration
	LastActivity, NextPing time.Time
	ACK                    uint32
	Owners                 map[*session]struct{}
}

type Server struct {
	Node          *node.Node
	cfg           Config
	mu            sync.Mutex // Serializes commands, durable state, and received radio events.
	state         state
	clients       map[*session]struct{}
	pending       map[uint32]pending
	loginFences   map[uint32]loginFence
	connections   map[[32]byte]*connection
	transient     map[[32]byte]*contact
	acks          map[uint32]time.Time
	traces        map[uint64]traceRequest
	diagnostics   chan []byte
	advertPaths   map[[32]byte]advertPath
	nextTag       uint32
	packets       chan receivedPacket
	overflow      chan struct{}
	done          chan struct{}
	workerDone    chan struct{}
	listener      net.Listener
	closeOnce     sync.Once
	finalizeOnce  sync.Once
	closeErr      error
	storageFault  error
	faulted       atomic.Bool
	terminalError atomic.Pointer[error]
	ctx           context.Context
	cancel        context.CancelFunc
	wg            sync.WaitGroup
	preferences   atomic.Pointer[policy.Preferences]
	identity      atomic.Pointer[meshcore.LocalIdentity]
	multiACKs     atomic.Uint32
}

type session struct {
	server   *Server
	conn     net.Conn
	out      chan []byte
	done     chan struct{}
	once     sync.Once
	version  byte
	cursor   uint64
	signData []byte
	scope    policy.SendScope
}

func New(id meshcore.LocalIdentity, radio node.Radio, cfg Config) (*Server, error) {
	if radio == nil {
		return nil, errors.New("companion: nil radio")
	}
	if _, ok := radio.(node.TxRadio); !ok {
		return nil, errors.New("companion parity requires a scheduled TxRadio")
	}
	if cfg.Name == "" {
		cfg.Name = "MeshCore host"
	}
	if !utf8.ValidString(cfg.Name) || bytes.IndexByte([]byte(cfg.Name), 0) >= 0 {
		return nil, errors.New("companion: advert name must be valid UTF-8 without NUL")
	}
	if cfg.AdvertInterval < 0 {
		return nil, errors.New("companion: negative advert interval")
	}
	if cfg.ImportIdentity != nil && cfg.StateDir == "" {
		return nil, errors.New("companion: identity import requires persistent role state")
	}
	s := &Server{
		cfg: cfg, clients: make(map[*session]struct{}), pending: make(map[uint32]pending),
		loginFences: make(map[uint32]loginFence),
		connections: make(map[[32]byte]*connection), packets: make(chan receivedPacket, 256),
		transient: make(map[[32]byte]*contact),
		traces:    make(map[uint64]traceRequest), diagnostics: make(chan []byte, 256),
		advertPaths: make(map[[32]byte]advertPath),
		acks:        make(map[uint32]time.Time),
		overflow:    make(chan struct{}, 1), done: make(chan struct{}), workerDone: make(chan struct{}),
		state: state{Version: 1, PublicKey: id.PublicKey(), Name: meshcore.TruncateUTF8(cfg.Name, 31), Retention: NativeQueue},
	}
	public, _ := meshcore.NewChannelFromBase64("Public", "izOH6cXN6mrJ5e26oRXNcg==")
	s.state.Channels[0] = public
	if cfg.StateDir != "" {
		if err := os.MkdirAll(cfg.StateDir, 0700); err != nil {
			return nil, err
		}
		data, err := hoststate.ReadRoleJSON(filepath.Join(cfg.StateDir, "companion.json"))
		if err == nil {
			var loaded state
			if err := json.Unmarshal(data, &loaded); err != nil {
				return nil, fmt.Errorf("companion state: %w", err)
			}
			s.state = loaded
			if s.state.Retention == "" {
				s.state.Retention = DurableReplay
			}
			if s.state.Version != 1 || s.state.PublicKey != id.PublicKey() {
				return nil, errors.New("companion state version or identity mismatch")
			}
			if len(s.state.Contacts) > maxContacts || len(s.state.Messages) > maxMessages {
				return nil, errors.New("companion state exceeds capacity")
			}
			for _, c := range s.state.Contacts {
				if _, _, ok := decodePath(c.OutPathLen); !ok {
					return nil, errors.New("companion state has invalid path")
				}
			}
			var previous uint64
			for _, msg := range s.state.Messages {
				if msg.Sequence == 0 || msg.Sequence <= previous || msg.Sequence > s.state.Sequence ||
					len(msg.Frame) > protocol.MaxFrameSize || len(msg.Frame) == 0 {
					return nil, errors.New("companion state has invalid message journal")
				}
				if msg.Frame[0] != protocol.RespContactMsgRecvV3 && msg.Frame[0] != protocol.RespChannelMsgRecvV3 && msg.Frame[0] != protocol.RespChannelDataRecv {
					return nil, errors.New("companion state has unsupported message format")
				}
				if _, err := protocol.ParseResponse(msg.Frame); err != nil {
					return nil, fmt.Errorf("companion state message: %w", err)
				}
				previous = msg.Sequence
			}
			if len(s.state.Name) == 0 || len(s.state.Name) > 31 ||
				!utf8.ValidString(s.state.Name) || bytes.IndexByte([]byte(s.state.Name), 0) >= 0 {
				return nil, errors.New("companion state has invalid advert name")
			}
		} else if !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
	}
	if s.state.Preferences.Version == 0 {
		s.state.Preferences = policy.Defaults(policy.Companion)
	}
	contacts := make([]contact, 0, len(s.state.Contacts))
	for _, entry := range s.state.Contacts {
		if entry.Type == 0 {
			c := entry
			s.addTransient(&c)
		} else {
			contacts = append(contacts, entry)
		}
	}
	s.state.Contacts = contacts
	if cfg.Retention != "" {
		s.state.Retention = cfg.Retention
	}
	if s.state.Retention != NativeQueue && s.state.Retention != DurableReplay {
		return nil, errors.New("unknown companion retention profile")
	}
	if cfg.AdvertInterval > 0 && cfg.Policy.FloodAdvertSeconds == nil {
		if cfg.AdvertInterval/time.Second > 255*3600 {
			return nil, errors.New("companion advert interval exceeds native range")
		}
		seconds := uint32(cfg.AdvertInterval / time.Second)
		if seconds == 0 {
			return nil, errors.New("companion advert interval must be at least one second")
		}
		cfg.Policy.FloodAdvertSeconds = &seconds
	}
	prefs, err := policy.ApplyOverrides(policy.Companion, s.state.Preferences, cfg.Policy)
	if err != nil {
		return nil, err
	}
	source, hasSource := radio.(sourceAirtime)
	if prefs.AirtimeFactor != 1 && !hasSource {
		return nil, errors.New("configured companion airtime factor requires a PHY source-policy API")
	}
	s.state.Preferences = prefs
	s.identity.Store(&id)
	s.publishPreferences()
	if err := s.save(); err != nil {
		return nil, err
	}
	if hasSource {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		err := source.SetSourceAirtimeFactor(ctx, float64(prefs.AirtimeFactor))
		cancel()
		if err != nil {
			return nil, fmt.Errorf("companion source airtime policy: %w", err)
		}
	}
	opts := []node.Option{node.WithMaxPeers(maxContacts + 8), node.WithMaxChannels(maxChannels),
		node.WithLearnedPathsOnly(), node.WithAllowForwardHandler(func(*meshcore.Packet) bool { return false }),
		node.WithAllowPacketHandler(func(pkt *meshcore.Packet) bool {
			return pkt.HasSignalInfo && pkt.SNR == -32 && pkt.RSSI == 127
		}),
		node.WithErrorHandler(s.report)}
	if cfg.AirtimeEstimator != nil {
		opts = append(opts, node.WithAirtimeEstimator(cfg.AirtimeEstimator))
	}
	opts = append(opts, s.nodePolicyOptions()...)
	s.ctx, s.cancel = context.WithCancel(context.Background())
	s.Node = node.New(id, radio, opts...)
	s.hydrate()
	for _, typ := range []byte{meshcore.PayloadTypeAdvert, meshcore.PayloadTypeTxtMsg, meshcore.PayloadTypeGrpTxt, meshcore.PayloadTypePath, meshcore.PayloadTypeResponse, meshcore.PayloadTypeAck, meshcore.PayloadTypeMultiPart, meshcore.PayloadTypeGrpData, meshcore.PayloadTypeTrace, meshcore.PayloadTypeRawCustom, meshcore.PayloadTypeControl, meshcore.PayloadTypeReq} {
		s.Node.OnPacket(typ, s.receive)
	}
	radio.SetRawDataHandler(s.rawReceive)
	go s.run()
	return s, nil
}

func (s *Server) report(err error) {
	if err == nil {
		return
	}
	if s.cfg.ErrorHandler != nil {
		s.cfg.ErrorHandler(err)
	} else {
		slog.Error("host companion", "error", err)
	}
}

func (s *Server) save() error {
	if s.storageFault != nil {
		return s.storageFault
	}
	if s.cfg.StateDir == "" {
		return nil
	}
	err := hoststate.WriteRoleJSON(filepath.Join(s.cfg.StateDir, "companion.json"), s.state)
	if errors.Is(err, hoststate.ErrCommitIndeterminate) {
		s.failClosed(err)
	}
	return err
}

// ApplicationError returns the sticky terminal fail-closed cause, or nil.
// It is safe to poll without acquiring the command mutex. Recoverable command,
// packet and telemetry errors are not terminal application faults.
func (s *Server) ApplicationError() error {
	if err := s.terminalError.Load(); err != nil {
		return *err
	}
	return nil
}

func (s *Server) failClosed(err error) {
	s.storageFault = err
	s.faulted.Store(true)
	s.terminalError.CompareAndSwap(nil, &err)
}

func (s *Server) hydrate() {
	for _, p := range s.Node.Peers().Peers() {
		s.Node.Peers().Remove(p.Identity.PublicKey())
	}
	for i := range s.state.Contacts {
		s.hydrateContact(&s.state.Contacts[i])
	}
	for _, c := range s.transient {
		s.hydrateContact(c)
	}
	for i, ch := range s.state.Channels {
		if ch == nil || ch.Name == "" {
			s.Node.RemoveChannel(i)
		} else {
			valid, _ := meshcore.NewChannelFromPSK(ch.Name, ch.PSK[:])
			s.Node.SetChannel(i, valid)
		}
	}
}

func (s *Server) hydrateContact(c *contact) {
	size, count, _ := decodePath(c.OutPathLen)
	var path []byte
	if c.OutPathLen != 255 {
		path = append([]byte{}, c.OutPath[:size*count]...)
	}
	s.Node.Peers().Insert(&node.Peer{Identity: c.Identity(), Name: c.AdvertName,
		OutPath: path, OutPathHashSize: byte(size), LastAdvertTimestamp: c.LastAdvert,
		LastSeen: time.Unix(int64(c.LastModified), 0)})
}

// Serve owns listener until cancellation or Close. Only one listener may be served.
func (s *Server) Serve(ctx context.Context, listener net.Listener) (serveErr error) {
	s.mu.Lock()
	select {
	case <-s.done:
		s.mu.Unlock()
		_ = listener.Close()
		return net.ErrClosed
	default:
	}
	if s.listener != nil {
		s.mu.Unlock()
		return errors.New("companion: already serving")
	}
	s.listener = listener
	s.mu.Unlock()
	defer func() { serveErr = errors.Join(serveErr, s.Close()) }()
	stop := context.AfterFunc(ctx, func() { _ = listener.Close() })
	defer stop()
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			select {
			case <-s.done:
				return nil
			default:
				return err
			}
		}
		s.mu.Lock()
		select {
		case <-s.done:
			s.mu.Unlock()
			_ = conn.Close()
			return nil
		default:
		}
		if len(s.clients) >= maxClients {
			s.mu.Unlock()
			_ = conn.Close()
			continue
		}
		c := &session{server: s, conn: conn, out: make(chan []byte, 512), done: make(chan struct{})}
		if len(s.state.Messages) > 0 {
			c.cursor = s.state.Messages[0].Sequence - 1
		} else {
			c.cursor = s.state.Sequence
		}
		s.clients[c] = struct{}{}
		s.wg.Add(1)
		s.mu.Unlock()
		go c.run()
	}
}

func (s *Server) Close() error {
	s.closeOnce.Do(func() {
		close(s.done)
		s.cancel()
		s.mu.Lock()
		if s.listener != nil {
			_ = s.listener.Close()
		}
		for c := range s.clients {
			c.close()
		}
		s.mu.Unlock()
		s.Node.Stop()
	})
	<-s.workerDone
	s.wg.Wait()
	s.finalizeOnce.Do(func() {
		s.mu.Lock()
		defer s.mu.Unlock()
		s.closeErr = s.save()
	})
	return s.closeErr
}

func (c *session) close() { c.once.Do(func() { close(c.done); _ = c.conn.Close() }) }
func (c *session) send(frame []byte) {
	select {
	case <-c.done:
		return
	default:
	}
	select {
	case c.out <- append([]byte(nil), frame...):
	default:
		c.close()
	}
}

func (c *session) run() {
	defer c.server.wg.Done()
	defer func() {
		c.close()
		c.server.mu.Lock()
		delete(c.server.clients, c)
		for key, conn := range c.server.connections {
			delete(conn.Owners, c)
			if len(conn.Owners) == 0 {
				delete(c.server.connections, key)
			}
		}
		for tag, p := range c.server.pending {
			if p.Client == c {
				delete(c.server.pending, tag)
			}
			for tag, p := range c.server.traces {
				if p.Client == c {
					delete(c.server.traces, tag)
				}
			}
		}
		c.server.mu.Unlock()
	}()
	writerDone := make(chan struct{})
	go func() {
		defer close(writerDone)
		defer c.close()
		for {
			select {
			case <-c.done:
				return
			case f := <-c.out:
				wire := make([]byte, 3+len(f))
				wire[0] = '>'
				binary.LittleEndian.PutUint16(wire[1:3], uint16(len(f)))
				copy(wire[3:], f)
				if err := c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second)); err != nil {
					c.server.report(fmt.Errorf("companion write deadline: %w", err))
					return
				}
				for len(wire) > 0 {
					n, err := c.conn.Write(wire)
					if err != nil || n == 0 {
						return
					}
					wire = wire[n:]
				}
			}
		}
	}()
	defer func() { c.close(); <-writerDone }()
	initial := true
	for {
		var header [3]byte
		if !initial {
			if _, err := io.ReadFull(c.conn, header[:1]); err != nil {
				return
			}
		}
		if err := c.conn.SetReadDeadline(time.Now().Add(frameReadTimeout)); err != nil {
			c.server.report(fmt.Errorf("companion frame deadline: %w", err))
			return
		}
		offset := 0
		if !initial {
			offset = 1
		}
		if _, err := io.ReadFull(c.conn, header[offset:]); err != nil {
			if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				c.server.report(fmt.Errorf("companion header read: %w", err))
			}
			return
		}
		n := int(binary.LittleEndian.Uint16(header[1:]))
		if header[0] != '<' || n == 0 || n > protocol.MaxFrameSize {
			return
		}
		frame := make([]byte, n)
		if _, err := io.ReadFull(c.conn, frame); err != nil {
			if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				c.server.report(fmt.Errorf("companion frame body read: %w", err))
			}
			return
		}
		if err := c.conn.SetReadDeadline(time.Time{}); err != nil {
			c.server.report(fmt.Errorf("companion idle deadline: %w", err))
			return
		}
		initial = false
		c.server.mu.Lock()
		select {
		case <-c.server.done:
			c.server.mu.Unlock()
			return
		default:
		}
		c.server.command(c, frame)
		c.server.mu.Unlock()
	}
}

func (s *Server) receive(pkt *meshcore.Packet) {
	cp := *pkt
	cp.Payload = append([]byte(nil), pkt.Payload...)
	cp.Path = append([]byte(nil), pkt.Path...)
	select {
	case <-s.done:
		return
	default:
	}
	select {
	case s.packets <- receivedPacket{Packet: &cp, Context: s.receiveContext(pkt)}:
	default:
		s.signalOverflow()
	}
}

func (s *Server) signalOverflow() {
	select {
	case s.overflow <- struct{}{}:
	default:
	}
}

func (s *Server) run() {
	defer close(s.workerDone)
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	s.mu.Lock()
	select {
	case <-s.done:
		s.mu.Unlock()
		return
	default:
		if s.state.Preferences.FloodAdvertSeconds > 0 {
			s.report(s.sendAdvert(true))
		} else if s.state.Preferences.LocalAdvertSeconds > 0 {
			s.report(s.sendAdvert(false))
		}
	}
	schedule := policy.NewAdvertisementSchedule(time.Now(), s.state.Preferences)
	s.mu.Unlock()
	for {
		select {
		case <-s.done:
			return
		case <-s.overflow:
			s.mu.Lock()
			for c := range s.clients {
				c.close()
			}
			s.mu.Unlock()
			s.report(errors.New("companion receive queue overflow; clients disconnected"))
		case pkt := <-s.packets:
			s.mu.Lock()
			s.handlePacket(pkt.Packet, pkt.Context)
			s.mu.Unlock()
		case frame := <-s.diagnostics:
			s.mu.Lock()
			s.broadcast(frame)
			s.mu.Unlock()
		case now := <-tick.C:
			s.mu.Lock()
			s.expireLoginFences(now)
			for tag, p := range s.pending {
				if now.After(p.Expires) {
					delete(s.pending, tag)
				}
				for tag, p := range s.traces {
					if now.After(p.Expires) {
						delete(s.traces, tag)
					}
				}
			}
			for crc, at := range s.acks {
				if now.Sub(at) > 2*time.Minute {
					delete(s.acks, crc)
				}
			}
			s.keepAlive(now)
			var kind policy.AdvertKind
			schedule, kind = schedule.Poll(now, s.state.Preferences)
			if kind != policy.AdvertNone {
				s.report(s.sendAdvert(kind == policy.AdvertFlood))
			}
			s.mu.Unlock()
		}
	}
}

func (s *Server) broadcast(frame []byte) {
	for c := range s.clients {
		c.send(frame)
	}
}
func (s *Server) now() uint32 { return uint32(time.Now().Unix() + s.state.ClockOffset) }
func (s *Server) modified() uint32 {
	now := s.now()
	if now <= s.state.LastModified {
		now = s.state.LastModified + 1
	}
	s.state.LastModified = now
	return now
}
func put32(b []byte, v uint32) { binary.LittleEndian.PutUint32(b, v) }
func u32(b []byte) uint32      { return binary.LittleEndian.Uint32(b) }
func decodePath(v byte) (size, count int, ok bool) {
	if v == 255 {
		return 1, 0, true
	}
	size, count = int(v>>6)+1, int(v&63)
	return size, count, meshcore.IsValidPathLen(v) && size*count <= 64
}

// phy returns the radio settings the companion reports and checks against.
func (s *Server) phy() (hardware.RadioConfig, int8) {
	if s.cfg.EffectivePHY != nil {
		if radio, power, ok := s.cfg.EffectivePHY(); ok {
			return radio, power
		}
	}
	return s.cfg.RadioConfig, s.cfg.TxPower
}
