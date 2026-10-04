package observer

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"
	meshcore "github.com/meshcore-go/meshcore-go"
	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/mochi-mqtt/server/v2/packets"
)

// This receiver is deliberately test-only and binds a kernel-assigned loopback
// port. The persistent broker's password authentication is not identity auth.
type identityReceiver struct {
	mqtt.HookBase
	mu     sync.Mutex
	tokens map[string]string
	now    func() time.Time
}

func (*identityReceiver) ID() string { return "scoped-observer-identity-test" }
func (*identityReceiver) Provides(event byte) bool {
	return event == mqtt.OnConnectAuthenticate || event == mqtt.OnACLCheck
}
func (h *identityReceiver) OnConnectAuthenticate(cl *mqtt.Client, pk packets.Packet) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.tokens, cl.ID)
	if string(pk.Connect.Username) == "test-reader" && string(pk.Connect.Password) == "test-password" {
		return true
	}
	token := string(pk.Connect.Password)
	if _, err := verifyAuthToken(string(pk.Connect.Username), token, "internal-observer-test", h.now()); err != nil {
		return false
	}
	h.tokens[cl.ID] = token
	return true
}
func (h *identityReceiver) OnACLCheck(cl *mqtt.Client, topic string, write bool) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	token, exists := h.tokens[cl.ID]
	if !exists {
		return string(cl.Properties.Username) == "test-reader"
	}
	key, err := verifyAuthToken(string(cl.Properties.Username), token, "internal-observer-test", h.now())
	if err != nil || !write {
		return false
	}
	parts := strings.Split(topic, "/")
	return len(parts) >= 4 && parts[0] == "meshcore" && parts[1] == "YYC" && parts[2] == key &&
		(parts[3] == "packets" || parts[3] == "status")
}
func startIdentityReceiver(t *testing.T, now func() time.Time) (*mqtt.Server, string) {
	t.Helper()
	server := mqtt.New(&mqtt.Options{Logger: quietLogger()})
	if err := server.AddHook(&identityReceiver{tokens: map[string]string{}, now: now}, nil); err != nil {
		t.Fatal(err)
	}
	listener := listeners.NewTCP(listeners.Config{ID: "identity-test", Address: "127.0.0.1:0"})
	if err := server.AddListener(listener); err != nil {
		t.Fatal(err)
	}
	if err := server.Serve(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Close() })
	return server, listener.Address()
}
func testKey() meshcore.LocalIdentity { return meshcore.NewLocalIdentityFromSeed([32]byte{1}) }
func decodeObject(t *testing.T, payload []byte) map[string]any {
	t.Helper()
	var object map[string]any
	if err := json.Unmarshal(payload, &object); err != nil {
		t.Fatal(err)
	}
	return object
}
func checkPublicPacket(t *testing.T, payload []byte) {
	t.Helper()
	object := decodeObject(t, payload)
	for _, field := range []string{"origin", "origin_id", "timestamp", "type", "direction", "time", "date", "len", "packet_type", "route", "payload_len", "raw", "SNR", "RSSI", "hash"} {
		if _, ok := object[field].(string); !ok {
			t.Fatalf("%s must be a string: %s", field, payload)
		}
	}
	if object["direction"] != "rx" || object["type"] != "PACKET" {
		t.Fatalf("not RF PACKET: %s", payload)
	}
	key := object["origin_id"].(string)
	if key != strings.ToUpper(key) || len(key) != 64 {
		t.Fatal("invalid origin identity")
	}
	raw, err := hex.DecodeString(object["raw"].(string))
	if err != nil {
		t.Fatal(err)
	}
	packet, err := meshcore.PacketFromBytes(raw)
	if err != nil {
		t.Fatal(err)
	}
	// Independent SHA-256 contract, including TRACE's uint16 packed length.
	hash := sha256.New()
	hash.Write([]byte{packet.PayloadType()})
	if packet.PayloadType() == 9 {
		hash.Write([]byte{packet.PathLength, 0})
	}
	hash.Write(packet.Payload)
	want := strings.ToUpper(hex.EncodeToString(hash.Sum(nil)[:8]))
	if object["hash"] != want {
		t.Fatalf("packet-content hash: got %v want %s", object["hash"], want)
	}
	if object["len"] != fmt.Sprint(len(raw)) || object["payload_len"] != fmt.Sprint(len(packet.Payload)) {
		t.Fatal("wire/payload lengths disagree")
	}
	utc, err := time.Parse(time.RFC3339Nano, object["timestamp"].(string))
	if err != nil || object["time"] != utc.UTC().Format("15:04:05") || object["date"] != utc.UTC().Format("02/01/2006") {
		t.Fatal("UTC fields disagree")
	}
}
func TestPublicWireRFAndPaths(t *testing.T) {
	id := testKey()
	o, err := New(id.String(), Config{Format: PublicFormat, IATA: "YYC", Origin: `Observer "test"`})
	if err != nil {
		t.Fatal(err)
	}
	for _, raw := range [][]byte{
		{0x15, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 0x51, 0x52, 0x53},
		{0x17, 1, 2, 3, 4, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 0x51, 0x52, 0x53},
		{0x25, 0x81, 0xaa, 0xbb, 0xcc, 1, 2},
	} {
		item := observation{data: raw, timestamp: time.Now(), hasSignal: true, snr: 4.5, rssi: -90}
		payload, err := o.publicPacket(item)
		if err != nil {
			t.Fatal(err)
		}
		checkPublicPacket(t, payload)
		item.snr, item.rssi = -32, 127
		if _, err := o.publicPacket(item); err == nil {
			t.Fatal("reflection counted as RF")
		}
		item.hasSignal = false
		if _, err := o.publicPacket(item); err == nil {
			t.Fatal("invented signal data")
		}
		item.hasSignal, item.snr, item.rssi = true, 4.5, -90
		item.timestamp = time.Unix(0, 0)
		if _, err := o.publicPacket(item); err == nil {
			t.Fatal("published before UTC readiness")
		}
	}
	var filter uint16
	o.cfg.PacketFilter = &filter
	if _, err := o.publicPacket(observation{data: []byte{0x15, 0, 1}, timestamp: time.Now(), hasSignal: true}); err == nil {
		t.Fatal("filter ignored")
	}
	o.cfg.PacketFilter = nil
	o.cfg.Format = CaptureFormat
	item := observation{data: []byte{0x17, 1, 2, 3, 4, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 1}, timestamp: time.Now(), hasSignal: true, snr: 4.25, rssi: -90}
	payload, err := o.publicPacket(item)
	if err != nil {
		t.Fatal(err)
	}
	capture := decodeObject(t, payload)
	if capture["route"] != "T" || capture["SNR"] != "4.25" || capture["path"] != nil {
		t.Fatal("capture transport dialect disagrees with upstream")
	}
	item.data = []byte{0x16, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 1}
	payload, err = o.publicPacket(item)
	if err != nil {
		t.Fatal(err)
	}
	if decodeObject(t, payload)["path"] != "aabbcc,123456" {
		t.Fatal("capture direct path is not the upstream comma string")
	}
}
func TestIdentityAuthRejection(t *testing.T) {
	id := testKey()
	now := time.Now().Truncate(time.Second)
	token, err := authToken(id.String(), "internal-observer-test", now, id.Sign)
	if err != nil {
		t.Fatal(err)
	}
	username := "v1_" + strings.ToUpper(id.String())
	parts := strings.Split(token, ".")
	badToken := parts[0] + "." + parts[1] + "." + strings.Repeat("0", 128)
	if _, err := verifyAuthToken(username, token, "internal-observer-test", now); err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		user, token, audience string
		now                   time.Time
	}{
		{username, token, "wrong-audience", now},
		{"v1_" + strings.Repeat("0", 64), token, "internal-observer-test", now},
		{username, token, "internal-observer-test", now.Add(tokenLifetime)},
		{username, token, "internal-observer-test", now.Add(-time.Second)},
		{username, badToken, "internal-observer-test", now},
		{username, strings.Replace(token, "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0", base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"none","typ":"JWT"}`)), 1), "internal-observer-test", now},
	} {
		if _, err := verifyAuthToken(test.user, test.token, test.audience, test.now); err == nil {
			t.Fatal("accepted invalid identity token")
		}
	}
	if _, err := authToken(id.String(), "internal-observer-test", now, func(m []byte) []byte { _, private, _ := ed25519.GenerateKey(nil); return ed25519.Sign(private, m) }); err == nil {
		t.Fatal("wrong signer accepted")
	}
	_, address := startIdentityReceiver(t, func() time.Time { return now })
	for _, password := range []string{token, badToken} {
		client := paho.NewClient(paho.NewClientOptions().AddBroker("tcp://" + address).SetClientID(fmt.Sprint(clientSequence.Add(1))).SetUsername(username).SetPassword(password).SetConnectTimeout(time.Second))
		result := client.Connect()
		if !result.WaitTimeout(3 * time.Second) {
			t.Fatal("CONNECT did not finish")
		}
		if (password == token) != (result.Error() == nil) {
			t.Fatal("receiver CONNECT identity validation incorrect")
		}
		client.Disconnect(0)
	}
}
func TestPublicIdentityHandshakeRenewalAndWill(t *testing.T) {
	id := testKey()
	var seconds atomic.Int64
	seconds.Store(time.Now().Unix())
	now := func() time.Time { return time.Unix(seconds.Load(), 0) }
	server, address := startIdentityReceiver(t, now)
	reader := connectTestClient(t, address, "test-reader", "test-password")
	topic := "meshcore/YYC/" + strings.ToUpper(id.String())
	statuses := subscribe(t, reader, topic+"/status")
	packets := subscribe(t, reader, topic+"/packets")
	o, err := New(id.String(), Config{URL: "tcp://" + address, Format: PublicFormat, IATA: "YYC", Audience: "internal-observer-test", Sign: id.Sign, Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	o.now = now
	if err := o.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = o.Close() })
	receiveStatus := func(want string) {
		t.Helper()
		select {
		case message := <-statuses:
			object := decodeObject(t, message.payload)
			if object["status"] != want || object["origin_id"] != strings.ToUpper(id.String()) {
				t.Fatalf("status handshake: %s", message.payload)
			}
		case <-time.After(8 * time.Second):
			t.Fatal("status handshake timed out")
		}
	}
	receiveStatus("online")
	o.Observe([]byte{0x15, 0, 1, 2, 3}, 4.5, -90, true)
	select {
	case message := <-packets:
		checkPublicPacket(t, message.payload)
	case <-time.After(3 * time.Second):
		t.Fatal("authenticated packet missing")
	}
	seconds.Add(int64((tokenLifetime - renewalMargin) / time.Second))
	// Maintenance must renew without admitting reflection as RF traffic.
	o.Observe([]byte{0x15, 0, 1}, -32, 127, true)
	if o.Stats().Observed != 1 {
		t.Fatal("reflection was counted as RF reception")
	}
	receiveStatus("offline")
	receiveStatus("online") // fresh CONNECT + JWT, before the previous exp
	validTime := seconds.Load()
	seconds.Store(0)
	o.Observe([]byte{0x15, 0, 1}, -32, 127, true)
	receiveStatus("offline")
	if o.Connected() {
		t.Fatal("identity session survived an unready backwards clock")
	}
	seconds.Store(validTime)
	receiveStatus("online")
	cl, exists := server.Clients.Get(o.cfg.ClientID)
	if !exists {
		t.Fatal("renewed identity connection missing")
	}
	cl.Net.Conn.Close() // ungraceful loss: broker publishes JSON LWT
	receiveStatus("offline")
}
func TestPublicGracefulShutdownPreservesOfflineTimestamp(t *testing.T) {
	id := testKey()
	var seconds atomic.Int64
	seconds.Store(time.Now().Unix())
	now := func() time.Time { return time.Unix(seconds.Load(), 0) }
	_, address := startIdentityReceiver(t, now)
	reader := connectTestClient(t, address, "test-reader", "test-password")
	topic := "meshcore/YYC/" + strings.ToUpper(id.String()) + "/status"
	statuses := subscribe(t, reader, topic)
	o, err := New(id.String(), Config{URL: "tcp://" + address, Format: PublicFormat,
		IATA: "YYC", Audience: "internal-observer-test", Sign: id.Sign, Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	o.now = now
	if err := o.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = o.Close() })
	online := decodeObject(t, receive(t, statuses).payload)
	seconds.Add(10)
	if err := o.Close(); err != nil {
		t.Fatal(err)
	}
	offline := decodeObject(t, receive(t, statuses).payload)
	if offline["status"] != "offline" || offline["timestamp"] == online["timestamp"] {
		t.Fatal("shutdown did not publish its current offline timestamp")
	}
	select {
	case <-statuses:
		t.Fatal("graceful shutdown also published an older LWT")
	case <-time.After(100 * time.Millisecond):
	}
	retainedReader := connectTestClient(t, address, "test-reader", "test-password")
	retained := receive(t, subscribe(t, retainedReader, topic))
	if !retained.retained || decodeObject(t, retained.payload)["timestamp"] != offline["timestamp"] {
		t.Fatal("retained offline timestamp was replaced by an older LWT")
	}
}

func privateObserverTestHost(t *testing.T) string {
	t.Helper()
	host := os.Getenv("MESHCORE_OBSERVER_TEST_HOST")
	ip := net.ParseIP(host)
	if ip == nil || !(ip.IsPrivate() || ip.IsLoopback()) {
		t.Fatal("MESHCORE_OBSERVER_TEST_HOST must select a private or loopback IP address")
	}
	return host
}

func TestApprovedInternalBroker(t *testing.T) {
	if os.Getenv("MESHCORE_OBSERVER_INTERNAL_TEST") != "1" {
		t.Skip("explicit internal-broker test is disabled")
	}
	address := net.JoinHostPort(privateObserverTestHost(t), os.Getenv("MESHCORE_OBSERVER_TEST_PORT"))
	port, err := strconv.Atoi(os.Getenv("MESHCORE_OBSERVER_TEST_PORT"))
	if err != nil || port < 1 || port > 65535 {
		t.Fatal("MESHCORE_OBSERVER_TEST_PORT must select the broker TCP port")
	}
	username, password := os.Getenv("MESHCORE_OBSERVER_TEST_USERNAME"), os.Getenv("MESHCORE_OBSERVER_TEST_PASSWORD")
	if username == "" || password == "" {
		t.Fatal("protected internal broker credentials are required")
	}
	id, err := meshcore.GenerateLocalIdentity(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	reader := paho.NewClient(paho.NewClientOptions().AddBroker("tcp://" + address).
		SetClientID("internal-observer-check-" + id.String()[:16]).
		SetUsername(username).SetPassword(password).SetConnectTimeout(time.Second))
	connected := reader.Connect()
	if !connected.WaitTimeout(3*time.Second) || connected.Error() != nil {
		t.Fatal("internal test reader connection failed")
	}
	t.Cleanup(func() { reader.Disconnect(100) })
	topic := "meshcore/YYC/" + strings.ToUpper(id.String())
	packets := subscribe(t, reader, topic+"/packets")
	statuses := subscribe(t, reader, topic+"/status")
	o, err := New(id.String(), Config{URL: "tcp://" + address, Username: username, Password: password,
		Format: PublicFormat, IATA: "YYC", Origin: "Internal observer wire test", Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	if err := o.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = o.Close() })
	select {
	case message := <-statuses:
		if decodeObject(t, message.payload)["status"] != "online" {
			t.Fatal("internal status handshake missing")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("internal broker status timed out")
	}
	o.Observe([]byte{0x17, 1, 2, 3, 4, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 0x51, 0x52, 0x53}, 4.5, -90, true)
	select {
	case message := <-packets:
		checkPublicPacket(t, message.payload)
		t.Log("selected broker received observer-v1 packet with two 3-byte path hops and the expected payload hash")
	case <-time.After(5 * time.Second):
		t.Fatal("internal broker packet timed out")
	}
	_ = o.Close()
	// Remove this synthetic identity's retained test status, not any fleet topic.
	clear := reader.Publish(topic+"/status", 1, true, []byte{})
	if !clear.WaitTimeout(3*time.Second) || clear.Error() != nil {
		t.Fatal("synthetic retained status cleanup failed")
	}
}
func TestNativeObserverFixture(t *testing.T) {
	directory := os.Getenv("MESHCORE_OBSERVER_FIXTURES")
	if directory == "" {
		t.Skip("set MESHCORE_OBSERVER_FIXTURES to native wire fixture directory")
	}
	verifyNativeObserverFixture(t, directory)
}
func TestPublicWSSCertificateAndIdentity(t *testing.T) {
	id := testKey()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(1),
		Subject:   pkix.Name{CommonName: "internal observer test"},
		NotBefore: time.Now().Add(-time.Minute), NotAfter: time.Now().Add(time.Hour),
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1")},
		KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IsCA:        true, BasicConstraintsValid: true}
	der, err := x509.CreateCertificate(rand.Reader, template, template, public, private)
	if err != nil {
		t.Fatal(err)
	}
	certificate, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	roots := x509.NewCertPool()
	roots.AddCert(certificate)
	reservation, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := reservation.Addr().String()
	_ = reservation.Close()
	server := mqtt.New(&mqtt.Options{Logger: quietLogger()})
	if err := server.AddHook(&identityReceiver{tokens: map[string]string{}, now: time.Now}, nil); err != nil {
		t.Fatal(err)
	}
	listener := listeners.NewWebsocket(listeners.Config{ID: "internal-wss-test", Address: address,
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: private}}}})
	if err := server.AddListener(listener); err != nil {
		t.Fatal(err)
	}
	if err := server.Serve(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Close() })
	for attempts := 0; attempts < 100; attempts++ {
		conn, err := net.DialTimeout("tcp", address, 50*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			break
		}
		if attempts == 99 {
			t.Fatal("loopback WSS receiver did not start")
		}
		time.Sleep(10 * time.Millisecond)
	}
	uri := "wss://" + address + "/mqtt"
	untrusted := paho.NewClient(paho.NewClientOptions().AddBroker(uri).SetClientID("untrusted-certificate").
		SetUsername("test-reader").SetPassword("test-password").SetConnectTimeout(time.Second).
		SetTLSConfig(&tls.Config{MinVersion: tls.VersionTLS12, RootCAs: x509.NewCertPool()}))
	result := untrusted.Connect()
	if !result.WaitTimeout(3*time.Second) || result.Error() == nil {
		t.Fatal("untrusted WSS certificate accepted")
	}
	untrusted.Disconnect(0)
	cfg := Config{URL: uri, Format: PublicFormat, IATA: "YYC", Audience: "internal-observer-test", Sign: id.Sign,
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12, RootCAs: roots}, Logger: quietLogger()}
	o, err := New(id.String(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := o.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = o.Close() })
	deadline := time.Now().Add(4 * time.Second)
	for !o.Connected() && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if !o.Connected() {
		t.Fatal("trusted WSS observer identity/status handshake failed")
	}
	cfg.TLSConfig = &tls.Config{InsecureSkipVerify: true}
	if _, err := New(id.String(), cfg); err == nil {
		t.Fatal("insecure public TLS config accepted")
	}
}
func verifyNativeObserverFixture(t *testing.T, directory string) {
	t.Helper()
	var identity struct {
		Key   string `json:"public_key"`
		Token string `json:"token"`
	}
	raw, err := os.ReadFile(filepath.Join(directory, "token.json"))
	if err != nil || json.Unmarshal(raw, &identity) != nil {
		t.Fatal("native identity fixture missing")
	}
	now := time.Now()
	if _, err := verifyAuthToken("v1_"+identity.Key, identity.Token, "internal-observer-test", now); err != nil {
		t.Fatal(err)
	}
	_, address := startIdentityReceiver(t, time.Now)
	client := connectTestClient(t, address, "v1_"+identity.Key, identity.Token)
	reader := connectTestClient(t, address, "test-reader", "test-password")
	topic := "meshcore/YYC/" + identity.Key
	messages := subscribe(t, reader, topic+"/packets")
	for _, name := range []string{"packet.json", "trace.json", "capture.json"} {
		payload, err := os.ReadFile(filepath.Join(directory, name))
		if err != nil {
			t.Fatal(err)
		}
		checkPublicPacket(t, payload)
		result := client.Publish(topic+"/packets", 1, false, payload)
		if !result.WaitTimeout(3*time.Second) || result.Error() != nil {
			t.Fatal("native authenticated publication failed")
		}
		select {
		case message := <-messages:
			checkPublicPacket(t, message.payload)
		case <-time.After(3 * time.Second):
			t.Fatal("native wire receiver missed packet")
		}
	}
}
