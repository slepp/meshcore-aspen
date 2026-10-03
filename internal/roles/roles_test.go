package roles

import (
	"bytes"
	"crypto/aes"
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
)

type wireRadio struct {
	mu           sync.Mutex
	handler      func(*meshcore.Packet)
	outbound     []func([]byte)
	output       chan []byte
	closed       bool
	closeError   error
	rejectAdvert bool
	jobs         []wireJob
}

type wireJob struct {
	wire     []byte
	priority uint8
	delay    time.Duration
}

func newWireRadio() *wireRadio { return &wireRadio{output: make(chan []byte, 128)} }
func (r *wireRadio) SetDataHandler(h func(*meshcore.Packet)) {
	r.mu.Lock()
	r.handler = h
	r.mu.Unlock()
}
func (r *wireRadio) SetRawDataHandler(func([]byte, float32, int8, bool)) {}
func (r *wireRadio) AddOutboundHandler(h func([]byte)) {
	r.mu.Lock()
	r.outbound = append(r.outbound, h)
	r.mu.Unlock()
}
func (r *wireRadio) SendData(b []byte) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return errors.New("closed test radio")
	}
	r.output <- bytes.Clone(b)
	for _, h := range r.outbound {
		h(b)
	}
	return nil
}
func (r *wireRadio) Enqueue(b []byte, priority uint8, delay time.Duration) bool {
	r.mu.Lock()
	rejected := r.rejectAdvert && len(b) > 0 && (b[0]>>2)&15 == 4
	if !rejected {
		r.jobs = append(r.jobs, wireJob{bytes.Clone(b), priority, delay})
	}
	r.mu.Unlock()
	return !rejected && r.SendData(b) == nil
}
func (r *wireRadio) TxQueueLen() int { return 0 }
func (r *wireRadio) Close() error    { r.mu.Lock(); r.closed = true; r.mu.Unlock(); return r.closeError }
func (r *wireRadio) inject(t *testing.T, raw []byte, local bool) {
	t.Helper()
	p, err := meshcore.PacketFromBytes(raw)
	if err != nil {
		t.Fatal(err)
	}
	p.HasSignalInfo, p.SNR, p.RSSI = true, 4.5, -73
	if local {
		p.SNR, p.RSSI = -32, 127
		p.MarkDoNotRetransmit()
	}
	r.mu.Lock()
	h := r.handler
	r.mu.Unlock()
	h(p)
}
func (r *wireRadio) next(t *testing.T) []byte {
	t.Helper()
	select {
	case p := <-r.output:
		return p
	case <-time.After(2 * time.Second):
		t.Fatal("no wire response")
		return nil
	}
}
func (r *wireRadio) quiet(t *testing.T) {
	t.Helper()
	select {
	case p := <-r.output:
		t.Fatalf("unexpected transmission: %x", p)
	case <-time.After(30 * time.Millisecond):
	}
}

func fixedID(seed byte) meshcore.LocalIdentity {
	var b [32]byte
	for i := range b {
		b[i] = seed
	}
	return meshcore.NewLocalIdentityFromSeed(b)
}

func stateDir(t *testing.T) string {
	t.Helper()
	return t.TempDir()
}
func startRole(t *testing.T, room bool, cfg Config) (*Service, *wireRadio, meshcore.LocalIdentity) {
	t.Helper()
	id := fixedID(1)
	r := newWireRadio()
	var s *Service
	var err error
	if room {
		s, err = NewRoom(id, r, cfg)
	} else {
		s, err = NewRepeater(id, r, cfg)
	}
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	advert := r.next(t)
	wantType := byte(0x82)
	if room {
		wantType = 0x83
	}
	if len(advert) < 103 || advert[0] != 0x12 || advert[1] != 0 || advert[102] != wantType ||
		string(advert[103:]) != cfg.Name {
		t.Fatalf("wrong advert wire layout: %x", advert)
	}
	signed := append(bytes.Clone(advert[2:38]), advert[102:]...)
	if !ed25519.Verify(id.PublicKeyBytes(), signed, advert[38:102]) {
		t.Fatal("invalid advert signature")
	}
	return s, r, id
}

func config(t *testing.T) Config {
	return Config{StateDir: stateDir(t), Name: "Host", Password: "room", AdminPassword: "admin", AdvertInterval: time.Hour, Retention: DurableReplay, AirtimeEstimator: func(int) uint32 { return 100 }}
}

func TestRoleRadioManagementUsesVerifiedSharedReadback(t *testing.T) {
	for _, room := range []bool{false, true} {
		cfg := config(t)
		profile := hardware.RadioConfig{FreqHz: 912525000, BwHz: 250000, SF: 7, CR: 5}
		available := true
		cfg.CurrentRadio = func() (hardware.RadioConfig, bool) { return profile, available }
		cfg.CurrentPower = func() (uint8, bool) { return 19, available }
		s, _, _ := startRole(t, room, cfg)
		command := func(text string) string {
			t.Helper()
			s.mu.Lock()
			defer s.mu.Unlock()
			reply, _, err := s.command(text)
			if err != nil {
				t.Fatal(err)
			}
			return reply
		}
		if got := command("get radio"); got != "> 912.5250244,250,7,5" {
			t.Fatalf("room=%v initial readback: %q", room, got)
		}
		if got := command("get tx"); got != "> 19" {
			t.Fatalf("room=%v initial power readback: %q", room, got)
		}
		if got := command("get freq"); got != "> 912.5250244" {
			t.Fatalf("room=%v initial frequency readback: %q", room, got)
		}
		profile = hardware.RadioConfig{FreqHz: 915000000, BwHz: 62500, SF: 9, CR: 6}
		if got := command("get radio"); got != "> 915.0,62.5,9,6" {
			t.Fatalf("room=%v retuned readback: %q", room, got)
		}
		if got := command("get freq"); got != "> 915.0" {
			t.Fatalf("room=%v retuned frequency readback: %q", room, got)
		}
		if got := command("set radio 912.525,250,7,5"); got != sharedPHYOwnerRequired {
			t.Fatalf("room=%v role-local retune: %q", room, got)
		}
		if got := command("set tx 19"); got != sharedPHYOwnerRequired {
			t.Fatalf("room=%v role-local transmit power: %q", room, got)
		}
		for _, write := range []string{"set radio", "set freq 912", "tempradio 912.525,250,7,5,1"} {
			if got := command(write); got != sharedPHYOwnerRequired {
				t.Fatalf("room=%v role-local shared-PHY write %q: %q", room, write, got)
			}
		}
		if got := command("get radio"); got != "> 915.0,62.5,9,6" {
			t.Fatalf("room=%v rejected retune changed physical readback: %q", room, got)
		}
		available = false
		if got := command("get radio"); got != "Error: shared radio readback unavailable" {
			t.Fatalf("room=%v offline readback: %q", room, got)
		}
		if got := command("get freq"); got != "Error: shared radio readback unavailable" {
			t.Fatalf("room=%v offline frequency readback: %q", room, got)
		}
		if got := command("get tx"); got != "Error: shared radio readback unavailable" {
			t.Fatalf("room=%v offline power readback: %q", room, got)
		}
		s.cfg.CurrentRadio = nil
		if got := command("get radio"); got != "Error: shared radio readback unavailable" {
			t.Fatalf("room=%v missing readback: %q", room, got)
		}
		s.cfg.CurrentPower = nil
		if got := command("get tx"); got != "Error: shared radio readback unavailable" {
			t.Fatalf("room=%v missing power readback: %q", room, got)
		}
	}
}

// These wire helpers use the firmware primitives directly, not the library's
// message builders/parsers, so a shared layout or ACK-key mistake is observable.
func seal(t *testing.T, key, plaintext []byte) []byte {
	t.Helper()
	block, err := aes.NewCipher(key[:16])
	if err != nil {
		t.Fatal(err)
	}
	padded := make([]byte, (len(plaintext)+15)/16*16)
	copy(padded, plaintext)
	for n := 0; n < len(padded); n += 16 {
		block.Encrypt(padded[n:n+16], padded[n:n+16])
	}
	mac := hmac.New(sha256.New, key)
	mac.Write(padded)
	return append(mac.Sum(nil)[:2], padded...)
}
func open(t *testing.T, key, data []byte) []byte {
	t.Helper()
	if len(data) < 18 || (len(data)-2)%16 != 0 {
		t.Fatalf("invalid cipher length %d", len(data))
	}
	mac := hmac.New(sha256.New, key)
	mac.Write(data[2:])
	if !hmac.Equal(mac.Sum(nil)[:2], data[:2]) {
		t.Fatal("wrong response MAC")
	}
	block, err := aes.NewCipher(key[:16])
	if err != nil {
		t.Fatal(err)
	}
	out := bytes.Clone(data[2:])
	for n := 0; n < len(out); n += 16 {
		block.Decrypt(out[n:n+16], out[n:n+16])
	}
	return out
}
func secret(t *testing.T, a meshcore.LocalIdentity, b meshcore.Identity) []byte {
	t.Helper()
	key, err := a.SharedSecret(b)
	if err != nil {
		t.Fatal(err)
	}
	return key
}
func wirePacket(kind, route, pathLen byte, path, payload []byte) []byte {
	out := append([]byte{kind<<2 | route, pathLen}, path...)
	return append(out, payload...)
}
func loginWire(t *testing.T, room bool, client, server meshcore.LocalIdentity, timestamp, since uint32, password string, route, pathLen byte, path []byte) []byte {
	t.Helper()
	plain := binary.LittleEndian.AppendUint32(nil, timestamp)
	if room {
		plain = binary.LittleEndian.AppendUint32(plain, since)
	}
	plain = append(plain, password...)
	plain = append(plain, 0)
	payload := append([]byte{server.PublicKey()[0]}, client.PublicKeyBytes()...)
	payload = append(payload, seal(t, secret(t, client, server.Identity), plain)...)
	return wirePacket(7, route, pathLen, path, payload)
}
func peerWire(t *testing.T, kind byte, client, server meshcore.LocalIdentity, plain []byte) []byte {
	t.Helper()
	payload := []byte{server.PublicKey()[0], client.PublicKey()[0]}
	payload = append(payload, seal(t, secret(t, client, server.Identity), plain)...)
	return wirePacket(kind, 2, 0, nil, payload)
}
func textWire(t *testing.T, client, server meshcore.LocalIdentity, stamp uint32, flags byte, text string) []byte {
	t.Helper()
	plain := binary.LittleEndian.AppendUint32(nil, stamp)
	plain = append(plain, flags)
	return peerWire(t, 2, client, server, append(plain, text...))
}
func payloadOffset(t *testing.T, raw []byte) int {
	t.Helper()
	if len(raw) < 2 {
		t.Fatal("short packet")
	}
	header := 1
	if raw[0]&3 == 0 || raw[0]&3 == 3 {
		header += 4
	}
	if len(raw) <= header {
		t.Fatal("short scoped packet")
	}
	length := raw[header]
	offset := header + 1 + int(length&63)*(int(length>>6)+1)
	if offset > len(raw) {
		t.Fatal("short path")
	}
	return offset
}
func decrypted(t *testing.T, raw []byte, client, server meshcore.LocalIdentity) []byte {
	t.Helper()
	p := raw[payloadOffset(t, raw):]
	if len(p) < 4 || p[0] != client.PublicKey()[0] || p[1] != server.PublicKey()[0] {
		t.Fatalf("wrong datagram addressing: %x", p)
	}
	return open(t, secret(t, client, server.Identity), p[2:])
}
func ackProof(plain []byte, key [32]byte) []byte {
	h := sha256.New()
	h.Write(plain)
	h.Write(key[:])
	return h.Sum(nil)[:4]
}
func await(t *testing.T, condition func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for !condition() {
		if time.Now().After(deadline) {
			t.Fatal("condition not reached")
		}
		time.Sleep(time.Millisecond)
	}
}
func loggedIn(t *testing.T, s *Service, r *wireRadio, client, server meshcore.LocalIdentity, room bool, timestamp uint32, password string) {
	t.Helper()
	r.inject(t, loginWire(t, room, client, server, timestamp, 0, password, 2, 0, nil), false)
	raw := r.next(t)
	if raw[0] != 0x05 {
		t.Fatalf("unknown return path must flood RESPONSE: %x", raw)
	}
	plain := decrypted(t, raw, client, server)
	perm := byte(0)
	if room {
		perm = 2
	}
	admin := byte(0)
	if password == "admin" {
		perm, admin = 3, 1
	}
	level := byte(1)
	if !room {
		level = 2
	}
	if !bytes.Equal(plain[4:8], []byte{0, 0, admin, perm}) || plain[12] != level {
		t.Fatalf("login response fields: %x", plain)
	}
}
func learnPath(t *testing.T, s *Service, r *wireRadio, client, server meshcore.LocalIdentity, length byte, path []byte) {
	t.Helper()
	body := append([]byte{length}, path...)
	body = append(body, 0x0f, 1, 2, 3, 4)
	r.inject(t, peerWire(t, 8, client, server, body), false)
	await(t, func() bool {
		s.mu.RLock()
		defer s.mu.RUnlock()
		m := s.state.Members[client.Identity.String()]
		return m != nil && m.KnownPath && m.PathLength == length
	})
}

func driveSync(t *testing.T, s *Service, now time.Time, count int) []transmission {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	var packets []transmission
	for range count {
		p, err := s.syncAt(now)
		if err != nil {
			t.Fatal(err)
		}
		packets = append(packets, p...)
		now = now.Add(150 * time.Millisecond)
	}
	return packets
}

func TestRepeaterRoutingDedupAndLocalDelivery(t *testing.T) {
	cfg := config(t)
	_, r, id := startRole(t, false, cfg)
	flood := []byte{0x15, 0, 0x22, 0x33, 0x44}
	r.inject(t, flood, false)
	want := []byte{0x15, 1, id.PublicKey()[0], 0x22, 0x33, 0x44}
	if got := r.next(t); !bytes.Equal(got, want) {
		t.Fatalf("flood relay %x, want %x", got, want)
	}
	assertWirePriority(t, r, want, 1)
	r.inject(t, flood, false)
	r.quiet(t)
	r.inject(t, []byte{0x15, 0, 0x22, 0x33, 0x45}, true)
	r.quiet(t)
	direct := []byte{0x16, 2, id.PublicKey()[0], 0x91, 0x55, 0x66}
	r.inject(t, direct, false)
	if got := r.next(t); !bytes.Equal(got, []byte{0x16, 1, 0x91, 0x55, 0x66}) {
		t.Fatalf("direct relay %x", got)
	}
	assertWirePriority(t, r, []byte{0x16, 1, 0x91, 0x55, 0x66}, 0)
	direct[len(direct)-1]++
	r.inject(t, direct, true)
	r.quiet(t)
	client := fixedID(2)
	r.inject(t, loginWire(t, false, client, id, 1234, 0, "admin", 2, 1, []byte{id.PublicKey()[0]}), true)
	reply := r.next(t)
	if reply[0] != 0x05 || !bytes.Equal(decrypted(t, reply, client, id)[4:8], []byte{0, 0, 1, 3}) {
		t.Fatalf("local addressed login not handled: %x", reply)
	}
	r.quiet(t)
}

func TestRoomLoginPathPostACKAndPersistence(t *testing.T) {
	cfg := config(t)
	s, r, id := startRole(t, true, cfg)
	alice, bob := fixedID(2), fixedID(3)
	r.inject(t, loginWire(t, true, alice, id, 10, 0, "wrong", 2, 0, nil), false)
	r.quiet(t)
	s.mu.RLock()
	n := len(s.state.Members)
	s.mu.RUnlock()
	if n != 0 {
		t.Fatal("wrong password created membership")
	}
	path := []byte{0xaa, 0xbb, 0xcc, 0xdd}
	r.inject(t, loginWire(t, true, alice, id, 10, 0, "room", 1, 0x42, path), false)
	reply := r.next(t)
	if reply[0] != 0x21 || reply[1] != 0x40 {
		t.Fatalf("flood login must return two-byte PATH: %x", reply)
	}
	plain := decrypted(t, reply, alice, id)
	if !bytes.Equal(plain[:6], []byte{0x42, 0xaa, 0xbb, 0xcc, 0xdd, 1}) ||
		!bytes.Equal(plain[10:14], []byte{0, 0, 0, 2}) || plain[18] != 1 {
		t.Fatalf("PATH login body %x", plain)
	}
	loggedIn(t, s, r, bob, id, true, 20, "room")
	learnPath(t, s, r, bob, id, 0x81, []byte{0x12, 0x34, 0x56})
	r.inject(t, textWire(t, alice, id, 11, 0, "hello"), false)
	postPlain := []byte{11, 0, 0, 0, 0, 'h', 'e', 'l', 'l', 'o'}
	wantACK := append([]byte{0x0d, 0}, ackProof(postPlain, alice.PublicKey())...)
	if got := r.next(t); !bytes.Equal(got, wantACK) {
		t.Fatalf("post ACK %x want %x", got, wantACK)
	}
	r.inject(t, textWire(t, alice, id, 11, 1, "hello"), false)
	postPlain[4] = 1
	wantACK = append([]byte{0x0d, 0}, ackProof(postPlain, alice.PublicKey())...)
	if got := r.next(t); !bytes.Equal(got, wantACK) {
		t.Fatalf("retry must ACK its own attempt: %x want %x", got, wantACK)
	}
	s.mu.RLock()
	count := len(s.state.History)
	s.mu.RUnlock()
	if count != 1 {
		t.Fatal("post retry duplicated history")
	}
	r.inject(t, textWire(t, alice, id, 10, 0, "old"), false)
	r.quiet(t)
	now := time.Now().Add(time.Minute)
	pushes := driveSync(t, s, now, 2)
	s.transmit(pushes, nil)
	push := r.next(t)
	if !bytes.Equal(push[:5], []byte{0x0a, 0x81, 0x12, 0x34, 0x56}) {
		t.Fatalf("learned direct path ignored: %x", push)
	}
	body := decrypted(t, push, bob, id)
	if body[4]&^3 != 8 || !bytes.Equal(body[5:9], alice.PublicKeyBytes()[:4]) || string(body[9:14]) != "hello" {
		t.Fatalf("signed post layout: %x", body)
	}
	wrong := ackProof(body[:14], id.PublicKey())
	r.inject(t, wirePacket(3, 2, 0, nil, wrong), false)
	r.quiet(t)
	s.mu.RLock()
	pending := s.state.Members[bob.Identity.String()].Pending
	s.mu.RUnlock()
	if !pending {
		t.Fatal("sender-key ACK incorrectly accepted for room post")
	}
	r.inject(t, wirePacket(3, 2, 0, nil, ackProof(body[:14], bob.PublicKey())), false)
	await(t, func() bool {
		s.mu.RLock()
		defer s.mu.RUnlock()
		return s.state.Members[bob.Identity.String()].SyncSince == u32(body)
	})
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s2, r2, _ := startRole(t, true, cfg)
	s2.mu.RLock()
	m := *s2.state.Members[bob.Identity.String()]
	history := append([]post(nil), s2.state.History...)
	s2.mu.RUnlock()
	if len(history) != 1 || history[0].Text != "hello" || m.SyncSince != u32(body) || m.Active ||
		!m.KnownPath || m.PathLength != 0x81 {
		t.Fatalf("restart state: member=%+v history=%+v", m, history)
	}
	r2.inject(t, loginWire(t, true, bob, id, 20, 0, "room", 2, 0, nil), false)
	r2.quiet(t)
	r2.inject(t, loginWire(t, true, bob, id, 21, m.SyncSince, "", 2, 0, nil), false)
	if got := r2.next(t); got[0] != 0x06 || got[1] != 0x81 {
		t.Fatalf("persisted ACL/path login response: %x", got)
	}
	r2.quiet(t)
}

func TestRoomRetryBoundAndKeepAliveProof(t *testing.T) {
	s, r, id := startRole(t, true, config(t))
	alice, bob := fixedID(2), fixedID(3)
	loggedIn(t, s, r, alice, id, true, 1, "room")
	loggedIn(t, s, r, bob, id, true, 1, "room")
	learnPath(t, s, r, bob, id, 0, nil)
	s.mu.Lock()
	randomAttempt := byte(0)
	s.random = func(b []byte) (int, error) {
		b[0] = randomAttempt
		randomAttempt++
		return len(b), nil
	}
	s.mu.Unlock()
	r.inject(t, textWire(t, alice, id, 2, 0, "retry"), false)
	r.next(t)
	now := time.Now().Add(time.Minute)
	var previousCRC []byte
	for attempt := 0; attempt < 3; attempt++ {
		packets := driveSync(t, s, now, 2)
		s.transmit(packets, nil)
		raw := r.next(t)
		body := decrypted(t, raw, bob, id)
		if raw[0] != 0x0a || raw[1] != 0 || body[4] != byte(8+attempt) {
			t.Fatalf("retry %d has wrong flags/path: %x %x", attempt, raw, body)
		}

		crc := ackProof(body[:14], bob.PublicKey())
		if bytes.Equal(previousCRC, crc) {
			t.Fatal("retry reused ACK")
		}
		if previousCRC != nil {
			r.inject(t, wirePacket(3, 2, 0, nil, previousCRC), false)
		}
		previousCRC = crc
		now = now.Add(20 * time.Second)
	}
	if p := driveSync(t, s, now, 4); len(p) != 0 {
		t.Fatalf("retry limit: %v", p)
	}
	s.mu.Lock()
	failures := s.state.Members[bob.Identity.String()].Failures
	s.mu.Unlock()
	if failures != 3 {
		t.Fatalf("failures=%d", failures)
	}
	r.quiet(t)
	keep := []byte{2, 0, 0, 0, 2, 0, 0, 0, 0}
	r.inject(t, peerWire(t, 0, bob, id, keep), false)
	want := append([]byte{0x0e, 0}, ackProof(keep, bob.PublicKey())...)
	want = append(want, 1)
	if got := r.next(t); !bytes.Equal(got, want) {
		t.Fatalf("keep-alive ACK %x want %x", got, want)
	}
	s.mu.Lock()
	m := s.state.Members[bob.Identity.String()]
	reset := !m.Pending && m.Failures == 0
	s.mu.Unlock()
	if !reset {
		t.Fatal("keep-alive did not resume synchronization")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s2, r2, _ := startRole(t, true, s.cfg)
	r2.inject(t, loginWire(t, true, bob, id, 3, 0, "", 2, 0, nil), false)
	r2.next(t)
	r2.inject(t, peerWire(t, 0, bob, id, []byte{3, 0, 0, 0, 2, 0, 0, 0, 0}), false)
	r2.next(t)
	s2.mu.Lock()
	randomAttempt = 3
	s2.random = func(b []byte) (int, error) {
		b[0] = randomAttempt
		randomAttempt++
		return len(b), nil
	}
	s2.mu.Unlock()
	for attempt := 3; attempt <= 4; attempt++ {
		s2.transmit(driveSync(t, s2, now, 2), nil)
		body := decrypted(t, r2.next(t), bob, id)
		if body[4] != 8|byte(attempt&3) || string(cstring(body[9:])) != "retry" {
			t.Fatalf("native random attempt or post incorrect: %x", body)
		}
		if attempt == 4 {
			if body[14] != 0 || body[15] != 0 {
				t.Fatalf("non-native extended retry tail: %x", body)
			}
			pathACK := []byte{0x42, 0x77, 0x88, 0x99, 0xaa, 3}
			pathACK = append(pathACK, ackProof(body[:14], bob.PublicKey())...)
			r2.inject(t, peerWire(t, 8, bob, id, pathACK), false)
			await(t, func() bool {
				s2.mu.RLock()
				defer s2.mu.RUnlock()
				m := s2.state.Members[bob.Identity.String()]
				return m.SyncSince == u32(body) && !m.Pending && m.PathLength == 0x42
			})
		}
		now = now.Add(20 * time.Second)
	}
}

func TestCloseReturnsRadioErrorAndIsConcurrentSafe(t *testing.T) {
	cfg := config(t)
	r := newWireRadio()
	want := errors.New("radio close failed")
	r.closeError = want
	s, err := NewRoom(fixedID(1), r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	r.next(t)
	var wg sync.WaitGroup
	for range 8 {
		wg.Go(func() {
			if err := s.Close(); !errors.Is(err, want) {
				t.Errorf("Close = %v, want radio error", err)
			}
		})
	}
	wg.Wait()
}

func TestAdminACLReadOnlyHistoryAndUnsupportedHardware(t *testing.T) {
	cfg := config(t)
	cfg.MaxHistory = 2
	s, r, id := startRole(t, true, cfg)
	admin, reader := fixedID(2), fixedID(3)
	loggedIn(t, s, r, admin, id, true, 1, "admin")
	command := "ab|setperm " + reader.Identity.String() + " 1"
	r.inject(t, textWire(t, admin, id, 2, 4, command), false)
	if body := decrypted(t, r.next(t), admin, id); body[4] != 4 || string(cstring(body[5:])) != "ab|OK" {
		t.Fatalf("setperm reply %x", body)
	}
	r.inject(t, loginWire(t, true, reader, id, 1, 0, "", 2, 0, nil), false)
	body := decrypted(t, r.next(t), reader, id)
	if body[7] != 1 {
		t.Fatalf("ACL read-only login: %x", body)
	}
	r.inject(t, textWire(t, reader, id, 2, 0, "forbidden"), false)
	r.quiet(t)
	for i, text := range []string{"one", "two", "three"} {
		r.inject(t, textWire(t, admin, id, uint32(i+3), 4, "room.post "+text), false)
		if got := string(cstring(decrypted(t, r.next(t), admin, id)[5:])); got != "OK" {
			t.Fatal(got)
		}
	}
	r.inject(t, textWire(t, admin, id, 6, 4, "set tx 22"), false)
	if got := string(cstring(decrypted(t, r.next(t), admin, id)[5:])); got != sharedPHYOwnerRequired {
		t.Fatal(got)
	}
	r.inject(t, textWire(t, admin, id, 7, 4, "setperm "+reader.Identity.String()+" 0"), false)
	if got := string(cstring(decrypted(t, r.next(t), admin, id)[5:])); got != "OK" {
		t.Fatal(got)
	}
	s.mu.RLock()
	h := append([]post(nil), s.state.History...)
	_, exists := s.state.Members[reader.Identity.String()]
	s.mu.RUnlock()
	if exists || len(h) != 2 || h[0].Text != "two" || h[1].Text != "three" {
		t.Fatalf("ACL/history bounds: %v %+v", exists, h)
	}
	raw, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.json"))
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(raw, []byte("admin")) || bytes.Contains(raw, []byte("shared_secret")) {
		t.Fatal("state contains password or shared secret")
	}
	info, err := os.Stat(filepath.Join(cfg.StateDir, "state.json"))
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0600 {
		t.Fatalf("state permissions %v", info.Mode())
	}
}

func TestRepeaterStatusAndUnsupportedTelemetry(t *testing.T) {
	cfg := config(t)
	errs := make(chan error, 4)
	cfg.ErrorHandler = func(err error) { errs <- err }
	s, r, id := startRole(t, false, cfg)
	client := fixedID(2)
	loggedIn(t, s, r, client, id, false, 1, "room")
	r.inject(t, peerWire(t, 0, client, id, []byte{2, 0, 0, 0, 1}), false)
	body := decrypted(t, r.next(t), client, id)
	if !bytes.Equal(body[:4], []byte{2, 0, 0, 0}) || binary.LittleEndian.Uint16(body[44:]) != 0 ||
		int16(binary.LittleEndian.Uint16(body[10:])) != -73 || int16(binary.LittleEndian.Uint16(body[46:])) != 18 ||
		u32(body[12:]) != 2 || u32(body[16:]) != 2 {
		t.Fatalf("status ABI/metadata counters: %x", body)
	}

	r.inject(t, peerWire(t, 0, client, id, []byte{3, 0, 0, 0, 3, 0, 0, 0, 0}), false)
	select {
	case err := <-errs:
		if !errors.Is(err, ErrUnsupported) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("unsupported telemetry not reported")
	}
	r.quiet(t)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestRoomSchedulerDeliversWithoutManualSync(t *testing.T) {
	s, r, id := startRole(t, true, config(t))
	alice, bob := fixedID(2), fixedID(3)
	loggedIn(t, s, r, alice, id, true, 1, "room")
	loggedIn(t, s, r, bob, id, true, 1, "room")
	r.inject(t, textWire(t, alice, id, 2, 0, "scheduled"), false)
	r.next(t)
	// Skip the firmware's delivery hold, not the production scheduler.
	s.mu.Lock()
	s.postDelay = -time.Minute
	s.nextPush = time.Time{}
	s.mu.Unlock()
	raw := r.next(t)
	body := decrypted(t, raw, bob, id)
	if raw[0] != 0x09 || body[4]&^3 != 8 || string(cstring(body[9:])) != "scheduled" {
		t.Fatalf("scheduler failed to deliver signed flood post: %x", body)
	}
}

func TestRepeaterAnonymousClockUsesSuppliedMultiBytePath(t *testing.T) {
	_, r, id := startRole(t, false, config(t))
	client := fixedID(2)
	plain := []byte{0x78, 0x56, 0x34, 0x12, 3, 0x42, 0x11, 0x22, 0x33, 0x44}
	payload := append([]byte{id.PublicKey()[0]}, client.PublicKeyBytes()...)
	payload = append(payload, seal(t, secret(t, client, id.Identity), plain)...)
	before := uint32(time.Now().Unix())
	r.inject(t, wirePacket(7, 2, 0, nil, payload), false)
	raw := r.next(t)
	if !bytes.Equal(raw[:6], []byte{0x06, 0x42, 0x11, 0x22, 0x33, 0x44}) {
		t.Fatalf("supplied path not honoured: %x", raw)
	}
	body := decrypted(t, raw, client, id)
	if !bytes.Equal(body[:4], plain[:4]) || u32(body[4:]) < before ||
		u32(body[4:]) > uint32(time.Now().Unix()) || body[8] != 0 {
		t.Fatalf("clock response contract: %x", body)
	}
}

func TestAdvertQueueFailureDoesNotReportCommandSuccess(t *testing.T) {
	cfg := config(t)
	errs := make(chan error, 2)
	cfg.ErrorHandler = func(err error) { errs <- err }
	s, r, id := startRole(t, false, cfg)
	client := fixedID(2)
	loggedIn(t, s, r, client, id, false, 1, "admin")
	r.mu.Lock()
	r.rejectAdvert = true
	r.mu.Unlock()
	r.inject(t, textWire(t, client, id, 2, 4, "advert"), false)
	select {
	case err := <-errs:
		if !strings.Contains(err.Error(), "transmit queue full") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("advert queue failure not reported")
	}
	r.quiet(t)
}

func TestCloseDrainsAcceptedLoginsBeforeRadioShutdown(t *testing.T) {
	cfg := config(t)
	s, r, id := startRole(t, true, cfg)
	var logins [][]byte
	for i := byte(2); i < 22; i++ {
		logins = append(logins, loginWire(t, true, fixedID(i), id, 1, 0, "room", 2, 0, nil))
	}
	closed := make(chan error, 1)
	func() {
		s.mu.Lock()
		defer s.mu.Unlock()
		for _, wire := range logins {
			r.inject(t, wire, false)
		}
		await(t, func() bool { return len(s.queue) >= len(logins)-1 })
		go func() { closed <- s.Close() }()
		select {
		case <-s.done:
		case <-time.After(time.Second):
			t.Fatal("shutdown did not close admission")
		}
	}()
	select {
	case err := <-closed:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("shutdown did not drain queued logins")
	}
	raw, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.json"))
	if err != nil {
		t.Fatal(err)
	}
	var state diskState
	if err := json.Unmarshal(raw, &state); err != nil {
		t.Fatal(err)
	}
	if len(state.Members) != 20 {
		t.Fatalf("shutdown lost accepted logins: persisted %d", len(state.Members))
	}
	for range 20 {
		if reply := r.next(t); reply[0] != 0x05 {
			t.Fatalf("missing login success: %x", reply)
		}
	}
}

func TestStateCorruptionAndIdentityMismatchAreFatal(t *testing.T) {
	cfg := config(t)
	s, _, _ := startRole(t, true, cfg)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if other, err := NewRoom(fixedID(9), newWireRadio(), cfg); err == nil {
		other.Close()
		t.Fatal("identity mismatch accepted")
	}
	if err := os.WriteFile(filepath.Join(cfg.StateDir, "state.json"), []byte("{broken"), 0600); err != nil {
		t.Fatal(err)
	}
	if other, err := NewRoom(fixedID(1), newWireRadio(), cfg); err == nil {
		other.Close()
		t.Fatal("corrupt state silently reset")
	}
}

func TestPersistenceFailureDoesNotAcknowledgeLogin(t *testing.T) {
	cfg := config(t)
	errs := make(chan error, 8)
	cfg.ErrorHandler = func(err error) { errs <- err }
	id := fixedID(1)
	r := newWireRadio()
	s, err := NewRoom(id, r, cfg)
	if err != nil {
		t.Fatal(err)
	}
	r.next(t)
	// Replacing the destination with a directory fails even when run as root.
	path := filepath.Join(cfg.StateDir, "state.json")
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(path, 0700); err != nil {
		t.Fatal(err)
	}
	r.inject(t, loginWire(t, true, fixedID(2), id, 1, 0, "room", 2, 0, nil), false)
	select {
	case <-errs:
	case <-time.After(time.Second):
		t.Fatal("persistence failure not reported")
	}
	r.quiet(t)
	s.mu.RLock()
	members := len(s.state.Members)
	s.mu.RUnlock()
	if members != 0 {
		t.Fatal("failed login remained authorized")
	}
	if err := s.Close(); err == nil {
		t.Fatal("Close hid persistence error")
	}
}

func TestMembershipLimitAndMalformedState(t *testing.T) {
	cfg := config(t)
	cfg.MaxMembers = 1
	errs := make(chan error, 8)
	cfg.ErrorHandler = func(err error) { errs <- err }
	s, r, id := startRole(t, true, cfg)
	loggedIn(t, s, r, fixedID(2), id, true, 1, "admin")
	r.inject(t, loginWire(t, true, fixedID(3), id, 1, 0, "room", 2, 0, nil), false)
	select {
	case err := <-errs:
		if !strings.Contains(err.Error(), "full") {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("full table not reported")
	}
	r.quiet(t)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(cfg.StateDir, "state.json"))
	if err != nil {
		t.Fatal(err)
	}
	var state diskState
	if err := json.Unmarshal(data, &state); err != nil {
		t.Fatal(err)
	}
	for _, m := range state.Members {
		m.PathLength = 0x81
		m.Path = []byte{1}
	}
	data, err = json.Marshal(state)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfg.StateDir, "state.json"), data, 0600); err != nil {
		t.Fatal(err)
	}
	if other, err := NewRoom(id, newWireRadio(), cfg); err == nil {
		other.Close()
		t.Fatal("invalid persisted path accepted")
	}
}
