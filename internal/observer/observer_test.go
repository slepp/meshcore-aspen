package observer

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

const testIdentity = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

var clientSequence atomic.Uint64

func quietLogger() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

func startTestBroker(t *testing.T, address string) *Broker {
	t.Helper()
	b, err := StartBroker(BrokerConfig{Address: address, Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = b.Close() })
	return b
}

func connectTestClient(t *testing.T, address, username, password string) mqtt.Client {
	t.Helper()
	c := mqtt.NewClient(mqtt.NewClientOptions().AddBroker("tcp://" + address).
		SetClientID(fmt.Sprintf("observer-test-%d", clientSequence.Add(1))).
		SetUsername(username).SetPassword(password).
		SetAutoReconnect(false).SetConnectTimeout(time.Second))
	token := c.Connect()
	if !token.WaitTimeout(3*time.Second) || token.Error() != nil {
		t.Fatalf("subscriber connect: %v", token.Error())
	}
	t.Cleanup(func() { c.Disconnect(100) })
	return c
}

type message struct {
	topic    string
	payload  []byte
	retained bool
}

func subscribe(t *testing.T, client mqtt.Client, topic string) <-chan message {
	t.Helper()
	messages := make(chan message, 32)
	token := client.Subscribe(topic, 1, func(_ mqtt.Client, m mqtt.Message) {
		messages <- message{m.Topic(), append([]byte(nil), m.Payload()...), m.Retained()}
	})
	if !token.WaitTimeout(3*time.Second) || token.Error() != nil {
		t.Fatalf("subscribe: %v", token.Error())
	}
	return messages
}

func receive(t *testing.T, messages <-chan message) message {
	t.Helper()
	select {
	case m := <-messages:
		return m
	case <-time.After(8 * time.Second):
		t.Fatal("timed out waiting for MQTT publication")
		return message{}
	}
}

func newTestObserver(t *testing.T, address string, size int) *Observer {
	t.Helper()
	o, err := New(testIdentity, Config{URL: "tcp://" + address, QueueSize: size, Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	if err := o.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = o.Close() })
	return o
}

func TestPublishesRFLocalAndMalformedFrames(t *testing.T) {
	broker := startTestBroker(t, "127.0.0.1:0")
	client := connectTestClient(t, broker.Addr().String(), "", "")
	topic := "meshcore/" + testIdentity
	messages := subscribe(t, client, topic+"/#")
	o := newTestObserver(t, broker.Addr().String(), 8)
	if status := receive(t, messages); status.topic != topic+"/status" || string(status.payload) != "online" {
		t.Fatalf("unexpected startup status: %+v", status)
	}
	before := time.Now().UTC()
	frame := []byte{0x15, 0x42, 0xaa, 0xbb, 0xcc, 0xdd, 0x77, 0x88}
	o.Observe(frame, 6.25, -97, true)
	frame[2] = 0 // Observe must retain its own copy.
	o.Observe([]byte{0x0e, 0, 1, 2, 3, 4}, -32, 127, true)
	o.Observe([]byte{0x15, 3, 0xaa}, 0, 0, false)
	o.Observe([]byte{0x41, 0}, float32(math.NaN()), -90, true)
	var events []Event
	for range 4 {
		m := receive(t, messages)
		if m.topic != topic+"/packets" || m.retained {
			t.Fatalf("unexpected packet topic/retain: %+v", m)
		}
		var event Event
		if err := json.Unmarshal(m.payload, &event); err != nil {
			t.Fatal(err)
		}
		if event.ObserverIdentity != testIdentity || event.Timestamp.Before(before) || event.Timestamp.After(time.Now()) {
			t.Fatalf("invalid observation metadata: %+v", event)
		}
		if event.EventID == "" {
			t.Fatal("missing event ID")
		}
		events = append(events, event)
	}
	rf := events[0]
	if rf.RawPacketHex != "1542aabbccdd7788" || rf.LocalLoopback || rf.SNR == nil || *rf.SNR != 6.25 || rf.RSSI == nil || *rf.RSSI != -97 {
		t.Fatalf("incorrect RF frame: %+v", rf)
	}
	wantPacket := Packet{Header: 0x15, Type: 5, TypeName: "GRP_TXT", Route: 1, RouteName: "FLOOD",
		Version: 0, PathHex: "aabbccdd", PathLength: 0x42, PathHashSize: 2, PathHopCount: 2}
	if rf.Packet == nil || *rf.Packet != wantPacket || rf.DecodeError != "" {
		t.Fatalf("incorrect parsed packet: %+v, error %q", rf.Packet, rf.DecodeError)
	}
	local := events[1]
	if !local.LocalLoopback || local.SNR != nil || local.RSSI != nil || local.RawPacketHex != "0e0001020304" || local.Packet == nil || local.Packet.TypeName != "ACK" {
		t.Fatalf("incorrect local frame: %+v", local)
	}
	bad := events[2]
	if bad.RawPacketHex != "1503aa" || bad.Packet != nil || !strings.Contains(bad.DecodeError, "not enough data for path") || bad.SNR != nil || bad.RSSI != nil {
		t.Fatalf("incorrect malformed frame: %+v", bad)
	}
	if events[3].Packet != nil || !strings.Contains(events[3].DecodeError, "unsupported payload version 1") || events[3].SNR != nil {
		t.Fatalf("unsupported version not retained: %+v", events[3])
	}
	ids := make(map[string]bool)
	for _, event := range events {
		if ids[event.EventID] {
			t.Fatal("distinct observations reused an event ID")
		}
		ids[event.EventID] = true
	}
	// Subscriber delivery can precede the publisher's QoS 1 PUBACK.
	deadline := time.Now().Add(5 * time.Second)
	for o.Stats().Published != 4 {
		if time.Now().After(deadline) {
			t.Fatalf("publisher acknowledgments did not settle: %+v", o.Stats())
		}
		time.Sleep(5 * time.Millisecond)
	}
	if err := o.Close(); err != nil {
		t.Fatal(err)
	}
	if status := receive(t, messages); string(status.payload) != "offline" || status.topic != topic+"/status" {
		t.Fatalf("unexpected shutdown status: %+v", status)
	}
	stats := o.Stats()
	if stats.Observed != 4 || stats.Published != 4 || stats.Dropped != 0 || stats.Queued != 0 {
		t.Fatalf("unexpected stats: %+v", stats)
	}
	late := connectTestClient(t, broker.Addr().String(), "", "")
	retained := subscribe(t, late, topic+"/#")
	if status := receive(t, retained); string(status.payload) != "offline" || !status.retained {
		t.Fatalf("offline status not retained: %+v", status)
	}
	timer := time.NewTimer(100 * time.Millisecond)
	defer timer.Stop()
	for {
		select {
		case m := <-retained:
			if m.topic != topic+"/status" || string(m.payload) != "offline" || m.retained {
				t.Fatalf("unexpected retained publication: %+v", m)
			}
		case <-timer.C:
			return
		}
	}
}

func TestOutageBoundsAdmissionAndRecovers(t *testing.T) {
	broker := startTestBroker(t, "127.0.0.1:0")
	address := broker.Addr().String()
	client := connectTestClient(t, address, "", "")
	topic := "meshcore/" + testIdentity
	statuses := subscribe(t, client, topic+"/status")
	o := newTestObserver(t, address, 2)
	if string(receive(t, statuses).payload) != "online" {
		t.Fatal("observer did not connect")
	}
	if err := broker.Close(); err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	for range 10000 {
		o.Observe([]byte{0x0d, 0, 1, 2, 3, 4}, 1, -95, true)
	}
	if time.Since(start) > time.Second {
		t.Fatal("MQTT outage blocked frame admission")
	}
	stats := o.Stats()
	if stats.Queued > 2 || stats.Dropped < 9997 {
		t.Fatalf("outage exceeded queue plus one pending event: %+v", stats)
	}
	broker = startTestBroker(t, address)
	reconnected := connectTestClient(t, address, "", "")
	statuses = subscribe(t, reconnected, topic+"/status")
	if string(receive(t, statuses).payload) != "online" {
		t.Fatal("observer did not reconnect")
	}
	packets := subscribe(t, reconnected, topic+"/packets")
	// Wait for the bounded backlog to leave the worker before adding a marker.
	deadline := time.Now().Add(5 * time.Second)
	for o.Stats().Published+o.Stats().Dropped != 10000 {
		if time.Now().After(deadline) {
			t.Fatalf("backlog did not settle: %+v", o.Stats())
		}
		time.Sleep(5 * time.Millisecond)
	}
	o.Observe([]byte{0x0d, 0, 0x99}, 2, -96, true)
	for {
		var event Event
		if err := json.Unmarshal(receive(t, packets).payload, &event); err != nil {
			t.Fatal(err)
		}
		if event.RawPacketHex == "0d0099" {
			break
		}
	}
}

func TestRetainedOnlineAndLastWill(t *testing.T) {
	broker := startTestBroker(t, "127.0.0.1:0")
	client := connectTestClient(t, broker.Addr().String(), "", "")
	topic := "meshcore/" + testIdentity + "/status"
	messages := subscribe(t, client, topic)
	o := newTestObserver(t, broker.Addr().String(), 1)
	if string(receive(t, messages).payload) != "online" {
		t.Fatal("missing online status")
	}
	late := connectTestClient(t, broker.Addr().String(), "", "")
	retained := subscribe(t, late, topic)
	if status := receive(t, retained); !status.retained || string(status.payload) != "online" {
		t.Fatalf("online status was not retained: %+v", status)
	}
	connection, ok := broker.server.Clients.Get(o.cfg.ClientID)
	if !ok {
		t.Fatal("observer connection not registered")
	}
	// Closing TCP without an MQTT DISCONNECT must trigger the configured will.
	if err := connection.Net.Conn.Close(); err != nil {
		t.Fatal(err)
	}
	if status := receive(t, messages); string(status.payload) != "offline" {
		t.Fatalf("missing offline last will: %+v", status)
	}
}

func TestUnavailableBrokerShutdownAndConcurrentAdmission(t *testing.T) {
	// A listening peer which never accepts/answers MQTT exercises the bounded
	// CONNACK timeout, rather than only immediate connection refusal.
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	o := newTestObserver(t, listener.Addr().String(), 2)
	var workers sync.WaitGroup
	for range 4 {
		workers.Go(func() {
			for range 1000 {
				o.Observe([]byte{0x15, 0}, 0, 0, false)
			}
		})
	}
	workers.Wait()
	start := time.Now()
	if err := o.Close(); err != nil {
		t.Fatal(err)
	}
	if time.Since(start) > 8*time.Second {
		t.Fatal("shutdown exceeded bounded connect timeout")
	}
	o.Observe([]byte{0x15, 0}, 0, 0, false)
	stats := o.Stats()
	if stats.Observed != 4001 || stats.Dropped != 4001 || stats.Published != 0 || stats.Queued != 0 {
		t.Fatalf("shutdown did not account for all rejected/buffered frames: %+v", stats)
	}
	if err := o.Start(context.Background()); err == nil {
		t.Fatal("closed observer restarted")
	}
}

func TestConfigurationAndAdmissionBounds(t *testing.T) {
	for _, cfg := range []Config{
		{QueueSize: -1}, {QueueSize: MaxQueueSize + 1}, {URL: "http://localhost"},
		{URL: "tcp://user:secret@localhost:1883"}, {TopicPrefix: "meshcore/#"},
		{TopicPrefix: "meshcore/\x00"}, {ClientID: "bad\x00id"},
	} {
		if _, err := New(testIdentity, cfg); err == nil {
			t.Fatalf("accepted invalid config: %+v", cfg)
		}
	}
	if _, err := New("not-a-public-key", Config{}); err == nil {
		t.Fatal("accepted invalid identity")
	}
	o, err := New(testIdentity, Config{Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	o.Observe([]byte{0x15, 0}, 0, 0, false)
	if err := o.Close(); err != nil {
		t.Fatal(err)
	}
	if o.Stats().Dropped != 1 {
		t.Fatal("pre-start observation was not counted as dropped")
	}
	o = newTestObserver(t, "127.0.0.1:1", 1)
	o.Observe(make([]byte, MaxFrameSize+1), 0, 0, false)
	if o.Stats().Dropped != 1 || o.Stats().Queued != 0 {
		t.Fatal("oversized frame entered queue")
	}
}

func TestBrokerBindPolicyAndAuthentication(t *testing.T) {
	for _, cfg := range []BrokerConfig{
		{Address: "0.0.0.0:0"}, {Address: ":0"}, {Address: "[::]:0"},
		{Address: "localhost:0"}, {Address: "127.0.0.1:0", Username: "only-user"},
		{Address: "127.0.0.1:0", Password: "only-password"},
	} {
		if b, err := StartBroker(cfg); err == nil {
			_ = b.Close()
			t.Fatalf("accepted unsafe broker config: %+v", cfg)
		}
	}
	b, err := StartBroker(BrokerConfig{Address: "127.0.0.1:0", Username: "reader", Password: "test-password", Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()
	for _, password := range []string{"", "incorrect"} {
		client := mqtt.NewClient(mqtt.NewClientOptions().AddBroker("tcp://" + b.Addr().String()).
			SetClientID(fmt.Sprintf("denied-%d", clientSequence.Add(1))).
			SetUsername("reader").SetPassword(password).SetConnectTimeout(time.Second))
		token := client.Connect()
		if !token.WaitTimeout(3*time.Second) || token.Error() == nil {
			client.Disconnect(0)
			t.Fatal("broker did not reject incorrect credentials")
		}
	}
	client := connectTestClient(t, b.Addr().String(), "reader", "test-password")
	messages := subscribe(t, client, "authenticated")
	token := client.Publish("authenticated", 1, false, "allowed")
	if !token.WaitTimeout(time.Second) || token.Error() != nil {
		t.Fatalf("authenticated publication failed: %v", token.Error())
	}
	if string(receive(t, messages).payload) != "allowed" {
		t.Fatal("authenticated subscription failed")
	}
}
