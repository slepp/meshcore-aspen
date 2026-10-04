package main

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"errors"
	"math"
	"net"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/app"
	"meshcore.local/meshcore/internal/radio"
)

func fixtureID(seed byte) meshcore.LocalIdentity {
	var b [32]byte
	for i := range b {
		b[i] = seed
	}
	return meshcore.NewLocalIdentityFromSeed(b)
}

func isolatedConfig() app.Config {
	cfg := app.DefaultConfig()
	cfg.Radio.FreqHz, cfg.Radio.BwHz, cfg.TxPower = 912525000, 250000, 2
	return cfg
}

type muxProbeModem struct {
	handler       func([]byte, float32, int8, bool)
	registrations int
}

func (m *muxProbeModem) SendData([]byte) error           { return errors.New("unexpected test transmit") }
func (m *muxProbeModem) AddOutboundHandler(func([]byte)) {}
func (m *muxProbeModem) SetDataHandler(h func([]byte, float32, int8, bool)) {
	m.handler = h
	m.registrations++
}

func TestSecondaryMuxRawHandlerDoesNotReplaceLinkIngress(t *testing.T) {
	modem := &muxProbeModem{}
	mux := node.NewRadioMux(modem)
	defer mux.Stop()
	rf := radio.LocalRadio{MuxRadio: mux.NewRadio()}
	defer rf.Close()
	capture := newCapture()
	rf.SetRawDataHandler(capture.receive)
	if modem.registrations != 1 {
		t.Fatalf("raw observer overwrote modem/link ingress: %d registrations", modem.registrations)
	}
	modem.handler([]byte{0x15, 0x81, 0xaa, 0xbb, 0xcc, 1, 2, 3}, 6, -71, true)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	frame, err := capture.wait(ctx, "mux_receive", func(p *meshcore.Packet) bool {
		return p.PathHashSize() == 3 && p.PathHashCount() == 1 && bytes.Equal(p.Path, []byte{0xaa, 0xbb, 0xcc})
	})
	if err != nil || frame.snr != 6 || frame.rssi != -71 {
		t.Fatalf("mux delivery: %+v %v", frame, err)
	}
}

func TestTransmitProofRequiresMatchingCompletedSend(t *testing.T) {
	capture := newCapture()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	wire := []byte{0x15, 0, 1, 2, 3}
	if err := capture.waitTX(ctx, "queued_only", wire); !errors.Is(err, context.Canceled) {
		t.Fatalf("queued-only transmission counted as completed: %v", err)
	}
	capture.sent([]byte{0x15, 0, 9, 8, 7})
	capture.sent(wire)
	wire[2] = 99
	ctx, cancel = context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := capture.waitTX(ctx, "completed", []byte{0x15, 0, 1, 2, 3}); err != nil {
		t.Fatal(err)
	}
	if capture.transmitted.Load() != 2 {
		t.Fatalf("completed sends = %d", capture.transmitted.Load())
	}
}

func TestSentACKAcceptsFirmwareExtendedAndLegacyWireLayouts(t *testing.T) {
	for _, test := range []struct {
		name string
		wire []byte
	}{
		{"extended", []byte{6, 1, 0x78, 0x56, 0x34, 0x12, 0xe8, 3, 0, 0}},
		{"legacy", []byte{6, 0x78, 0x56, 0x34, 0x12}},
	} {
		t.Run(test.name, func(t *testing.T) {
			response, err := protocol.ParseResponse(test.wire)
			if err != nil {
				t.Fatal(err)
			}
			crc, err := sentACK(response.Data.(protocol.SentResponse))
			if err != nil || crc != 0x12345678 {
				t.Fatalf("ACK = %08x, %v", crc, err)
			}
		})
	}
	if _, err := sentACK(protocol.SentResponse{}); err == nil {
		t.Fatal("missing ACK proof accepted")
	}
	if crc, err := sentACK(protocol.SentResponse{HasExtended: true}); err != nil || crc != 0 {
		t.Fatalf("valid zero-valued CRC rejected: %08x %v", crc, err)
	}
}

func TestExplicitPHYAndDeadlineSafety(t *testing.T) {
	valid := options{peer: "192.0.2.2:8001", timeout: 150 * time.Second}
	for _, test := range []struct {
		name      string
		cfg       app.Config
		opts      options
		wantError bool
	}{
		{"isolated", isolatedConfig(), valid, false},
		{"active_defaults", app.DefaultConfig(), valid, true},
		{"explicit_override", app.DefaultConfig(), options{peer: valid.peer, timeout: valid.timeout, allowUnsafePHY: true}, false},
		{"too_long", isolatedConfig(), options{peer: valid.peer, timeout: 181 * time.Second}, true},
		{"too_short", isolatedConfig(), options{peer: valid.peer, timeout: time.Second}, true},
		{"missing_port", isolatedConfig(), options{peer: "192.0.2.2", timeout: valid.timeout}, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			err := validateOptions(test.cfg, test.opts)
			if (err != nil) != test.wantError {
				t.Fatalf("validate=%v wantError=%v", err, test.wantError)
			}
		})
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := distinctRadios(ctx, "192.0.2.1:8001", "192.0.2.1:8002"); err == nil {
		t.Fatal("same radio on different ports accepted")
	}
	if err := distinctRadios(ctx, "192.0.2.1:8001", "[::ffff:192.0.2.1]:8001"); err == nil {
		t.Fatal("IPv4-mapped alias accepted as independent radio")
	}
	if err := distinctRadios(ctx, "192.0.2.1:8001", "192.0.2.2:8001"); err != nil {
		t.Fatal(err)
	}
}

func TestRFProofRejectsSyntheticAndMissingMetadata(t *testing.T) {
	for _, test := range []struct {
		snr    float32
		rssi   int8
		signal bool
		want   bool
	}{
		{-32, 127, true, false}, {1, -75, false, false}, {float32(math.NaN()), -75, true, false},
		{float32(math.Inf(1)), -75, true, false}, {-3.25, -104, true, true},
		{-32, -104, true, true}, {5, 127, true, true},
	} {
		if got := physicalSignal(test.snr, test.rssi, test.signal); got != test.want {
			t.Fatalf("signal(%v,%v,%v)=%v", test.snr, test.rssi, test.signal, got)
		}
	}
	c := newCapture()
	raw := []byte{0x15, 0, 1, 2, 3}
	c.receive(raw, -32, 127, true)
	c.receive(raw, 5, -80, false)
	c.receive(raw, 4.25, -80, true)
	raw[2] = 99
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	frame, err := c.wait(ctx, "test", func(p *meshcore.Packet) bool { return bytes.Equal(p.Payload, []byte{1, 2, 3}) })
	if err != nil || frame.rssi != -80 || frame.snr != 4.25 || c.rejected.Load() != 2 || c.physical.Load() != 1 {
		t.Fatalf("capture: %+v %v rejected=%d physical=%d", frame, err, c.rejected.Load(), c.physical.Load())
	}
}

func TestRFProofRequiresExpectedPayloadAndRepeaterHop(t *testing.T) {
	original := &meshcore.Packet{Header: 0x15, PathLength: 0x80, Payload: []byte{1, 2, 3}}
	repeater, other := fixtureID(1), fixtureID(2)
	key := repeater.PublicKey()
	good := &meshcore.Packet{Header: 0x15, PathLength: 0x81, Path: bytes.Clone(key[:3]), Payload: []byte{1, 2, 3}}
	if !forwardedBy(good, original, repeater.Identity) {
		t.Fatal("genuine appended three-byte repeater hop rejected")
	}
	for _, bad := range []*meshcore.Packet{
		original,
		{Header: 0x15, PathLength: 0x81, Path: bytes.Clone(other.PublicKeyBytes()[:3]), Payload: []byte{1, 2, 3}},
		{Header: 0x15, PathLength: 0x81, Path: bytes.Clone(key[:3]), Payload: []byte{1, 2, 4}},
		{Header: 0x16, PathLength: 0x81, Path: bytes.Clone(key[:3]), Payload: []byte{1, 2, 3}},
	} {
		if forwardedBy(bad, original, repeater.Identity) {
			t.Fatalf("false forwarding proof accepted: %+v", bad)
		}
	}
}

func independentlyDecrypt(t *testing.T, key, encrypted []byte) []byte {
	t.Helper()
	if len(encrypted) < 18 || (len(encrypted)-2)%16 != 0 {
		t.Fatalf("invalid cipher length %d", len(encrypted))
	}
	mac := hmac.New(sha256.New, key)
	mac.Write(encrypted[2:])
	if !hmac.Equal(mac.Sum(nil)[:2], encrypted[:2]) {
		t.Fatal("wrong HMAC")
	}
	block, err := aes.NewCipher(key[:16])
	if err != nil {
		t.Fatal(err)
	}
	plain := bytes.Clone(encrypted[2:])
	for i := 0; i < len(plain); i += 16 {
		block.Decrypt(plain[i:i+16], plain[i:i+16])
	}
	return plain
}

func TestIndependentLoginGroupAndACKWireContracts(t *testing.T) {
	probe, room := fixtureID(1), fixtureID(2)
	secret, err := probe.SharedSecret(room.Identity)
	if err != nil {
		t.Fatal(err)
	}
	before := uint32(time.Now().Unix())
	login, err := roomLogin(probe, room.Identity, secret, 0x12345678, "hello")
	if err != nil {
		t.Fatal(err)
	}
	if login.Header != 0x1d || login.PathLength != 0 || login.Payload[0] != room.PublicKey()[0] ||
		!bytes.Equal(login.Payload[1:33], probe.PublicKeyBytes()) {
		t.Fatal("ANON_REQ addressing/header mismatch")
	}
	body := independentlyDecrypt(t, secret, login.Payload[33:])
	if !bytes.Equal(body[:4], []byte{0x78, 0x56, 0x34, 0x12}) ||
		binary.LittleEndian.Uint32(body[4:8]) < before ||
		binary.LittleEndian.Uint32(body[4:8]) > uint32(time.Now().Unix()) ||
		!bytes.Equal(body[8:14], []byte{'h', 'e', 'l', 'l', 'o', 0}) {
		t.Fatalf("room login format: %x", body)
	}
	var key [16]byte
	copy(key[:], []byte("test-only-key-123"))
	group, err := groupPacket(key, "test", 0x04030201)
	if err != nil {
		t.Fatal(err)
	}
	if group.Header != 0x15 || group.PathLength != 0x80 {
		t.Fatal("group must flood using three-byte hop hashes")
	}
	if plain := independentlyDecrypt(t, key[:], group.Payload[1:]); !bytes.Equal(plain[:19], []byte{1, 2, 3, 4, 0, 'r', 'f', '-', 'c', 'h', 'e', 'c', 'k', ':', ' ', 't', 'e', 's', 't'}) {
		t.Fatalf("group wire %x", plain)
	}
	plain := []byte{1, 2, 3, 4, 0, 'o', 'k'}
	hash := sha256.Sum256(append(bytes.Clone(plain), probe.PublicKeyBytes()...))
	crc := ackProof(plain, probe.Identity)
	var encoded [4]byte
	binary.LittleEndian.PutUint32(encoded[:], crc)
	if !bytes.Equal(encoded[:], hash[:4]) {
		t.Fatal("ACK hash not SHA256(timestamp/flags/text/public key)[:4]")
	}
	if !matchesACK(&meshcore.Packet{Header: 0x0e, Payload: encoded[:]}, crc) {
		t.Fatal("valid ACK rejected")
	}
	if matchesACK(&meshcore.Packet{Header: 0x06, Payload: encoded[:]}, crc) {
		t.Fatal("non-ACK payload accepted as ACK")
	}
}

func TestLoginResponseAuthenticatesRoomAndDecodesReturnedPath(t *testing.T) {
	probe, room, wrong := fixtureID(1), fixtureID(2), fixtureID(3)
	secret, err := probe.SharedSecret(room.Identity)
	if err != nil {
		t.Fatal(err)
	}
	// PathLen 0x41 means one two-byte hop; extra type 1 is RESPONSE.
	body := []byte{0x41, 0xaa, 0xbb, 1, 1, 0, 0, 0, 0, 0, 0, 2, 9, 8, 7, 6, 1}
	packet, err := datagram(room, probe.Identity, secret, 8, body, route{})
	if err != nil {
		t.Fatal(err)
	}
	path, ok := loginResponse(packet, probe.Identity, room.Identity, secret)
	if !ok || !path.known || path.length != 0x41 || !bytes.Equal(path.path, []byte{0xaa, 0xbb}) {
		t.Fatalf("login PATH: %+v %t", path, ok)
	}
	if _, ok := loginResponse(packet, probe.Identity, wrong.Identity, secret); ok {
		t.Fatal("wrong room source accepted")
	}
	packet.Payload[len(packet.Payload)-1] ^= 1
	if _, ok := loginResponse(packet, probe.Identity, room.Identity, secret); ok {
		t.Fatal("corrupted login ciphertext accepted")
	}
}

func TestPublicStatusRequiresIndependentOnlineRoles(t *testing.T) {
	base, room, repeater, observer := fixtureID(1), fixtureID(2), fixtureID(3), fixtureID(4)
	for _, failure := range []string{"", "offline", "observer_offline", "duplicate", "invalid"} {
		t.Run(failure, func(t *testing.T) {
			status := map[string]map[string]any{}
			for name, id := range map[string]meshcore.LocalIdentity{"companion": base, "room": room, "repeater": repeater, "observer": observer} {
				status[name] = map[string]any{"public_key": id.Identity.String(), "radio_connected": true}
			}
			switch failure {
			case "offline":
				status["room"]["radio_connected"] = false
			case "observer_offline":
				status["observer"]["radio_connected"] = false
			case "duplicate":
				status["room"]["public_key"] = base.Identity.String()
			case "invalid":
				status["room"]["public_key"] = "bad"
			}
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != "/status" {
					t.Errorf("unexpected endpoint %s", r.URL.Path)
				}
				if err := json.NewEncoder(w).Encode(status); err != nil {
					t.Error(err)
				}
			}))
			defer server.Close()
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			ids, err := loadIdentities(ctx, strings.TrimPrefix(server.URL, "http://"))
			if (err != nil) != (failure != "") {
				t.Fatalf("status=%+v err=%v failure=%s", ids, err, failure)
			}
		})
	}
}

func TestCleanupContinuesAndPropagatesFailure(t *testing.T) {
	var order []int
	want := errors.New("channel restoration failed")
	actions := []cleanup{
		func(context.Context) error { order = append(order, 1); return nil },
		func(context.Context) error { order = append(order, 2); return want },
		func(context.Context) error { order = append(order, 3); return nil },
	}
	if err := runCleanups(context.Background(), actions); !errors.Is(err, want) {
		t.Fatalf("cleanup swallowed failure: %v", err)
	}
	if !reflect.DeepEqual(order, []int{3, 2, 1}) {
		t.Fatalf("cleanup order/continuation: %v", order)
	}
}

func TestCaptureDeadlineAndOverflowAreFailures(t *testing.T) {
	c := newCapture()
	for range 257 {
		c.receive([]byte{0x15, 0, 1, 2, 3}, 3, -80, true)
	}
	if _, err := c.wait(context.Background(), "overflow", func(*meshcore.Packet) bool { return true }); err == nil || !strings.Contains(err.Error(), "overflow") {
		t.Fatalf("overflow=%v", err)
	}
	c = newCapture()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := c.wait(ctx, "missing_rf", func(*meshcore.Packet) bool { return true }); !errors.Is(err, context.Canceled) || !strings.Contains(err.Error(), "physical_rx=0") {
		t.Fatalf("deadline context: %v", err)
	}
}

func TestWildcardListenersDialLocally(t *testing.T) {
	for _, address := range []string{"0.0.0.0:5000", "[::]:5000", ":5000"} {
		if got := dialAddress(address); got != "127.0.0.1:5000" {
			t.Fatalf("dialAddress(%s)=%s", address, got)
		}
	}
	if got := dialAddress(net.JoinHostPort("192.0.2.1", "5000")); got != "192.0.2.1:5000" {
		t.Fatalf("explicit address changed: %s", got)
	}
}
