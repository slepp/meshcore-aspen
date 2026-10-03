package radio

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

type sessionMast struct {
	listener            net.Listener
	mu                  sync.Mutex
	active              []net.Conn
	accepted            atomic.Int32
	extendedProbes      atomic.Int32
	extended            bool
	legacyCapacityReply bool
	silentExtended      bool
	malformedExtended   bool
	sessionBusy         bool
	rejectHelloPort     int
	externalSlots       byte
	subports            byte
	owners              [sessionPorts]int
	hello               [sessionPorts]int
	configGets          [sessionPorts]int
	configSets          [sessionPorts]int
	configGeneration    uint32
	profile             []byte
	submits             chan int
}

func newSessionMast(t *testing.T, extended bool) *sessionMast {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	m := &sessionMast{
		listener: listener, extended: extended, submits: make(chan int, 16),
		profile: (PHYSettings{
			Radio:   hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5},
			TxPower: 22, Profile: PHYProfile{AirtimeFactor: 1},
		}).wire(),
		configGeneration: 1, externalSlots: 8, subports: 4, rejectHelloPort: -1,
	}
	var workers sync.WaitGroup
	ended := make(chan struct{})
	go func() {
		defer close(ended)
		for {
			conn, err := listener.Accept()
			if err != nil {
				workers.Wait()
				return
			}
			epoch := uint32(m.accepted.Add(1))
			m.mu.Lock()
			m.active = append(m.active, conn)
			m.mu.Unlock()
			workers.Add(1)
			go func() {
				defer workers.Done()
				defer conn.Close()
				m.serve(conn, epoch)
			}()
		}
	}()
	t.Cleanup(func() {
		_ = listener.Close()
		m.mu.Lock()
		for _, conn := range m.active {
			_ = conn.Close()
		}
		m.mu.Unlock()
		<-ended
	})
	return m
}

func (m *sessionMast) serve(conn net.Conn, epoch uint32) {
	var remainder []byte
	buffer := make([]byte, 1024)
	var signalSet [sessionPorts]bool
	var signalValues [sessionPorts]byte
	for {
		n, err := conn.Read(buffer)
		if err != nil {
			return
		}
		frames, rest, failures := hardware.ExtractFrames(append(remainder, buffer[:n]...))
		if len(failures) != 0 {
			return
		}
		remainder = rest
		for _, frame := range frames {
			if frame.Port >= sessionPorts || frame.Command != hardware.KISS_CMD_SETHARDWARE || len(frame.Data) == 0 {
				return
			}
			port := frame.Port
			cmd, data := frame.Data[0], frame.Data[1:]
			var response []byte
			switch cmd {
			case hardware.HW_CMD_SET_SIGNAL_REPORT:
				if len(data) != 1 {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_LENGTH}
					break
				}
				signalSet[port], signalValues[port] = true, data[0]
				response = []byte{hardware.HwResp(hardware.HW_CMD_GET_SIGNAL_REPORT), data[0]}
			case hardware.HW_CMD_GET_SIGNAL_REPORT:
				response = []byte{hardware.HwResp(cmd), signalValues[port]}
			case HWQueuedHello:
				m.mu.Lock()
				m.hello[port]++
				m.owners[port] += int(data[1])
				reject := port == m.rejectHelloPort
				m.mu.Unlock()
				if !signalSet[port] {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}
					break
				}
				if reject {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}
					break
				}
				response = make([]byte, 13)
				response[0], response[1] = cmd|0x80, 1
				binary.LittleEndian.PutUint32(response[3:], 100+epoch+uint32(port))
				binary.LittleEndian.PutUint32(response[7:], 1)
				response[11], response[12] = 12, data[1]
			case HWQueuedCapacity:
				m.mu.Lock()
				extended, legacy, silent, malformed, busy := m.extended, m.legacyCapacityReply, m.silentExtended, m.malformedExtended, m.sessionBusy
				slots, subports := m.externalSlots, m.subports
				m.mu.Unlock()
				if len(data) == 2 {
					m.extendedProbes.Add(1)
				}
				if len(data) == 2 && silent {
					continue
				} else if len(data) == 2 && malformed {
					response = []byte{cmd | 0x80, 1, slots, 3, 4}
				} else if len(data) == 2 && data[1] == 1 && extended {
					sessions := byte(1)
					if busy {
						sessions = 0
					}
					response = []byte{cmd | 0x80, 1, slots, 3, subports, sessions}
				} else if len(data) == 2 && legacy {
					response = []byte{cmd | 0x80, 1, 8, 3}
				} else if len(data) == 2 {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}
				} else {
					response = []byte{cmd | 0x80, 1, 8, 3}
				}
			case HWQueuedConfig:
				response = make([]byte, 25)
				response[0], response[1] = cmd|0x80, 1
				m.mu.Lock()
				if len(data) == 24 && data[1] == 1 {
					m.configSets[port]++
					m.profile = bytes.Clone(data[6:])
				} else {
					m.configGets[port]++
				}
				binary.LittleEndian.PutUint32(response[3:], m.configGeneration)
				copy(response[7:], m.profile)
				m.mu.Unlock()
			case HWQueuedSourcePolicy:
				response = append([]byte{cmd | 0x80, 1, 0}, data[5:]...)
			case HWQueuedSubmit:
				m.submits <- port
				response = make([]byte, 24)
				response[0], response[1] = HWQueuedTXEvent, 1
				copy(response[2:10], data[1:9])
				response[10] = byte(TXAccepted)
			case hardware.HW_CMD_GET_BATTERY:
				response = []byte{cmd | 0x80, 0xe4, 0x0c}
			case hardware.HW_CMD_GET_NOISE_FLOOR:
				response = []byte{cmd | 0x80, 0x88, 0xff}
			case hardware.HW_CMD_GET_CURRENT_RSSI:
				response = []byte{cmd | 0x80, 0xb0 + byte(port)}
			case hardware.HW_CMD_GET_STATS:
				response = append([]byte{cmd | 0x80}, make([]byte, 12)...)
			default:
				response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_NO_CALLBACK}
			}
			if _, err := conn.Write(hardware.EncodeHardwareFrame(port, response[0], response[1:])); err != nil {
				return
			}
		}
	}
}

func sessionConfig(m *sessionMast, s *Session, port int) Config {
	return Config{
		Address: m.listener.Addr().String(), Radio: hardware.RadioConfig{
			FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5,
		},
		TxPower: 22, Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
		RequireParity: true, ConfigurationOwner: port == 0,
		Session: s, SessionPort: port,
	}
}

func openSessionRole(t *testing.T, m *sessionMast, s *Session, port int) *Link {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	link, err := Open(ctx, sessionConfig(m, s, port))
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = link.Close(); cancel() })
	return link
}

func TestSessionFourRolesOwnOneSocketAndSeparateJobResults(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	links := [sessionPorts]*Link{}
	for i := range links {
		links[i] = openSessionRole(t, m, s, i)
	}
	if got := m.accepted.Load(); got != 1 {
		t.Fatalf("four logical sources used %d sockets", got)
	}
	m.mu.Lock()
	hello, owners, configSets, configGets := m.hello, m.owners, m.configSets, m.configGets
	m.mu.Unlock()
	for i := range links {
		if hello[i] != 1 || owners[i] != btoi(i == 0) {
			t.Fatalf("port %d HELLO count=%d owner=%d", i, hello[i], owners[i])
		}
		if configSets[i] != btoi(i == 0) || configGets[i] != btoi(i != 0) {
			t.Fatalf("port %d unexpectedly retuned or missed readback: SET=%d GET=%d",
				i, configSets[i], configGets[i])
		}
	}
	if cap, err := s.Capacity(); err != nil || cap.ExternalTCPSlots != 8 || cap.LocalSourceSlots != 3 {
		t.Fatalf("extended capacity: %+v, %v", cap, err)
	}
	type reply struct {
		port int
		data []byte
		err  error
	}
	replies := make(chan reply, sessionPorts)
	for i, link := range links {
		go func() {
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			data, err := link.modem.Request(ctx, hardware.HW_CMD_GET_CURRENT_RSSI, nil)
			replies <- reply{i, data, err}
		}()
	}
	for range links {
		got := <-replies
		if got.err != nil || !bytes.Equal(got.data, []byte{0xb0 + byte(got.port)}) {
			t.Fatalf("concurrent port %d response crossed sources: %x %v", got.port, got.data, got.err)
		}
	}
	results := [sessionPorts]chan TXResult{}
	for i, link := range links {
		results[i] = make(chan TXResult, 2)
		link.AddTXResultHandler(func(result TXResult) { results[i] <- result })
	}
	for i, link := range links {
		if _, err := link.submit([]byte{byte(0x40 + i)}, 1, 0, 0); err != nil {
			t.Fatal(err)
		}
	}
	for i := range links {
		select {
		case result := <-results[i]:
			if result.State != TXAccepted || result.JobID != 1 || result.Generation != uint32(101+i) ||
				!bytes.Equal(result.PacketBytes(), []byte{byte(0x40 + i)}) {
				t.Fatalf("port %d got another source's TX event: %+v", i, result)
			}
		case <-time.After(time.Second):
			t.Fatalf("port %d missing TX event", i)
		}
	}
}

func TestSessionRoleFollowsModemPHYWithoutChangingOwnerLease(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	openSessionRole(t, m, s, 0)
	cfg := sessionConfig(m, s, 1)
	cfg.PHYTracking = PHYFollow
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	role, err := Open(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer role.Close()
	m.mu.Lock()
	m.profile[8] = 9
	m.configGeneration = 2
	m.mu.Unlock()
	role.requestPHYRefresh()
	waitUntil(t, "per-port PHY-follow readback", func() bool {
		state, ok := role.EffectivePHY()
		return ok && state.ConfigurationGeneration == 2 && state.Settings.Radio.SF == 9
	})
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.configSets[1] != 0 || m.configGets[1] < 2 || m.owners[0] != 1 || m.owners[1] != 0 {
		t.Fatalf("follower changed config lease: SET=%d GET=%d HELLO=%v", m.configSets[1], m.configGets[1], m.owners)
	}
}

func btoi(ok bool) int {
	if ok {
		return 1
	}
	return 0
}

func TestSessionRxMetaAndResponsesStayOnTheirPort(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	links := [sessionPorts]*Link{}
	type rx struct {
		packet []byte
		snr    float32
		rssi   int8
		signal bool
	}
	received := make(chan struct {
		port int
		rx
	}, 4)
	for port := range links {
		links[port] = openSessionRole(t, m, s, port)
		links[port].SetDataHandler(func(data []byte, snr float32, rssi int8, signal bool) {
			received <- struct {
				port int
				rx
			}{port, rx{bytes.Clone(data), snr, rssi, signal}}
		})
	}
	m.mu.Lock()
	conn := m.active[0]
	m.mu.Unlock()
	wire := append(hardware.EncodeFrame(1, 0, []byte{0x11}), hardware.EncodeFrame(2, 0, []byte{0x22})...)
	wire = append(wire, hardware.EncodeHardwareFrame(2, hardware.HW_RESP_RX_META, []byte{8, 0xa1})...)
	wire = append(wire, hardware.EncodeHardwareFrame(1, hardware.HW_RESP_RX_META, []byte{12, 0x9c})...)
	if _, err := conn.Write(wire); err != nil {
		t.Fatal(err)
	}
	for range 2 {
		select {
		case got := <-received:
			if got.port == 1 && (!bytes.Equal(got.packet, []byte{0x11}) || !got.signal || got.snr != 3 || got.rssi != -100) ||
				got.port == 2 && (!bytes.Equal(got.packet, []byte{0x22}) || !got.signal || got.snr != 2 || got.rssi != -95) ||
				got.port != 1 && got.port != 2 {
				t.Fatalf("RX metadata crossed ports: %+v", got)
			}
		case <-time.After(time.Second):
			t.Fatal("missing per-port RX/meta")
		}
	}
}

func TestSessionDisconnectFencesPendingWithoutReplay(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	owner := openSessionRole(t, m, s, 0)
	role := openSessionRole(t, m, s, 1)
	result := make(chan TXResult, 3)
	role.AddTXResultHandler(func(r TXResult) { result <- r })
	if _, err := role.submit([]byte{0x33}, 1, 0, 0); err != nil {
		t.Fatal(err)
	}
	select {
	case <-m.submits:
	case <-time.After(time.Second):
		t.Fatal("first job not sent")
	}
	select {
	case got := <-result:
		if got.State != TXAccepted {
			t.Fatalf("first job: %+v", got)
		}
	case <-time.After(time.Second):
		t.Fatal("accept event missing")
	}
	m.mu.Lock()
	conn := m.active[0]
	m.mu.Unlock()
	_ = conn.Close()
	select {
	case got := <-result:
		if got.State != TXUnknown || !bytes.Equal(got.PacketBytes(), []byte{0x33}) {
			t.Fatalf("disconnect did not fence uncertain job: %+v", got)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("pending job not fenced")
	}
	waitUntil(t, "session reconnect", func() bool { return owner.Online() && role.Online() && m.accepted.Load() >= 2 })
	select {
	case port := <-m.submits:
		t.Fatalf("uncertain job replayed on port %d", port)
	case <-time.After(100 * time.Millisecond):
	}
	if _, err := role.submit([]byte{0x44}, 1, 0, 0); err != nil {
		t.Fatalf("fresh job after reconnect: %v", err)
	}
	select {
	case port := <-m.submits:
		if port != 1 {
			t.Fatalf("new job sent on port %d", port)
		}
	case <-time.After(time.Second):
		t.Fatal("new job was not sent")
	}
	select {
	case got := <-result:
		if got.State != TXAccepted || got.Generation != 103 || !bytes.Equal(got.PacketBytes(), []byte{0x44}) {
			t.Fatalf("new generation did not own the new job: %+v", got)
		}
	case <-time.After(time.Second):
		t.Fatal("new generation event missing")
	}
}

func TestSessionChildRestartQuarantinesOnlyItsPort(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	owner := openSessionRole(t, m, s, 0)
	child := openSessionRole(t, m, s, 1)
	sibling := openSessionRole(t, m, s, 2)
	results := make(chan TXResult, 2)
	child.AddTXResultHandler(func(r TXResult) { results <- r })
	if _, err := child.submit([]byte{0x51}, 1, 0, 0); err != nil {
		t.Fatal(err)
	}
	select {
	case got := <-results:
		if got.State != TXAccepted {
			t.Fatalf("child admission: %+v", got)
		}
	case <-time.After(time.Second):
		t.Fatal("child admission missing")
	}
	if err := child.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case got := <-results:
		if got.State != TXUnknown || !bytes.Equal(got.PacketBytes(), []byte{0x51}) {
			t.Fatalf("child uncertain job not fenced: %+v", got)
		}
	case <-time.After(time.Second):
		t.Fatal("child job did not become unknown")
	}
	m.mu.Lock()
	conn := m.active[0]
	m.mu.Unlock()
	late := hardware.EncodeHardwareFrame(1, hardware.HW_CMD_GET_CURRENT_RSSI|0x80, []byte{0x7f})
	if _, err := conn.Write(late); err != nil {
		t.Fatalf("closing child dropped shared socket: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if m.accepted.Load() != 1 || !owner.Online() || !sibling.Online() {
		t.Fatalf("child closure flapped siblings or shared socket: sockets=%d owner=%v sibling=%v",
			m.accepted.Load(), owner.Online(), sibling.Online())
	}
	reply, err := sibling.modem.Request(ctx, hardware.HW_CMD_GET_CURRENT_RSSI, nil)
	if err != nil || !bytes.Equal(reply, []byte{0xb2}) {
		t.Fatalf("surviving sibling lost response ownership: %x %v", reply, err)
	}
	if _, err := owner.modem.Request(ctx, hardware.HW_CMD_GET_CURRENT_RSSI, nil); err != nil {
		t.Fatalf("owner unable to serve: %v", err)
	}
	siblingResults := make(chan TXResult, 2)
	sibling.AddTXResultHandler(func(r TXResult) { siblingResults <- r })
	if _, err := sibling.submit([]byte{0x52}, 1, 0, 0); err != nil {
		t.Fatalf("sibling cannot transmit before child recovery: %v", err)
	}
	select {
	case accepted := <-siblingResults:
		if accepted.State != TXAccepted {
			t.Fatalf("sibling job not admitted: %+v", accepted)
		}
	case <-time.After(time.Second):
		t.Fatal("sibling admission missing")
	}
	joinCtx, cancelJoin := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancelJoin()
	rejoined, err := Open(joinCtx, sessionConfig(m, s, 1))
	if err != nil {
		t.Fatalf("child restart did not request a bounded fresh epoch: %v", err)
	}
	defer rejoined.Close()
	waitUntil(t, "owner and sibling join after one child-requested refresh", func() bool {
		return owner.Online() && sibling.Online()
	})
	if m.accepted.Load() != 2 {
		t.Fatalf("one child request triggered %d physical sockets, want 2", m.accepted.Load())
	}
	m.mu.Lock()
	hellos := m.hello
	m.mu.Unlock()
	if hellos[1] != 2 {
		t.Fatalf("restarted role did not issue a new port HELLO: %v", hellos)
	}
	select {
	case unknown := <-siblingResults:
		if unknown.State != TXUnknown || !bytes.Equal(unknown.PacketBytes(), []byte{0x52}) {
			t.Fatalf("sibling uncertain job was not fenced: %+v", unknown)
		}
	case <-time.After(time.Second):
		t.Fatal("sibling job stayed pending after owner refresh")
	}
	for _, want := range []int{1, 2} {
		select {
		case port := <-m.submits:
			if port != want {
				t.Fatalf("submitted port=%d, want %d", port, want)
			}
		case <-time.After(time.Second):
			t.Fatalf("original port %d submit missing", want)
		}
	}
	select {
	case port := <-m.submits:
		t.Fatalf("uncertain child job replayed on port %d", port)
	case <-time.After(100 * time.Millisecond):
	}
	readCtx, cancelRead := context.WithTimeout(context.Background(), time.Second)
	defer cancelRead()
	got, err := rejoined.modem.Request(readCtx, hardware.HW_CMD_GET_CURRENT_RSSI, nil)
	if err != nil || !bytes.Equal(got, []byte{0xb1}) {
		t.Fatalf("late response from retired epoch reached new role: %x %v", got, err)
	}
	got, err = sibling.modem.Request(readCtx, hardware.HW_CMD_GET_CURRENT_RSSI, nil)
	if err != nil || !bytes.Equal(got, []byte{0xb2}) {
		t.Fatalf("sibling did not serve after child recovery: %x %v", got, err)
	}
}

func TestSessionChildFailureKeepsOwnerAndSiblingOnline(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	owner := openSessionRole(t, m, s, 0)
	child := openSessionRole(t, m, s, 1)
	sibling := openSessionRole(t, m, s, 2)
	child.requestReset(child.modem)
	waitUntil(t, "failed child disconnected", func() bool { return !child.Online() })
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	for _, link := range []*Link{owner, sibling} {
		reply, err := link.modem.Request(ctx, hardware.HW_CMD_GET_CURRENT_RSSI, nil)
		if err != nil || len(reply) != 1 {
			t.Fatalf("child failure interrupted healthy source: %x %v", reply, err)
		}
	}
	if m.accepted.Load() != 1 || child.Online() {
		t.Fatalf("child failure flapped session or rejoined quarantined port: sockets=%d child=%v",
			m.accepted.Load(), child.Online())
	}
	waitUntil(t, "failed child requested and joined a fresh epoch", func() bool {
		return m.accepted.Load() >= 2 && owner.Online() && child.Online() && sibling.Online()
	})
	if m.accepted.Load() != 2 {
		t.Fatalf("one failed child caused repeated owner refresh: %d", m.accepted.Load())
	}
}

func TestSessionFailedChildHelloCannotRefreshOwnerAgainAfterSimulatedLongBackoff(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	owner := openSessionRole(t, m, s, 0)
	child := openSessionRole(t, m, s, 1)
	sibling := openSessionRole(t, m, s, 2)
	m.mu.Lock()
	m.rejectHelloPort = 1
	m.mu.Unlock()
	child.requestReset(child.modem)
	waitUntil(t, "first child failure requests one owner epoch", func() bool {
		m.mu.Lock()
		hellos := m.hello[1]
		m.mu.Unlock()
		return m.accepted.Load() == 2 && hellos >= 2 && owner.Online() && sibling.Online()
	})
	s.mu.Lock()
	used := s.refreshAttempted[1]
	s.mu.Unlock()
	if !used {
		t.Fatal("failed HELLO incorrectly rearmed child refresh")
	}
	// Link retries at 2, 4, 8, 16 and then 30 seconds. Probe their equivalent
	// attempts without sleeping through more than 90 seconds of wall time.
	var simulated time.Duration
	for _, delay := range []time.Duration{2, 4, 8, 16, 30, 30, 30} {
		simulated += delay * time.Second
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		_, err := Open(ctx, sessionConfig(m, s, 1))
		cancel()
		if !errors.Is(err, ErrSessionRefreshLimited) {
			t.Fatalf("failed HELLO allowed repeat owner refresh at simulated %s: %v", simulated, err)
		}
		if m.accepted.Load() != 2 || !owner.Online() || !sibling.Online() {
			t.Fatalf("siblings flapped at simulated %s: sockets=%d owner=%v sibling=%v",
				simulated, m.accepted.Load(), owner.Online(), sibling.Online())
		}
	}
	if simulated <= 90*time.Second {
		t.Fatalf("retry simulation too short: %s", simulated)
	}
	m.mu.Lock()
	m.rejectHelloPort = -1
	conn := m.active[len(m.active)-1]
	m.mu.Unlock()
	_ = conn.Close()
	waitUntil(t, "child succeeds after natural owner epoch", func() bool {
		return m.accepted.Load() == 3 && owner.Online() && child.Online() && sibling.Online()
	})
	s.mu.Lock()
	used = s.refreshAttempted[1]
	s.mu.Unlock()
	if used {
		t.Fatal("successful child HELLO/config/PHY settle did not rearm one refresh")
	}
	child.requestReset(child.modem)
	waitUntil(t, "new successful child lifetime may refresh once", func() bool {
		return m.accepted.Load() == 4 && owner.Online() && child.Online() && sibling.Online()
	})
	if m.accepted.Load() != 4 {
		t.Fatalf("child recovery triggered multiple owner refreshes: %d", m.accepted.Load())
	}
}

func TestSessionOldFirmwareNeedsNewDirectConnection(t *testing.T) {
	for _, oldThreeBytes := range []bool{false, true} {
		t.Run(map[bool]string{false: "INVALID_PARAM", true: "legacy_three_bytes"}[oldThreeBytes], func(t *testing.T) {
			m := newSessionMast(t, false)
			m.legacyCapacityReply = oldThreeBytes
			s := NewSession(m.listener.Addr().String(), 1)
			_, err := Open(context.Background(), sessionConfig(m, s, 0))
			if !errors.Is(err, ErrSubportsUnavailable) {
				t.Fatalf("old CAPACITY not detected: %v", err)
			}
			cfg := sessionConfig(m, nil, 0)
			link, err := Open(context.Background(), cfg)
			if err != nil {
				t.Fatalf("fresh independent connection failed: %v", err)
			}
			defer link.Close()
			if m.accepted.Load() != 2 {
				t.Fatalf("uncorrelated discovery socket was reused: %d", m.accepted.Load())
			}
		})
	}
}

func TestSessionFirmwareDowngradeFencesRolesAndSignalsFatalOwner(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSession(m.listener.Addr().String(), 1)
	owner := openSessionRole(t, m, s, 0)
	child := openSessionRole(t, m, s, 1)
	m.mu.Lock()
	m.extended = false
	conn := m.active[0]
	m.mu.Unlock()
	_ = conn.Close()
	select {
	case err := <-owner.Fatal():
		if !errors.Is(err, ErrSubportsUnavailable) {
			t.Fatalf("downgrade did not signal missing subports: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("downgraded owner silently retried forever")
	}
	waitUntil(t, "child fenced after downgrade", func() bool { return !owner.Online() && !child.Online() })
	if _, err := s.Capacity(); !errors.Is(err, ErrOffline) {
		t.Fatalf("downgraded session was reported ready: %v", err)
	}
}

func TestSessionTransientCapabilityLossRetriesWithoutFatalDowngrade(t *testing.T) {
	for _, kind := range []string{"timeout", "malformed", "three_ports", "busy"} {
		t.Run(kind, func(t *testing.T) {
			m := newSessionMast(t, true)
			s := NewSession(m.listener.Addr().String(), 1)
			owner := openSessionRole(t, m, s, 0)
			child := openSessionRole(t, m, s, 1)
			m.mu.Lock()
			switch kind {
			case "timeout":
				m.silentExtended = true
			case "malformed":
				m.malformedExtended = true
			case "three_ports":
				m.subports = 3
			case "busy":
				m.sessionBusy = true
			}
			conn := m.active[0]
			m.mu.Unlock()
			_ = conn.Close()
			waitUntil(t, "transient capability probe reached modem", func() bool {
				return m.accepted.Load() >= 2 && m.extendedProbes.Load() >= 2
			})
			select {
			case err := <-owner.Fatal():
				t.Fatalf("transient CAPACITY %s caused a fatal downgrade: %v", kind, err)
			default:
			}
			m.mu.Lock()
			m.silentExtended = false
			m.malformedExtended = false
			m.subports = 4
			m.sessionBusy = false
			m.mu.Unlock()
			deadline := time.After(18 * time.Second)
			for m.accepted.Load() < 3 || !owner.Online() || !child.Online() {
				select {
				case <-deadline:
					m.mu.Lock()
					hello, probes := m.hello, m.extendedProbes.Load()
					m.mu.Unlock()
					t.Fatalf("transient %s did not recover: accepts=%d probes=%d hello=%v owner=%v child=%v",
						kind, m.accepted.Load(), probes, hello, owner.Online(), child.Online())
				case <-time.After(10 * time.Millisecond):
				}
			}
			select {
			case err := <-owner.Fatal():
				t.Fatalf("recovered CAPACITY %s still caused fatal error: %v", kind, err)
			default:
			}
		})
	}
}

func TestSessionBusyCapacityDoesNotFallBackOrAdmitChildren(t *testing.T) {
	m := newSessionMast(t, true)
	m.mu.Lock()
	m.sessionBusy = true
	m.mu.Unlock()
	s := NewSession(m.listener.Addr().String(), 1)
	_, err := Open(context.Background(), sessionConfig(m, s, 0))
	if !errors.Is(err, ErrSubportsBusy) ||
		errors.Is(err, ErrSubportsUnavailable) ||
		errors.Is(err, ErrSubportsInsufficient) {
		t.Fatalf("valid busy CAPACITY response misclassified: %v", err)
	}
	if _, err := s.Capacity(); !errors.Is(err, ErrOffline) {
		t.Fatalf("busy session admitted children: %v", err)
	}
	m.mu.Lock()
	m.sessionBusy = false
	m.mu.Unlock()
	owner := openSessionRole(t, m, s, 0)
	child := openSessionRole(t, m, s, 1)
	if !owner.Online() || !child.Online() || m.accepted.Load() != 2 {
		t.Fatalf("fresh aggregate connection did not recover: sockets=%d", m.accepted.Load())
	}
}

func TestSessionSilentOldCapabilityRetiresUncertainSocket(t *testing.T) {
	m := newSessionMast(t, false)
	m.silentExtended = true
	s := NewSession(m.listener.Addr().String(), 1)
	_, err := Open(context.Background(), sessionConfig(m, s, 0))
	if !errors.Is(err, ErrSubportsProbeTimeout) || errors.Is(err, ErrSubportsUnavailable) {
		t.Fatalf("silent extended CAPACITY was misclassified: %v", err)
	}
	if _, err := s.Capacity(); !errors.Is(err, ErrOffline) {
		t.Fatalf("timed-out socket stayed available: %v", err)
	}
}

func TestSessionCapacityRequiresFourPortsIncludingOwner(t *testing.T) {
	m := newSessionMast(t, true)
	m.mu.Lock()
	m.subports = 3
	m.mu.Unlock()
	s := NewSession(m.listener.Addr().String(), 1)
	if _, err := Open(context.Background(), sessionConfig(m, s, 0)); !errors.Is(err, ErrSubportsInsufficient) || errors.Is(err, ErrSubportsUnavailable) {
		t.Fatalf("three ports including owner were treated as explicit old firmware: %v", err)
	}
	if _, err := s.Capacity(); !errors.Is(err, ErrOffline) {
		t.Fatalf("insufficient subports were marked available: %v", err)
	}
	direct, err := Open(context.Background(), sessionConfig(m, nil, 0))
	if err != nil {
		t.Fatalf("legacy direct fallback did not use a fresh socket: %v", err)
	}
	defer direct.Close()
	if m.accepted.Load() != 2 {
		t.Fatalf("negotiation socket was reused: %d", m.accepted.Load())
	}
}

func TestSessionReservedRolePortsSurviveCapacityChange(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSessionWithPorts(m.listener.Addr().String(), 1, 2)
	owner := openSessionRole(t, m, s, 0)
	child := openSessionRole(t, m, s, 3)
	if err := s.ReserveCapacity(5, 1); err == nil {
		t.Fatal("reserved nonexistent virtual port")
	}
	if err := s.ReserveCapacity(4, 2); err != nil {
		t.Fatal(err)
	}
	m.mu.Lock()
	m.subports = 3
	conn := m.active[0]
	m.mu.Unlock()
	_ = conn.Close()
	waitUntil(t, "capacity shrink rejected on reconnect", func() bool {
		return m.accepted.Load() >= 2 && m.extendedProbes.Load() >= 2 && !owner.Online() && !child.Online()
	})
	select {
	case err := <-owner.Fatal():
		t.Fatalf("temporary port loss caused terminal downgrade: %v", err)
	default:
	}
	m.mu.Lock()
	m.subports = 4
	m.mu.Unlock()
	waitUntil(t, "reserved role ports recovered", func() bool {
		return m.accepted.Load() >= 3 && owner.Online() && child.Online()
	})
}

func TestSessionReservedDirectClientSlotsSurviveCapacityChange(t *testing.T) {
	m := newSessionMast(t, true)
	s := NewSessionWithPorts(m.listener.Addr().String(), 1, 2)
	owner := openSessionRole(t, m, s, 0)
	if err := s.ReserveCapacity(2, 9); err == nil {
		t.Fatal("reserved more physical sockets than the modem reported")
	}
	if err := s.ReserveCapacity(2, 4); err != nil {
		t.Fatal(err)
	}
	m.mu.Lock()
	m.externalSlots = 3
	conn := m.active[0]
	m.mu.Unlock()
	_ = conn.Close()
	waitUntil(t, "capacity shrink rejected on reconnect", func() bool {
		return m.accepted.Load() >= 2 && m.extendedProbes.Load() >= 2 && !owner.Online()
	})
	m.mu.Lock()
	m.externalSlots = 4
	m.mu.Unlock()
	waitUntil(t, "reserved direct sockets recovered", func() bool {
		return m.accepted.Load() >= 3 && owner.Online()
	})
}

func TestSessionRequiresSlotsOnStartAndReconnect(t *testing.T) {
	m := newSessionMast(t, true)
	m.mu.Lock()
	m.externalSlots = 3
	m.mu.Unlock()
	s := NewSession(m.listener.Addr().String(), 4)
	_, err := Open(context.Background(), sessionConfig(m, s, 0))
	if err == nil || errors.Is(err, ErrSubportsUnavailable) {
		t.Fatalf("negotiated but insufficient TCP capacity was treated as old firmware: %v", err)
	}
	m.mu.Lock()
	m.externalSlots = 4
	m.mu.Unlock()
	owner := openSessionRole(t, m, s, 0)
	role := openSessionRole(t, m, s, 1)
	m.mu.Lock()
	m.externalSlots = 3
	conn := m.active[len(m.active)-1]
	m.mu.Unlock()
	_ = conn.Close()
	waitUntil(t, "lost connection", func() bool { return !owner.Online() && !role.Online() })
	waitUntil(t, "renegotiation attempted", func() bool { return m.accepted.Load() >= 3 })
	if owner.Online() || role.Online() {
		t.Fatal("reconnect admitted sources after CAPACITY dropped below reserved clients")
	}
}

func TestSessionSlowPortBoundedWhileOtherPortSends(t *testing.T) {
	s := NewSession("unused", 1)
	a, b := net.Pipe()
	defer b.Close()
	e := &sessionEpoch{conn: a, wake: make(chan struct{}, 1), done: make(chan struct{})}
	for i := range e.queues {
		e.queues[i] = make(chan sessionWrite, sessionQueueDepth)
	}
	v0, err := s.Transport(0)
	if err != nil {
		t.Fatal(err)
	}
	v1, err := s.Transport(1)
	if err != nil {
		t.Fatal(err)
	}
	t0, t1 := v0.(*sessionTransport), v1.(*sessionTransport)
	s.mu.Lock()
	s.current = e
	e.ports[0], e.ports[1] = t0, t1
	t0.epoch, t1.epoch = e, e
	s.mu.Unlock()
	defer s.fail(e, nil)
	go s.write(e)
	first := sessionWrite{wire: hardware.EncodeFrame(0, 6, []byte{1}), done: make(chan error, 1)}
	e.queues[0] <- first
	e.wake <- struct{}{}
	waitUntil(t, "writer entered blocked write", func() bool { return len(e.queues[0]) == 0 })
	for i := 0; i < sessionQueueDepth; i++ {
		e.queues[0] <- sessionWrite{wire: hardware.EncodeFrame(0, 6, []byte{byte(i)}), done: make(chan error, 1)}
	}
	if err := t0.Send(hardware.EncodeFrame(0, 6, []byte{2})); !errors.Is(err, ErrSessionBackpressure) {
		t.Fatalf("slow port did not apply bounded backpressure: %v", err)
	}
	other := make(chan error, 1)
	go func() { other <- t1.Send(hardware.EncodeFrame(0, 6, []byte{3})) }()
	waitUntil(t, "second port entered its own queue", func() bool { return len(e.queues[1]) == 1 })
	readPort := func() int {
		t.Helper()
		_ = b.SetReadDeadline(time.Now().Add(time.Second))
		buf := make([]byte, 64)
		n, err := b.Read(buf)
		if err != nil {
			t.Fatal(err)
		}
		frame, err := hardware.DecodeFrame(buf[:n])
		if err != nil {
			t.Fatal(err)
		}
		return frame.Port
	}
	if first, second := readPort(), readPort(); first != 0 || second != 1 {
		t.Fatalf("busy port starved another port: first=%d second=%d", first, second)
	}
	s.fail(e, nil)
	select {
	case <-first.done:
	case <-time.After(time.Second):
		t.Fatal("blocked send survived disconnect")
	}
	select {
	case <-other:
	case <-time.After(time.Second):
		t.Fatal("other port survived disconnect")
	}
}
