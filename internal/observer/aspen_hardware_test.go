package observer

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/mochi-mqtt/server/v2/packets"
)

const aspenValidatorAudience = "aspen-observer-validation.internal"

type hardwareEvent struct {
	Kind    string          `json:"kind"`
	Time    time.Time       `json:"time"`
	Topic   string          `json:"topic,omitempty"`
	Payload json.RawMessage `json:"payload,omitempty"`
}

type aspenReceiver struct {
	mqtt.HookBase
	mu     sync.Mutex
	tokens map[string]string
	events chan hardwareEvent
	public string
}

func (*aspenReceiver) ID() string { return "private-aspen-validation" }
func (*aspenReceiver) Provides(event byte) bool {
	return event == mqtt.OnConnectAuthenticate || event == mqtt.OnACLCheck || event == mqtt.OnPublished
}
func (h *aspenReceiver) OnConnectAuthenticate(cl *mqtt.Client, pk packets.Packet) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.tokens, cl.ID)
	token := string(pk.Connect.Password)
	key, err := verifyAuthToken(string(pk.Connect.Username), token, aspenValidatorAudience, time.Now())
	if err != nil || key != h.public {
		h.events <- hardwareEvent{Kind: "auth-rejected", Time: time.Now()}
		return false
	}
	payload, _ := base64.RawURLEncoding.DecodeString(strings.Split(token, ".")[1])
	h.tokens[cl.ID] = token
	h.events <- hardwareEvent{Kind: "identity-authenticated", Time: time.Now(), Payload: payload}
	return true
}
func (h *aspenReceiver) OnACLCheck(cl *mqtt.Client, topic string, write bool) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	key, err := verifyAuthToken(string(cl.Properties.Username), h.tokens[cl.ID], aspenValidatorAudience, time.Now())
	prefix := "meshcore/YEG/" + key
	return err == nil && key == h.public && write &&
		(topic == prefix+"/packets" || topic == prefix+"/status")
}
func (h *aspenReceiver) OnPublished(_ *mqtt.Client, pk packets.Packet) {
	h.events <- hardwareEvent{Kind: "publish", Time: time.Now(), Topic: pk.TopicName,
		Payload: append(json.RawMessage(nil), pk.Payload...)}
}

// This opt-in server uses only a private, kernel-assigned port and never forwards.
func TestAspenPrivateHardwareReceiver(t *testing.T) {
	if os.Getenv("MESHCORE_ASPEN_PRIVATE_VALIDATOR") != "1" {
		t.Skip("physical Aspen validation is disabled")
	}
	host := privateObserverTestHost(t)
	directory := os.Getenv("MESHCORE_ASPEN_VALIDATOR_DIRECTORY")
	if directory == "" || !filepath.IsAbs(directory) {
		t.Fatal("provide an owned absolute validator artifact directory")
	}
	public := strings.ToUpper(os.Getenv("MESHCORE_ASPEN_OBSERVER_PUBLIC_KEY"))
	decoded, err := hex.DecodeString(public)
	if err != nil || len(decoded) != 32 {
		t.Fatal("provide Aspen's existing observer public key from its read-only dashboard")
	}
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	certificate := func(serial int64) (tls.Certificate, []byte) {
		key, err := rsa.GenerateKey(rand.Reader, 2048)
		if err != nil {
			t.Fatal(err)
		}
		template := &x509.Certificate{SerialNumber: big.NewInt(serial),
			Subject:   pkix.Name{CommonName: host},
			NotBefore: time.Now().Add(-time.Minute), NotAfter: time.Now().Add(2 * time.Hour),
			IPAddresses: []net.IP{net.ParseIP(host)},
			DNSNames:    []string{aspenValidatorAudience},
			KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
			ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
			IsCA:        true, BasicConstraintsValid: true}
		der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
		if err != nil {
			t.Fatal(err)
		}
		return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key},
			pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	}
	trusted, ca := certificate(1)
	_, wrongCA := certificate(2)
	for name, data := range map[string][]byte{"ca.crt": ca, "untrusted-ca.crt": wrongCA} {
		if err := os.WriteFile(filepath.Join(directory, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	reservation, err := net.Listen("tcp", net.JoinHostPort(host, "0"))
	if err != nil {
		t.Fatal(err)
	}
	address := reservation.Addr().String()
	_ = reservation.Close()
	hook := &aspenReceiver{tokens: map[string]string{}, events: make(chan hardwareEvent, 256), public: public}
	server := mqtt.New(&mqtt.Options{Logger: quietLogger()})
	if err := server.AddHook(hook, nil); err != nil {
		t.Fatal(err)
	}
	if err := server.AddListener(listeners.NewWebsocket(listeners.Config{
		ID: "private-aspen-wss", Address: address,
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{trusted}},
	})); err != nil {
		t.Fatal(err)
	}
	if err := server.Serve(); err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	roots := x509.NewCertPool()
	roots.AppendCertsFromPEM(ca)
	for attempt := 0; ; attempt++ {
		conn, err := tls.DialWithDialer(&net.Dialer{Timeout: time.Second}, "tcp", address,
			&tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12})
		if err == nil {
			_ = conn.Close()
			break
		}
		if attempt == 100 {
			t.Fatal("private WSS listener did not become responsive")
		}
		time.Sleep(20 * time.Millisecond)
	}
	metadata, _ := json.Marshal(map[string]string{
		"host": host, "uri": "wss://" + address + "/mqtt",
		"audience": aspenValidatorAudience, "observer_key": public,
	})
	if err := os.WriteFile(filepath.Join(directory, "receiver.json"), metadata, 0600); err != nil {
		t.Fatal(err)
	}
	log, err := os.OpenFile(filepath.Join(directory, "events.jsonl"), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	defer log.Close()
	t.Log("private WSS validator responsive; server key remains in memory")
	encoder := json.NewEncoder(log)
	deadline := time.After(35 * time.Minute)
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case event := <-hook.events:
			if !json.Valid(event.Payload) && len(event.Payload) != 0 {
				t.Error("device published invalid JSON")
			}
			if strings.HasSuffix(event.Topic, "/packets") {
				checkPublicPacket(t, event.Payload)
			}
			if err := encoder.Encode(event); err != nil {
				t.Fatal(err)
			}
		case <-tick.C:
			if _, err := os.Stat(filepath.Join(directory, "stop")); err == nil {
				t.Log("private validator stopped deliberately")
				return
			}
		case <-deadline:
			t.Fatal(fmt.Sprintf("private validator exceeded its bounded lifetime: %s", address))
		}
	}
}
