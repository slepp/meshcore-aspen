package radio

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

var ErrSubportsUnavailable = errors.New("KISS session subports unavailable")
var ErrSubportsBusy = errors.New("KISS aggregate session busy; retry connection")
var ErrSubportsProbeTimeout = errors.New("KISS session subport probe timed out")
var ErrSubportsInsufficient = errors.New("KISS session has too few subports for the selected roles")
var ErrSessionBackpressure = errors.New("KISS session port output full")
var ErrSessionRefreshLimited = errors.New("KISS session port has not recovered since owner epoch refresh")

const sessionPorts = 4
const sessionQueueDepth = 8

// Session owns one TCP connection and gives each host source its own virtual
// hardware.Transport. Only port 0 may dial; the other ports wait for the
// extended CAPACITY exchange on that physical connection.
type Session struct {
	address          string
	minSlots         int
	minPorts         int
	mu               sync.Mutex
	current          *sessionEpoch
	changed          chan struct{}
	refreshAttempted [sessionPorts]bool
}

type sessionEpoch struct {
	conn        net.Conn
	ports       [sessionPorts]*sessionTransport
	quarantined [sessionPorts]bool
	queues      [sessionPorts]chan sessionWrite
	wake        chan struct{}
	done        chan struct{}
	once        sync.Once
	refreshing  bool
	ready       bool
	cap         ClientCapacity
}

type sessionWrite struct {
	wire []byte
	done chan error
}

type sessionTransport struct {
	session *Session
	port    int
	mu      sync.RWMutex
	epoch   *sessionEpoch
	dead    chan struct{}
	closed  bool
	frame   func(*hardware.KissFrame)
	onError func(error)
}

var _ hardware.Transport = (*sessionTransport)(nil)

func NewSession(address string, minSlots int) *Session {
	return NewSessionWithPorts(address, minSlots, sessionPorts)
}

func NewSessionWithPorts(address string, minSlots, minPorts int) *Session {
	return &Session{address: address, minSlots: minSlots, minPorts: minPorts, changed: make(chan struct{})}
}

// ReserveCapacity fences a reconnect from exposing fewer virtual ports or TCP
// slots than the selected role and direct-client plan uses. Call after CAPACITY
// and before starting child sources.
func (s *Session) ReserveCapacity(ports, slots int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if ports < 1 || ports > sessionPorts || s.current == nil || !s.current.ready ||
		ports > int(s.current.cap.LogicalPorts) || slots < 1 ||
		slots > int(s.current.cap.ExternalTCPSlots) {
		return fmt.Errorf("cannot reserve %d KISS logical ports and %d TCP slots on the negotiated session", ports, slots)
	}
	s.minPorts = max(s.minPorts, ports)
	s.minSlots = max(s.minSlots, slots)
	return nil
}

func (s *Session) Transport(port int) (hardware.Transport, error) {
	if port < 0 || port >= sessionPorts {
		return nil, fmt.Errorf("KISS session port %d outside 0..3", port)
	}
	return &sessionTransport{session: s, port: port, dead: make(chan struct{})}, nil
}

func (s *Session) notify() {
	close(s.changed)
	s.changed = make(chan struct{})
}

func (t *sessionTransport) Connect(ctx context.Context) error {
	t.mu.Lock()
	if t.closed || t.epoch != nil {
		t.mu.Unlock()
		return errors.New("KISS virtual transport already used")
	}
	t.mu.Unlock()
	s := t.session
	if t.port == 0 {
		conn, err := (&net.Dialer{KeepAlive: 15 * time.Second}).DialContext(ctx, "tcp", s.address)
		if err != nil {
			return fmt.Errorf("dial KISS session: %w", err)
		}
		e := &sessionEpoch{conn: conn, wake: make(chan struct{}, 1), done: make(chan struct{})}
		for i := range e.queues {
			e.queues[i] = make(chan sessionWrite, sessionQueueDepth)
		}
		s.mu.Lock()
		if s.current != nil {
			s.mu.Unlock()
			_ = conn.Close()
			return errors.New("KISS session owner already connected")
		}
		s.current = e
		e.ports[0] = t
		t.mu.Lock()
		if t.closed {
			t.mu.Unlock()
			s.mu.Unlock()
			s.fail(e, hardware.ErrDisconnected)
			return hardware.ErrModemClosed
		}
		t.epoch = e
		t.mu.Unlock()
		s.notify()
		s.mu.Unlock()
		go s.read(e)
		go s.write(e)
		return nil
	}
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		s.mu.Lock()
		e := s.current
		if e != nil && e.ready {
			if e.quarantined[t.port] {
				t.mu.RLock()
				closed := t.closed
				t.mu.RUnlock()
				if closed {
					s.mu.Unlock()
					return hardware.ErrModemClosed
				}
				refresh := !e.refreshing
				if refresh {
					if s.refreshAttempted[t.port] {
						s.mu.Unlock()
						return fmt.Errorf("%w: port %d; waiting for an owner reconnect or operator restart",
							ErrSessionRefreshLimited, t.port)
					}
					s.refreshAttempted[t.port] = true
				}
				e.refreshing = true
				changed := s.changed
				s.mu.Unlock()
				if refresh {
					s.fail(e, fmt.Errorf("KISS session port %d needs a fresh owner epoch", t.port))
				} else {
					select {
					case <-changed:
					case <-ctx.Done():
						return ctx.Err()
					}
				}
				continue
			}
			if e.ports[t.port] != nil {
				s.mu.Unlock()
				return fmt.Errorf("KISS session port %d already connected", t.port)
			}
			t.mu.Lock()
			if t.closed {
				t.mu.Unlock()
				s.mu.Unlock()
				return hardware.ErrModemClosed
			}
			e.ports[t.port], t.epoch = t, e
			t.mu.Unlock()
			s.mu.Unlock()
			return nil
		}
		changed := s.changed
		s.mu.Unlock()
		select {
		case <-changed:
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (t *sessionTransport) markOnline() {
	s := t.session
	s.mu.Lock()
	t.mu.RLock()
	e := t.epoch
	if e != nil && s.current == e && e.ready && e.ports[t.port] == t && !t.closed {
		s.refreshAttempted[t.port] = false
	}
	t.mu.RUnlock()
	s.mu.Unlock()
}

// Negotiate runs only on the owner, after its ordinary HELLO and before any
// other virtual port connects. A timed-out or ambiguous reply retires this
// socket; no later control exchange can mistake a late response for its own.
func (s *Session) Negotiate(ctx context.Context, modem *hardware.KissModem) (ClientCapacity, error) {
	probeCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	reply, err := modem.Request(probeCtx, HWQueuedCapacity, []byte{QueuedProtocolVersion, 1})
	if err == nil && len(reply) == 3 {
		return ClientCapacity{}, fmt.Errorf("%w: legacy three-byte CAPACITY reply", ErrSubportsUnavailable)
	}
	if errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_INVALID_PARAM)) ||
		errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_INVALID_LENGTH)) ||
		errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_UNKNOWN_CMD)) ||
		errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_NO_CALLBACK)) {
		return ClientCapacity{}, fmt.Errorf("%w: %v", ErrSubportsUnavailable, err)
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return ClientCapacity{}, fmt.Errorf("%w: %v", ErrSubportsProbeTimeout, err)
	}
	if err != nil {
		return ClientCapacity{}, fmt.Errorf("extended CAPACITY: %w", err)
	}
	if len(reply) != 5 || reply[0] != QueuedProtocolVersion || reply[1] == 0 || reply[3] == 0 {
		return ClientCapacity{}, fmt.Errorf("invalid extended CAPACITY response: %x", reply)
	}
	s.mu.Lock()
	minPorts := s.minPorts
	minSlots := s.minSlots
	s.mu.Unlock()
	if minPorts < 1 || minPorts > sessionPorts {
		return ClientCapacity{}, fmt.Errorf("invalid KISS session minimum port count %d", minPorts)
	}
	if reply[3] < uint8(minPorts) {
		return ClientCapacity{}, fmt.Errorf("%w: modem reports %d ports including owner, need %d", ErrSubportsInsufficient, reply[3], minPorts)
	}
	if reply[4] == 0 {
		return ClientCapacity{}, fmt.Errorf("%w: extended CAPACITY reports no free sessions", ErrSubportsBusy)
	}
	if int(reply[1]) < minSlots {
		return ClientCapacity{}, fmt.Errorf("extended CAPACITY reports %d TCP slots, need %d including direct clients",
			reply[1], minSlots)
	}
	cap := ClientCapacity{ExternalTCPSlots: reply[1], LocalSourceSlots: reply[2], LogicalPorts: min(reply[3], sessionPorts)}
	s.mu.Lock()
	if s.current == nil {
		s.mu.Unlock()
		return ClientCapacity{}, hardware.ErrDisconnected
	}
	s.current.cap, s.current.ready = cap, true
	s.notify()
	s.mu.Unlock()
	return cap, nil
}

func (s *Session) Capacity() (ClientCapacity, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.current == nil || !s.current.ready {
		return ClientCapacity{}, ErrOffline
	}
	return s.current.cap, nil
}

func (s *Session) fail(e *sessionEpoch, err error) {
	e.once.Do(func() {
		close(e.done)
		_ = e.conn.Close()
		s.mu.Lock()
		if s.current == e {
			s.current = nil
			s.notify()
		}
		for i, port := range e.ports {
			if port == nil {
				continue
			}
			port.mu.Lock()
			if port.epoch == e {
				port.epoch = nil
				close(port.dead)
				if port.onError != nil && err != nil {
					// Error callbacks are not called under session locks.
					go port.onError(err)
				}
			}
			port.mu.Unlock()
			e.ports[i] = nil
		}
		s.mu.Unlock()
	})
}

func (s *Session) read(e *sessionEpoch) {
	buf := make([]byte, 1024)
	var remainder []byte
	for {
		n, err := e.conn.Read(buf)
		if n > 0 {
			frames, rest, failures := hardware.ExtractFrames(append(remainder, buf[:n]...))
			if len(rest) > hardware.KISS_MAX_ENCODED_FRAME_SIZE {
				s.fail(e, errors.New("KISS session partial frame too large"))
				return
			}
			remainder = append([]byte(nil), rest...)
			if len(failures) != 0 {
				s.fail(e, fmt.Errorf("KISS session malformed frame: %w", failures[0]))
				return
			}
			for _, frame := range frames {
				if frame.Port < 0 || frame.Port >= sessionPorts {
					s.fail(e, fmt.Errorf("unexpected KISS session port %d", frame.Port))
					return
				}
				s.mu.Lock()
				port := e.ports[frame.Port]
				s.mu.Unlock()
				if port != nil {
					port.mu.RLock()
					if port.epoch == e && port.frame != nil {
						port.frame(frame)
					}
					port.mu.RUnlock()
				}
			}
		}
		if err != nil {
			s.fail(e, err)
			return
		}
	}
}

func (s *Session) write(e *sessionEpoch) {
	next := 0
	for {
		var request sessionWrite
		requestPort := 0
		found := false
		for i := 0; i < sessionPorts; i++ {
			port := (next + i) % sessionPorts
			select {
			case request = <-e.queues[port]:
				next = (port + 1) % sessionPorts
				requestPort = port
				found = true
			default:
			}
			if found {
				break
			}
		}
		if !found {
			select {
			case <-e.wake:
				continue
			case <-e.done:
				return
			}
		}
		select {
		case <-e.done:
			request.done <- hardware.ErrDisconnected
			return
		default:
		}
		s.mu.Lock()
		port := e.ports[requestPort]
		if port != nil {
			port.mu.RLock()
		}
		s.mu.Unlock()
		if port == nil {
			request.done <- hardware.ErrDisconnected
			continue
		}
		if port.epoch != e {
			port.mu.RUnlock()
			request.done <- hardware.ErrDisconnected
			continue
		}
		err := e.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if err == nil {
			var n int
			n, err = e.conn.Write(request.wire)
			if err == nil && n != len(request.wire) {
				err = io.ErrShortWrite
			}
		}
		port.mu.RUnlock()
		if err != nil {
			request.done <- err
			s.fail(e, err)
			return
		}
		request.done <- nil
	}
}

func (t *sessionTransport) Send(data []byte) error {
	frame, err := hardware.DecodeFrame(data)
	if err != nil {
		return err
	}
	if frame.Port != 0 || len(data) > hardware.KISS_MAX_ENCODED_FRAME_SIZE ||
		!bytes.Equal(data, hardware.EncodeFrame(0, frame.Command, frame.Data)) {
		return errors.New("invalid virtual KISS frame")
	}
	t.mu.RLock()
	e := t.epoch
	if e == nil {
		t.mu.RUnlock()
		return hardware.ErrDisconnected
	}
	wire := hardware.EncodeFrame(t.port, frame.Command, frame.Data)
	request := sessionWrite{wire: wire, done: make(chan error, 1)}
	select {
	case <-e.done:
		t.mu.RUnlock()
		return hardware.ErrDisconnected
	case e.queues[t.port] <- request:
		select {
		case e.wake <- struct{}{}:
		default:
		}
	default:
		t.mu.RUnlock()
		return ErrSessionBackpressure
	}
	t.mu.RUnlock()
	select {
	case err := <-request.done:
		return err
	case <-t.dead:
		return hardware.ErrDisconnected
	case <-e.done:
		return hardware.ErrDisconnected
	}
}

func (t *sessionTransport) Close() error {
	s := t.session
	s.mu.Lock()
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		s.mu.Unlock()
		return nil
	}
	t.closed = true
	e := t.epoch
	if e != nil {
		t.epoch = nil
		if e.ports[t.port] == t {
			e.ports[t.port] = nil
			if t.port != 0 {
				e.quarantined[t.port] = true
			}
		}
	}
	select {
	case <-t.dead:
	default:
		close(t.dead)
	}
	t.mu.Unlock()
	s.mu.Unlock()
	// Retire only the owner's socket. A child port stays quarantined so late
	// replies cannot be adopted by another role lifetime on the same epoch.
	if e != nil && t.port == 0 {
		s.fail(e, hardware.ErrDisconnected)
	}
	return nil
}

func (t *sessionTransport) Dead() <-chan struct{} { return t.dead }
func (t *sessionTransport) SetFrameHandler(h func(*hardware.KissFrame)) {
	t.mu.Lock()
	t.frame = h
	t.mu.Unlock()
}
func (t *sessionTransport) SetErrorHandler(h func(error)) {
	t.mu.Lock()
	t.onError = h
	t.mu.Unlock()
}
