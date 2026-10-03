package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"math"
	"strings"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"meshcore.local/meshcore/internal/app"
	"meshcore.local/meshcore/internal/observer"
)

func TestMeasuredObserverEvidenceRejectsMissingSyntheticAndInvalidSignals(t *testing.T) {
	identity := fixtureID(4).Identity.String()
	no, yes := false, true
	snr, rssi := 13.75, -32.0
	nan, inf, markerSNR, markerRSSI := math.NaN(), math.Inf(1), -32.0, 127.0
	tooHigh, fractional, excessiveSNR := 128.0, -31.5, 32.0
	valid := observerEvent{ObserverIdentity: identity, LocalLoopback: &no, SNR: &snr, RSSI: &rssi}
	cases := []struct {
		name     string
		modify   func(*observerEvent)
		retained bool
		wantErr  bool
	}{
		{"physical", func(*observerEvent) {}, false, false},
		{"retained", func(*observerEvent) {}, true, true},
		{"wrong_identity", func(e *observerEvent) { e.ObserverIdentity = fixtureID(3).Identity.String() }, false, true},
		{"missing_loopback", func(e *observerEvent) { e.LocalLoopback = nil }, false, true},
		{"loopback", func(e *observerEvent) { e.LocalLoopback = &yes }, false, true},
		{"null_snr", func(e *observerEvent) { e.SNR = nil }, false, true},
		{"null_rssi", func(e *observerEvent) { e.RSSI = nil }, false, true},
		{"nan", func(e *observerEvent) { e.SNR = &nan }, false, true},
		{"infinite", func(e *observerEvent) { e.RSSI = &inf }, false, true},
		{"marker_even_if_loopback_false", func(e *observerEvent) { e.SNR, e.RSSI = &markerSNR, &markerRSSI }, false, true},
		{"rssi_out_of_range", func(e *observerEvent) { e.RSSI = &tooHigh }, false, true},
		{"fractional_rssi", func(e *observerEvent) { e.RSSI = &fractional }, false, true},
		{"snr_out_of_range", func(e *observerEvent) { e.SNR = &excessiveSNR }, false, true},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			event := valid
			test.modify(&event)
			evidence, err := measuredObserverEvent(event, identity, test.retained)
			if (err != nil) != test.wantErr {
				t.Fatalf("evidence=%+v err=%v wantError=%t", evidence, err, test.wantErr)
			}
			if !test.wantErr && (evidence.snr != 13.75 || evidence.rssi != -32) {
				t.Fatalf("measured signal changed: %+v", evidence)
			}
		})
	}
}

func TestRealPahoSubscriberMatchesOnlyOriginalPhysicalObserverPacket(t *testing.T) {
	broker, err := observer.StartBroker(observer.BrokerConfig{
		Address: "127.0.0.1:0", Username: "rf-test", Password: "test-only-password",
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := broker.Close(); err != nil {
			t.Error(err)
		}
	})
	t.Setenv("RF_CHECK_TEST_MQTT_USER", "rf-test")
	t.Setenv("RF_CHECK_TEST_MQTT_PASS", "test-only-password")
	cfg := app.MQTTConfig{
		URL: "tcp://" + broker.Addr().String(), TopicPrefix: "/rf-acceptance/",
		UsernameEnv: "RF_CHECK_TEST_MQTT_USER", PasswordEnv: "RF_CHECK_TEST_MQTT_PASS",
	}
	identity := fixtureID(4).Identity
	raw := []byte{0x15, 0x80, 1, 2, 3}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	probe, err := openMQTTProbe(ctx, cfg, identity, raw)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(probe.Close)
	expectedTopic := "rf-acceptance/" + identity.String() + "/packets"
	if probe.topic != expectedTopic {
		t.Fatalf("observer topic = %s", probe.topic)
	}
	publisher := mqtt.NewClient(mqtt.NewClientOptions().AddBroker(cfg.URL).
		SetClientID("rf-check-test-publisher").SetUsername("rf-test").SetPassword("test-only-password").
		SetConnectTimeout(time.Second).SetAutoReconnect(false))
	t.Cleanup(func() { publisher.Disconnect(100) })
	if err := mqttToken(ctx, publisher.Connect()); err != nil {
		t.Fatal(err)
	}
	publish := func(topic, rawHex string) {
		t.Helper()
		body, err := json.Marshal(map[string]any{
			"observer_identity": identity.String(), "raw_packet_hex": rawHex,
			"local_loopback": false, "snr": 13.75, "rssi": -32,
		})
		if err != nil {
			t.Fatal(err)
		}
		if err := mqttToken(ctx, publisher.Publish(topic, 1, false, body)); err != nil {
			t.Fatal(err)
		}
	}
	// A forwarded copy has the same payload but a different complete wire frame.
	publish(expectedTopic, "1581aabbcc010203")
	publish("rf-acceptance/another-observer/packets", hex.EncodeToString(raw))
	short, stop := context.WithTimeout(ctx, 25*time.Millisecond)
	_, err = probe.wait(short)
	stop()
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("unrelated topic/forwarded frame qualified as original RF evidence: %v", err)
	}
	publish(expectedTopic, hex.EncodeToString(raw))
	evidence, err := probe.wait(ctx)
	if err != nil || evidence.snr != 13.75 || evidence.rssi != -32 {
		t.Fatalf("Paho RF evidence=%+v error=%v", evidence, err)
	}
}

func TestMQTTCredentialsFailClearlyWhenConfiguredEnvironmentIsEmpty(t *testing.T) {
	t.Setenv("RF_CHECK_MISSING_MQTT_SECRET", "")
	_, err := mqttEnv("RF_CHECK_MISSING_MQTT_SECRET")
	if err == nil || !strings.Contains(err.Error(), "empty or unset") {
		t.Fatalf("missing configured credential: %v", err)
	}
	if value, err := mqttEnv(""); value != "" || err != nil {
		t.Fatalf("anonymous configuration should not read an environment variable: %q %v", value, err)
	}
}
