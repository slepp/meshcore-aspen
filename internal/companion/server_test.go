package companion

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
	"github.com/meshcore-go/meshcore-go/hardware"
)

type testRadio struct {
	mu         sync.Mutex
	handler    func(*meshcore.Packet)
	rawHandler func([]byte, float32, int8, bool)
	out        chan []byte
	jobs       []testTransmission
}

type testTransmission struct {
	Data     []byte
	Priority uint8
	Delay    time.Duration
}

func newTestRadio() *testRadio { return &testRadio{out: make(chan []byte, 1024)} }

func TestOperationErrorsReachCallbackOrDefaultLogger(t *testing.T) {
	failure := errors.New("persistent write failed")
	s := &Server{}
	var got error
	s.cfg.ErrorHandler = func(err error) { got = err }
	s.report(failure)
	if got != failure {
		t.Fatalf("error handler received %v, want %v", got, failure)
	}
	s.cfg.ErrorHandler = nil
	var output bytes.Buffer
	old := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&output, nil)))
	t.Cleanup(func() { slog.SetDefault(old) })
	s.report(failure)
	if !bytes.Contains(output.Bytes(), []byte(failure.Error())) {
		t.Fatalf("companion error was not logged: %q", output.String())
	}
}

func (r *testRadio) SendData(b []byte) error {
	if !r.Enqueue(b, 0, 0) {
		return errors.New("full")
	}
	return nil
}
func (r *testRadio) SetDataHandler(h func(*meshcore.Packet)) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.handler = h
}
func (r *testRadio) SetRawDataHandler(h func([]byte, float32, int8, bool)) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.rawHandler = h
}
func (r *testRadio) AddOutboundHandler(func([]byte)) {}
func (r *testRadio) Close() error                    { return nil }
func (r *testRadio) Enqueue(b []byte, priority uint8, delay time.Duration) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	select {
	case r.out <- append([]byte(nil), b...):
		r.jobs = append(r.jobs, testTransmission{Data: append([]byte(nil), b...), Priority: priority, Delay: delay})
		return true
	default:
		return false
	}
}
func (r *testRadio) TxQueueLen() int { return len(r.out) }
func (r *testRadio) inject(p *meshcore.Packet) {
	r.mu.Lock()
	h, raw := r.handler, r.rawHandler
	r.mu.Unlock()
	if data, err := p.ToBytes(); err == nil && raw != nil {
		raw(data, p.SNR, p.RSSI, p.HasSignalInfo)
	}
	h(p.Clone())
}
func (r *testRadio) packet(t *testing.T) *meshcore.Packet {
	t.Helper()
	select {
	case raw := <-r.out:
		p, err := meshcore.PacketFromBytes(raw)
		if err != nil {
			t.Fatal(err)
		}
		return p
	case <-time.After(3 * time.Second):
		t.Fatal("no radio packet")
		return nil
	}
}

// The adapter deliberately uses the upstream framing and response parsers.
// The optional upstream TCP transport is a separate module, not a host dependency.
type testTransport struct {
	address  string
	conn     net.Conn
	response func(protocol.Response)
	onError  func(error)
	done     chan struct{}
}

func (tr *testTransport) SetResponseHandler(h func(protocol.Response)) { tr.response = h }
func (tr *testTransport) SetErrorHandler(h func(error))                { tr.onError = h }
func (tr *testTransport) Connect(ctx context.Context) error {
	c, err := (&net.Dialer{}).DialContext(ctx, "tcp", tr.address)
	if err != nil {
		return err
	}
	tr.conn = c
	tr.done = make(chan struct{})
	go func() {
		defer close(tr.done)
		for {
			frame, err := readFrame(c)
			if err != nil {
				return
			}
			resp, err := protocol.ParseResponse(frame)
			if err != nil {
				tr.onError(err)
				return
			}
			tr.response(resp)
		}
	}()
	return nil
}
func (tr *testTransport) Close() error { err := tr.conn.Close(); <-tr.done; return err }
func (tr *testTransport) Send(b []byte) error {
	wire, err := protocol.FrameEncode(protocol.FrameTypeOutgoing, b)
	if err != nil {
		return err
	}
	_, err = tr.conn.Write(wire)
	return err
}
func readFrame(c net.Conn) ([]byte, error) {
	h := make([]byte, 3)
	if _, err := io.ReadFull(c, h); err != nil {
		return nil, err
	}
	if h[0] != '>' {
		return nil, fmt.Errorf("wrong frame marker %x", h[0])
	}
	n := int(binary.LittleEndian.Uint16(h[1:]))
	if n < 1 || n > protocol.MaxFrameSize {
		return nil, fmt.Errorf("invalid length %d", n)
	}
	b := make([]byte, n)
	_, err := io.ReadFull(c, b)
	return b, err
}

func testIdentity(n byte) meshcore.LocalIdentity {
	var seed [32]byte
	seed[0] = n
	return meshcore.NewLocalIdentityFromSeed(seed)
}
func stateDir(t *testing.T) string {
	t.Helper()
	var suffix [8]byte
	if _, err := rand.Read(suffix[:]); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join("..", "..", ".tmp", "companion-test-"+hex.EncodeToString(suffix[:]))
	if err := os.MkdirAll(path, 0700); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(path) })
	return path
}
func testConfig(dir string) Config {
	return Config{StateDir: dir, Name: "Host test", RadioConfig: hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5}, TxPower: 20, AdvertInterval: time.Hour, Retention: DurableReplay}
}
func startTestServer(t *testing.T, id meshcore.LocalIdentity, cfg Config) (*Server, *testRadio, string) {
	t.Helper()
	r := newTestRadio()
	s, err := New(id, r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	initial := r.packet(t)
	advert, err := meshcore.AdvertFromBytes(initial.Payload)
	if err != nil || initial.PayloadType() != meshcore.PayloadTypeAdvert || !advert.Verify() || !advert.PublicKey.Matches(id.Identity) {
		t.Fatalf("missing signed initial advertisement: %+v %v", initial, err)
	}
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	serveDone := make(chan error, 1)
	go func() { serveDone <- s.Serve(context.Background(), l) }()
	t.Cleanup(func() {
		s.Close()
		if err := <-serveDone; err != nil {
			t.Error(err)
		}
	})
	deadline := time.Now().Add(time.Second)
	for {
		s.mu.Lock()
		ready := s.listener != nil
		s.mu.Unlock()
		if ready {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("companion listener did not become ready")
		}
		time.Sleep(time.Millisecond)
	}
	return s, r, l.Addr().String()
}
func testClient(t *testing.T, address string) *client.Client {
	t.Helper()
	c := client.New(&testTransport{address: address})
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := c.Connect(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = c.Close() })
	return c
}
func testContext(t *testing.T) context.Context {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	t.Cleanup(cancel)
	return ctx
}

func TestUpstreamClientsShareStateAndKeepRepliesIndependent(t *testing.T) {
	id := testIdentity(1)
	_, _, addr := startTestServer(t, id, testConfig(stateDir(t)))
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	for _, c := range []*client.Client{a, b} {
		info, err := c.DeviceQuery(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if info.FirmwareVersion != 13 || info.MaxContacts != 350 || info.MaxChannels != 40 ||
			info.RepeatEnabled || info.Model != "MeshCore Go TCP host" {
			t.Fatalf("device info: %+v", info)
		}
		self, err := c.AppStart(ctx, 3, "upstream test")
		if err != nil {
			t.Fatal(err)
		}
		if self.PublicKey != id.PublicKey() || self.Name != "Host test" || self.RadioFrequency != 910525 || self.RadioBandwidth != 62500 {
			t.Fatalf("self info: %+v", self)
		}
	}
	ch := meshcore.NewChannelFromHashtag("#test")
	if err := a.SetChannel(ctx, 2, ch.Name, ch.PSK); err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i, c := range []*client.Client{a, b} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for range 30 {
				if i == 0 {
					v, err := c.GetDeviceTime(ctx)
					if err != nil || v.Timestamp < 1_700_000_000 {
						t.Errorf("time reply: %+v, %v", v, err)
						return
					}
				} else {
					v, err := c.GetChannel(ctx, 2)
					if err != nil || v.Name != ch.Name || v.Secret != ch.PSK {
						t.Errorf("channel reply: %+v, %v", v, err)
						return
					}
				}
			}
		}()
	}
	wg.Wait()
	if err := b.SetRadioParams(ctx, 910525, 62500, 7, 5); err != nil {
		t.Fatal(err)
	}
	if err := a.SetRadioParams(ctx, 915000, 62500, 7, 5); err == nil {
		t.Fatal("conflicting physical tuning accepted")
	}
	if _, err := a.GetBattAndStorage(ctx); err == nil {
		t.Fatal("fabricated battery accepted")
	}
	if err := a.SetTxPower(ctx, 19); err == nil {
		t.Fatal("conflicting power accepted")
	}
}

func TestContactsChannelsAndConfigSurviveRestart(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	cfg := testConfig(stateDir(t))
	s, _, addr := startTestServer(t, id, cfg)
	c := testClient(t, addr)
	ctx := testContext(t)
	cmd := protocol.AddUpdateContactCommand{PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeRoom, Flags: 1, OutPathLen: 2, OutPath: []byte{0xa1, 0xb2}, Name: "Room"}
	if err := c.AddUpdateContactFull(ctx, cmd); err != nil {
		t.Fatal(err)
	}
	ch := meshcore.NewChannelFromHashtag("#persistent")
	if err := c.SetChannel(ctx, 7, ch.Name, ch.PSK); err != nil {
		t.Fatal(err)
	}
	if err := c.SetAdvertName(ctx, "Station"); err != nil {
		t.Fatal(err)
	}
	if err := c.SetAdvertLatLon(ctx, 49123456, -123456789); err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	_, _, addr2 := startTestServer(t, id, cfg)
	d := testClient(t, addr2)
	contacts, err := d.GetContacts(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(contacts) != 1 || contacts[0].PublicKey != peer.PublicKey() || contacts[0].OutPathLen != 2 || contacts[0].OutPath[1] != 0xb2 || contacts[0].Flags != 1 {
		t.Fatalf("contacts: %+v", contacts)
	}
	got, err := d.GetChannel(ctx, 7)
	if err != nil || got.Secret != ch.PSK {
		t.Fatalf("channel: %+v %v", got, err)
	}
	info, err := d.AppStart(ctx, 3, "test")
	if err != nil || info.Name != "Station" || info.AdvertLatitude != 49123456 || info.AdvertLongitude != -123456789 {
		t.Fatalf("self: %+v %v", info, err)
	}
	if err := d.RemoveContact(ctx, peer.Identity); err != nil {
		t.Fatal(err)
	}
	contacts, err = d.GetContacts(ctx)
	if err != nil || len(contacts) != 0 {
		t.Fatalf("remove: %+v %v", contacts, err)
	}
	if _, err := New(testIdentity(3), newTestRadio(), cfg); err == nil {
		t.Fatal("state accepted another identity")
	}
}

func TestAdvertNameRejectsLossyRuntimeUpdatesAndSurvivesRestart(t *testing.T) {
	id := testIdentity(1)
	cfg := testConfig(stateDir(t))
	s, _, _ := startTestServer(t, id, cfg)
	for _, bad := range [][]byte{{0xff}, []byte("bad\x00name")} {
		command := append([]byte{protocol.CmdSetAdvertName}, bad...)
		if reply := runCommand(t, s, command); !bytes.Equal(reply, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}) {
			t.Fatalf("invalid advert name %x accepted: %x", bad, reply)
		}
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	reloaded, _, address := startTestServer(t, id, cfg)
	client := testClient(t, address)
	info, err := client.AppStart(testContext(t), 3, "rename check")
	if err != nil || info.Name != cfg.Name {
		t.Fatalf("rejected rename changed saved name: %+v, %v", info, err)
	}
	for _, name := range []string{"bad\x00name", string([]byte{0xff})} {
		invalid := testConfig("")
		invalid.Name = name
		if _, err := New(id, newTestRadio(), invalid); err == nil {
			t.Fatalf("invalid initial advert name %q accepted", name)
		}
	}
	if err := reloaded.Close(); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cfg.StateDir, "companion.json")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var persisted state
	if err := json.Unmarshal(data, &persisted); err != nil {
		t.Fatal(err)
	}
	persisted.Name = "bad\x00name"
	corrupt, err := json.Marshal(persisted)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, corrupt, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := New(id, newTestRadio(), cfg); err == nil {
		t.Fatal("corrupt persisted advert name was silently accepted")
	}
}

func injectText(t *testing.T, r *testRadio, from, to meshcore.LocalIdentity, timestamp uint32, attempt int, text string) {
	t.Helper()
	secret, err := from.SharedSecret(to.Identity)
	if err != nil {
		t.Fatal(err)
	}
	plain := meshcore.BuildTextPlaintextWithAttempt(time.Unix(int64(timestamp), 0), 0, []byte(text), attempt)
	msg, err := meshcore.NewTextMessage(from, to.Identity, plain, secret)
	if err != nil {
		t.Fatal(err)
	}
	payload, _ := msg.ToBytes()
	r.inject(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeTxtMsg, 0), Payload: payload, SNR: 3.25, HasSignalInfo: true})
}
func waitMessage(t *testing.T, ch <-chan struct{}) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(3 * time.Second):
		t.Fatal("message notification missing")
	}
}
func watchMessages(c *client.Client) <-chan struct{} {
	out := make(chan struct{}, 16)
	c.OnPush(protocol.PushMsgWaiting, func(protocol.Response) { out <- struct{}{} })
	return out
}

func TestPrivateAndGroupMessagesReachEveryClientAndReplayAfterRestart(t *testing.T) {
	id, peer := testIdentity(1), testIdentity(2)
	cfg := testConfig(stateDir(t))
	s, r, addr := startTestServer(t, id, cfg)
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	a.DeviceQuery(ctx)
	b.DeviceQuery(ctx)
	if err := a.AddUpdateContact(ctx, peer.Identity, "Peer"); err != nil {
		t.Fatal(err)
	}
	aw, bw := watchMessages(a), watchMessages(b)
	injectText(t, r, peer, id, 100, 0, "hello")
	waitMessage(t, aw)
	waitMessage(t, bw)
	for _, c := range []*client.Client{a, b} {
		msgs, err := c.GetWaitingMessages(ctx)
		if err != nil || len(msgs) != 1 || msgs[0].Contact.Text != "hello" {
			t.Fatalf("private messages: %+v %v", msgs, err)
		}
		msgs, err = c.GetWaitingMessages(ctx)
		if err != nil || len(msgs) != 0 {
			t.Fatalf("duplicate poll: %+v %v", msgs, err)
		}
	}
	ack := r.packet(t)
	if ack.PayloadType() != meshcore.PayloadTypeAck {
		t.Fatalf("no ACK: %+v", ack)
	}
	injectText(t, r, peer, id, 100, 1, "hello")
	r.packet(t) // A retry is acknowledged, without duplicating the application message.
	msgs, err := b.GetWaitingMessages(ctx)
	if err != nil || len(msgs) != 0 {
		t.Fatalf("retry duplicated: %+v %v", msgs, err)
	}
	ch, _ := meshcore.NewChannelFromBase64("Public", "izOH6cXN6mrJ5e26oRXNcg==")
	group, _ := (&meshcore.GroupTextPayload{Timestamp: 101, Sender: "Remote", Text: "group hello"}).Encrypt(ch.Hash, ch.PSK[:])
	payload, _ := group.ToBytes()
	r.inject(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeGrpTxt, 0), Payload: payload})
	waitMessage(t, aw)
	waitMessage(t, bw)
	for _, c := range []*client.Client{a, b} {
		msgs, err := c.GetWaitingMessages(ctx)
		if err != nil || len(msgs) != 1 || !msgs[0].IsChannel || msgs[0].Channel.Text != "Remote: group hello" {
			t.Fatalf("group messages: %+v %v", msgs, err)
		}
	}
	s.Close()
	_, _, addr2 := startTestServer(t, id, cfg)
	d := testClient(t, addr2)
	d.DeviceQuery(ctx)
	msgs, err = d.GetWaitingMessages(ctx)
	if err != nil || len(msgs) != 2 || msgs[0].Contact.Text != "hello" || msgs[1].Channel.Text != "Remote: group hello" {
		t.Fatalf("durable replay: %+v %v", msgs, err)
	}
}

func TestMalformedFramesDisconnectOnlyOffendingClient(t *testing.T) {
	_, _, addr := startTestServer(t, testIdentity(1), testConfig(""))
	healthy := testClient(t, addr)
	ctx := testContext(t)
	for _, wire := range [][]byte{{'>', 1, 0, 5}, {'<', 0, 0}, {'<', 177, 0}} {
		c, err := net.Dial("tcp", addr)
		if err != nil {
			t.Fatal(err)
		}

		c.SetDeadline(time.Now().Add(time.Second))
		c.Write(wire)
		var b [1]byte
		n, err := c.Read(b[:])
		if n != 0 || err == nil {
			t.Fatalf("malformed frame accepted: %x", wire)
		}
		c.Close()
		if _, err := healthy.GetDeviceTime(ctx); err != nil {
			t.Fatal(err)
		}
	}
	raw, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	raw.SetDeadline(time.Now().Add(3 * time.Second))
	// Coalesced frames and one-byte fragmentation are both legal TCP delivery patterns.
	wire, _ := protocol.FrameEncode('<', []byte{protocol.CmdSetChannel, 0})
	timeWire, _ := protocol.FrameEncode('<', []byte{protocol.CmdGetDeviceTime})
	wire = append(wire, timeWire...)
	for _, v := range wire {
		if _, err := raw.Write([]byte{v}); err != nil {
			t.Fatal(err)
		}
	}
	f, err := readFrame(raw)
	if err != nil || !bytes.Equal(f, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}) {
		t.Fatalf("bad command: %x %v", f, err)
	}
	f, err = readFrame(raw)
	if err != nil || f[0] != protocol.RespCurrTime {
		t.Fatalf("next frame: %x %v", f, err)
	}
}

func TestClientProtocolVersionsRemainIndependent(t *testing.T) {
	s, _, addr := startTestServer(t, testIdentity(1), testConfig(""))
	var conns []net.Conn
	for _, version := range []byte{2, 3, 13} {
		conn, err := net.Dial("tcp", addr)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { conn.Close() })
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		wire, _ := protocol.FrameEncode('<', []byte{protocol.CmdDeviceQuery, version})
		if _, err := conn.Write(wire); err != nil {
			t.Fatal(err)
		}
		info, err := readFrame(conn)
		if err != nil {
			t.Fatal(err)
		}
		if len(info) != 82 || info[0] != protocol.RespDeviceInfo || info[1] != 13 {
			t.Fatalf("target %d received invalid protocol 13 device info: %x", version, info)
		}
		if string(bytes.TrimRight(info[60:80], "\x00")) != "host-v2-slp-birch" {
			t.Fatalf("target %d received invalid profile version: %q", version, info[60:80])
		}
		parsed, err := protocol.ParseResponse(info)
		if err != nil || parsed.Data.(protocol.DeviceInfoResponse).FirmwareVersion != 13 {
			t.Fatalf("target %d could not parse device info: %+v %v", version, parsed, err)
		}
		conns = append(conns, conn)
	}
	s.mu.Lock()
	versions := make(map[string]byte, len(s.clients))
	for c := range s.clients {
		versions[c.conn.RemoteAddr().String()] = c.version
	}
	s.mu.Unlock()
	for i, conn := range conns {
		got, ok := versions[conn.LocalAddr().String()]
		if want := []byte{2, 3, 13}[i]; !ok || got != want {
			t.Fatalf("client %d session target = %d (found %t), want %d", i, got, ok, want)
		}
	}
	s.mu.Lock()
	s.queueMessage([]byte{protocol.RespChannelMsgRecvV3, 13, 0, 0, 0, 255, 0, 123, 0, 0, 0, 'h', 'i'})
	s.mu.Unlock()
	for i, conn := range conns {
		wire, _ := protocol.FrameEncode('<', []byte{protocol.CmdSyncNextMessage})
		if _, err := conn.Write(wire); err != nil {
			t.Fatal(err)
		}
		for {
			frame, err := readFrame(conn)
			if err != nil {
				t.Fatal(err)
			}
			if frame[0] >= 128 {
				continue
			}
			want := []byte{protocol.RespChannelMsgRecv, protocol.RespChannelMsgRecvV3, protocol.RespChannelMsgRecvV3}[i]
			if frame[0] != want {
				t.Fatalf("client %d: got protocol message %d, want %d", i, frame[0], want)
			}
			if _, err := protocol.ParseResponse(frame); err != nil {
				t.Fatal(err)
			}
			break
		}
	}
}

func TestTruncatedCommandsReturnErrorsWithoutLosingFraming(t *testing.T) {
	_, _, addr := startTestServer(t, testIdentity(1), testConfig(""))
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(3 * time.Second))
	for _, tc := range []struct {
		command byte
		minimum int
	}{
		{protocol.CmdAppStart, 8}, {protocol.CmdSendTxtMsg, 14},
		{protocol.CmdSendChannelTxtMsg, 8}, {protocol.CmdAddUpdateContact, 36},
		{protocol.CmdSetRadioParams, 11}, {protocol.CmdSetAdvertLatLon, 9},
		{protocol.CmdSetChannel, 50}, {protocol.CmdSendPathDiscoveryReq, 34},
		{protocol.CmdSendLogin, 33}, {protocol.CmdGetAdvertPath, 34},
	} {
		for length := 1; length < tc.minimum; length++ {
			cmd := make([]byte, length)
			cmd[0] = tc.command
			wire, _ := protocol.FrameEncode('<', cmd)
			if _, err := conn.Write(wire); err != nil {
				t.Fatal(err)
			}
			frame, err := readFrame(conn)
			if err != nil || len(frame) != 2 || frame[0] != protocol.RespErr {
				t.Fatalf("command %d length %d: %x %v", tc.command, length, frame, err)
			}
		}
	}
}

func TestSelfInfoAndRadioChecksFollowEffectivePHY(t *testing.T) {
	var mu sync.Mutex
	live, power, online := hardware.RadioConfig{FreqHz: 915000000, BwHz: 250000, SF: 12, CR: 8}, int8(14), true
	cfg := testConfig(stateDir(t))
	cfg.EffectivePHY = func() (hardware.RadioConfig, int8, bool) {
		mu.Lock()
		defer mu.Unlock()
		return live, power, online
	}
	s, _, addr := startTestServer(t, testIdentity(1), cfg)
	c := testClient(t, addr)
	ctx := testContext(t)
	self, err := c.AppStart(ctx, 3, "live phy")
	if err != nil {
		t.Fatal(err)
	}
	if self.RadioFrequency != 915000 || self.RadioBandwidth != 250000 || self.RadioSpreadFactor != 12 ||
		self.RadioCodingRate != 8 || self.TxPower != 14 {
		t.Fatalf("SELF_INFO did not report the effective PHY: %+v", self)
	}
	if err := c.SetRadioParams(ctx, 910525, 62500, 7, 5); err == nil {
		t.Fatal("stale configured tuning accepted while the mast runs another profile")
	}
	conflicting := make([]byte, 11)
	conflicting[0] = protocol.CmdSetRadioParams
	binary.LittleEndian.PutUint32(conflicting[1:], 910525)
	binary.LittleEndian.PutUint32(conflicting[5:], 62500)
	conflicting[9], conflicting[10] = 7, 5
	if got := runCommand(t, s, conflicting); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}) {
		t.Fatalf("conflicting client retune did not report native unsupported-owner error: %x", got)
	}
	if got := runCommand(t, s, []byte{protocol.CmdSetRadioTxPower, 20}); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}) {
		t.Fatalf("conflicting client power did not report native unsupported-owner error: %x", got)
	}
	if err := c.SetRadioParams(ctx, 915000, 250000, 12, 8); err != nil {
		t.Fatalf("effective tuning rejected: %v", err)
	}
	if err := c.SetTxPower(ctx, 14); err != nil {
		t.Fatalf("effective power rejected: %v", err)
	}
	packet := &meshcore.Packet{SNR: -8, HasSignalInfo: true}
	if got := s.rxScore(packet, 64); got != float32(hardware.PacketScore(-8, 12, 64)) {
		t.Fatalf("RX score did not use effective SF: %v", got)
	}

	// A mast return is visible to the next query without restarting the server.
	mu.Lock()
	live, power = cfg.RadioConfig, cfg.TxPower
	mu.Unlock()
	self, err = c.AppStart(ctx, 3, "live phy")
	if err != nil || self.RadioFrequency != 910525 || self.RadioSpreadFactor != 7 || self.TxPower != 20 {
		t.Fatalf("SELF_INFO after return: %+v %v", self, err)
	}
	if err := c.SetTxPower(ctx, 14); err == nil {
		t.Fatal("previous effective power accepted after return")
	}

	// Unverified (offline) falls back to the configured profile.
	mu.Lock()
	live, power, online = hardware.RadioConfig{FreqHz: 1, SF: 5}, 1, false
	mu.Unlock()
	if err := c.SetRadioParams(ctx, 910525, 62500, 7, 5); err != nil {
		t.Fatalf("configured fallback rejected: %v", err)
	}
	if got := s.rxScore(packet, 64); got != float32(hardware.PacketScore(-8, 7, 64)) {
		t.Fatalf("offline RX score: %v", got)
	}
}
