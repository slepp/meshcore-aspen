package observer

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func reflectorFixture(t *testing.T) (ReflectorConfig, string, []byte, []byte) {
	t.Helper()
	key := testKey()
	dir := t.TempDir()
	if err := os.Chmod(dir, 0700); err != nil {
		t.Fatal(err)
	}
	cfg := ReflectorConfig{
		StateDir: filepath.Join(dir, "queue"), SourceURL: "tcp://127.0.0.1:1883",
		UsernameEnv: "REFLECTOR_TEST_USER", PasswordEnv: "REFLECTOR_TEST_PASSWORD",
		NodeURL: "https://aspen.example", PasswordFile: filepath.Join(dir, "password"),
		Identity: strings.ToUpper(key.String()), IATA: "YYC", Origin: "Test observer",
		Destinations: []ReflectorDestination{
			{Name: "first", URLs: []string{"wss://first.example/mqtt", "wss://backup.example/mqtt"}},
			{Name: "second", URLs: []string{"wss://second.example/mqtt"}},
		},
	}
	o, err := New(key.String(), Config{Format: PublicFormat, IATA: cfg.IATA, Origin: cfg.Origin})
	if err != nil {
		t.Fatal(err)
	}
	packet, err := o.publicPacket(observation{
		data:      []byte{0x16, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 0x51, 0x52, 0x53},
		timestamp: time.Now().UTC(), hasSignal: true, snr: 4.5, rssi: -90,
	})
	if err != nil {
		t.Fatal(err)
	}
	return cfg, "meshcore/" + cfg.IATA + "/" + cfg.Identity, packet, []byte(o.publicStatus("online", time.Now()))
}

func TestReflectorSchemaPrivacyRFAndRawConsistency(t *testing.T) {
	cfg, base, packet, status := reflectorFixture(t)
	for suffix, raw := range map[string][]byte{"packets": packet, "status": status} {
		if _, retained, err := validateReflection(cfg, base+"/"+suffix, raw); err != nil || retained != (suffix == "status") {
			t.Fatalf("%s: retained=%v error=%v", suffix, retained, err)
		}
	}
	for field, bad := range map[string]any{
		"event_id": "internal", "sender_identity": cfg.Identity, "private_key": "secret",
		"direction": "tx", "origin_id": strings.Repeat("A", 64), "origin": "another node",
		"len": "99", "payload_len": "99", "packet_type": "1", "hash": strings.Repeat("A", 16),
		"time": "00:00:00", "date": "01/01/1970", "timestamp": "2026-01-01T01:00:00+01:00",
		"path": []string{"aabb"}, "SNR": "NaN", "RSSI": "-129", "raw": "15C001",
	} {
		object := decodeObject(t, packet)
		object[field] = bad
		raw, err := json.Marshal(object)
		if err != nil {
			t.Fatal(err)
		}
		if _, _, err := validateReflection(cfg, base+"/packets", raw); err == nil {
			t.Errorf("accepted %s=%v", field, bad)
		}
	}
	object := decodeObject(t, packet)
	object["SNR"], object["RSSI"] = "-32", "127"
	raw, _ := json.Marshal(object)
	if _, _, err := validateReflection(cfg, base+"/packets", raw); err == nil {
		t.Fatal("local modem reflection reached the public feed")
	}
	for _, topic := range []string{base + "/roles/bot", strings.Replace(base, "/YYC/", "/YEG/", 1) + "/packets"} {
		if _, _, err := validateReflection(cfg, topic, packet); err == nil {
			t.Fatalf("accepted internal/wrong topic %s", topic)
		}
	}
	if _, _, err := validateReflection(cfg, base+"/packets", append(bytes.Clone(packet), []byte(`{}`)...)); err == nil {
		t.Fatal("accepted extra JSON after the object")
	}
}

func TestReflectorDurableIndependentAcknowledgementsAndLatestStatus(t *testing.T) {
	cfg, base, packet, status := reflectorFixture(t)
	q, lock, err := openReflectionQueue(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := q.enqueue(base+"/packets", packet); err != nil {
		t.Fatal(err)
	}
	if err := q.enqueue(base+"/status", status); err != nil {
		t.Fatal(err)
	}
	name, record := q.next("first")
	if name != "status.json" {
		t.Fatal("status must precede pending RF packets")
	}
	if err := q.acknowledge(name, "first", record); err != nil {
		t.Fatal(err)
	}
	name, record = q.next("first")
	if name == "" || name == "status.json" {
		t.Fatal("pending RF packet unavailable")
	}
	if err := q.acknowledge(name, "first", record); err != nil {
		t.Fatal(err)
	}
	if err := lock.Close(); err != nil {
		t.Fatal(err)
	}
	q, lock, err = openReflectionQueue(cfg)
	if err != nil {
		t.Fatal("durable queue did not survive restart:", err)
	}
	defer lock.Close()
	if name, _ := q.next("first"); name != "" {
		t.Fatal("replayed a destination's acknowledged packet after restart")
	}
	if err := q.acknowledge("status.json", "second", q.items["status.json"]); err != nil {
		t.Fatal(err)
	}
	name, record = q.next("second")
	if name == "" || !slices.Equal(record.Pending, []string{"second"}) {
		t.Fatal("second destination's unacknowledged packet was lost")
	}
	if err := q.acknowledge(name, "second", record); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(cfg.StateDir, name)); !os.IsNotExist(err) {
		t.Fatal("fully acknowledged packet remains on disk")
	}
	old := q.items["status.json"]
	var newStatus reflectedStatus
	if err := json.Unmarshal(status, &newStatus); err != nil {
		t.Fatal(err)
	}
	newStatus.Status = "offline"
	status, _ = json.Marshal(newStatus)
	if err := q.enqueue(base+"/status", status); err != nil {
		t.Fatal(err)
	}
	if err := q.acknowledge("status.json", "second", old); err != nil {
		t.Fatal(err)
	}
	if !slices.Contains(q.items["status.json"].Pending, "second") {
		t.Fatal("late ACK of prior status erased the replacement's pending destination")
	}
	otherCfg := cfg
	otherCfg.Destinations = cfg.Destinations[:1]
	if _, other, err := openReflectionQueue(otherCfg); err == nil {
		other.Close()
		t.Fatal("second process obtained an already-owned queue")
	}
}

func TestReflectorRejectsCorruptQueueAndUnsafeConfig(t *testing.T) {
	cfg, base, packet, _ := reflectorFixture(t)
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*ReflectorConfig){
		func(c *ReflectorConfig) { c.NodeURL = "http://aspen.example" },
		func(c *ReflectorConfig) { c.SourceURL = "tcp://user:pass@127.0.0.1" },
		func(c *ReflectorConfig) { c.StateDir = "relative" },
		func(c *ReflectorConfig) { c.Destinations[0].URLs[0] = "ws://unprotected.example" },
		func(c *ReflectorConfig) { c.Destinations[0].URLs[0] = "wss://user:pass@first.example" },
	} {
		c := cfg
		c.Destinations = slices.Clone(cfg.Destinations)
		c.Destinations[0].URLs = slices.Clone(cfg.Destinations[0].URLs)
		mutate(&c)
		if c.Validate() == nil {
			t.Fatal("accepted unsafe reflector configuration")
		}
	}
	q, lock, err := openReflectionQueue(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := q.enqueue(base+"/packets", packet); err != nil {
		t.Fatal(err)
	}
	name, _ := q.next("first")
	lock.Close()
	if err := os.WriteFile(filepath.Join(cfg.StateDir, name), []byte(`{"topic":"internal"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, lock, err := openReflectionQueue(cfg); err == nil {
		lock.Close()
		t.Fatal("silently accepted or discarded corrupt durable queue")
	}
}

func TestReflectorMastTokenSignatureAudienceAndSessionCleanup(t *testing.T) {
	cfg, _, _, _ := reflectorFixture(t)
	key := testKey()
	if err := os.WriteFile(cfg.PasswordFile, []byte("test-password"), 0600); err != nil {
		t.Fatal(err)
	}
	var logouts atomic.Int32
	var audienceOverride atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		switch r.URL.Path {
		case "/admin/login":
			if string(body) != "test-password" {
				http.Error(w, "denied", http.StatusForbidden)
				return
			}
			fmt.Fprint(w, strings.Repeat("a", 32))
		case "/admin/observer-token":
			if r.Header.Get("X-Mast-Session") != strings.Repeat("a", 32) {
				t.Error("token request missing authenticated node session")
			}
			audience := string(body)
			if audienceOverride.Load() {
				audience = "wrong.example"
			}
			token, err := authToken(key.String(), audience, time.Now(), key.Sign)
			if err != nil {
				t.Error(err)
			}
			fmt.Fprint(w, token)
		case "/admin/logout":
			if r.Header.Get("X-Mast-Session") != strings.Repeat("a", 32) {
				t.Error("logout missing authenticated session")
			}
			logouts.Add(1)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	cfg.NodeURL, cfg.TrustedLAN = server.URL, true
	signer := &mastTokenSource{cfg: cfg, client: server.Client()}
	token, renew, err := signer.token(context.Background(), "broker.example", quietLogger())
	if err != nil || token == "" || renew.Before(time.Now().Add(23*time.Hour)) || logouts.Load() != 1 {
		t.Fatalf("signature/renewal/logout: %v %v %d", err, renew, logouts.Load())
	}
	audienceOverride.Store(true)
	signer.last = time.Time{}
	if _, _, err := signer.token(context.Background(), "broker.example", quietLogger()); err == nil || logouts.Load() != 2 {
		t.Fatal("accepted a wrong-audience token or leaked an admin session")
	}
	if err := os.Chmod(cfg.PasswordFile, 0644); err != nil {
		t.Fatal(err)
	}
	signer.last = time.Time{}
	if _, _, err := signer.token(context.Background(), "broker.example", quietLogger()); err == nil {
		t.Fatal("read world-accessible node password")
	}
}

func TestReflectorPresenceDoesNotInventOnlineAfterSourceLoss(t *testing.T) {
	cfg, _, _, status := reflectorFixture(t)
	var s reflectedStatus
	if err := json.Unmarshal(status, &s); err != nil {
		t.Fatal(err)
	}
	s.Time = time.Now().Add(-4 * time.Minute).UTC().Format(time.RFC3339Nano)
	raw, _ := json.Marshal(s)
	record := reflectionRecord{Payload: raw}
	if decodeObject(t, reflectorPresence(record, false))["status"] != "offline" {
		t.Fatal("stale source was advertised online")
	}
	s.Time = time.Now().UTC().Format(time.RFC3339Nano)
	raw, _ = json.Marshal(s)
	record.Payload = raw
	if decodeObject(t, reflectorPresence(record, false))["status"] != "online" ||
		decodeObject(t, reflectorPresence(record, true))["status"] != "offline" {
		t.Fatal("fresh status/will direction differs")
	}
	if cfg.Origin != s.Origin {
		t.Fatal("presence did not retain the radio origin")
	}
}

func TestReflectorQueuePacketCapacityAndPrivateDirectory(t *testing.T) {
	cfg, base, packet, status := reflectorFixture(t)
	q, lock, err := openReflectionQueue(cfg)
	if err != nil {
		t.Fatal(err)
	}
	for i := range MaxQueueSize {
		q.items[fmt.Sprintf("capacity-%d", i)] = reflectionRecord{}
	}
	if err := q.enqueue(base+"/packets", packet); err == nil || !strings.Contains(err.Error(), "4096") {
		t.Fatal("packet queue did not enforce its exact 4096-entry bound")
	}
	if err := q.enqueue(base+"/status", status); err != nil {
		t.Fatal("full packet queue blocked the separately bounded latest status:", err)
	}
	delete(q.items, "capacity-0")
	if err := q.enqueue(base+"/packets", packet); err != nil {
		t.Fatal("4095 pending packets did not leave one usable packet slot:", err)
	}
	if len(q.items) != MaxQueueSize+1 {
		t.Fatalf("packet+status shape: %d", len(q.items))
	}
	lock.Close()
	if err := os.Chmod(cfg.StateDir, 0755); err != nil {
		t.Fatal(err)
	}
	if _, lock, err := openReflectionQueue(cfg); err == nil {
		lock.Close()
		t.Fatal("opened a world-readable reception queue")
	}
}

func TestReflectorLocalSubscriptionPersistenceAndShutdown(t *testing.T) {
	cfg, base, packet, _ := reflectorFixture(t)
	broker, err := StartBroker(BrokerConfig{
		Address: "127.0.0.1:0", Username: "reader", Password: "password", Logger: quietLogger(),
	})
	if err != nil {
		t.Fatal(err)
	}
	defer broker.Close()
	cfg.SourceURL = "tcp://" + broker.Addr().String()
	t.Setenv(cfg.UsernameEnv, "reader")
	t.Setenv(cfg.PasswordEnv, "password")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- RunReflector(ctx, cfg, quietLogger()) }()
	publisher := connectTestClient(t, broker.Addr().String(), "reader", "password")
	deadline := time.Now().Add(4 * time.Second)
	for {
		tok := publisher.Publish(base+"/packets", 1, false, packet)
		if !tok.WaitTimeout(time.Second) || tok.Error() != nil {
			t.Fatal("source publication failed")
		}
		files, err := filepath.Glob(filepath.Join(cfg.StateDir, "*.json"))
		if err != nil {
			t.Fatal(err)
		}
		if len(files) == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("selected public local feed was not saved")
		}
		time.Sleep(10 * time.Millisecond)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(4 * time.Second):
		t.Fatal("reflector did not stop its source and destination workers")
	}
	// Without an observer status no destination connects or invents presence.
	q, lock, err := openReflectionQueue(cfg)
	if err != nil {
		t.Fatal("service shutdown did not release its durable queue:", err)
	}
	defer lock.Close()
	if len(q.items) != 1 {
		t.Fatal("unacknowledged reception lost during service shutdown")
	}
	_, record := q.next("first")
	if !slices.Equal(record.Pending, []string{"first", "second"}) {
		t.Fatal("offline destinations did not retain their independent deliveries")
	}
}
