package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"strings"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/app"
)

type observerEvent struct {
	ObserverIdentity string   `json:"observer_identity"`
	RawPacketHex     string   `json:"raw_packet_hex"`
	LocalLoopback    *bool    `json:"local_loopback"`
	SNR              *float64 `json:"snr"`
	RSSI             *float64 `json:"rssi"`
}

type observerEvidence struct {
	snr  float64
	rssi int8
}

type mqttProbe struct {
	client   mqtt.Client
	topic    string
	received chan observerEvidence
	failures chan error
}

func mqttEnv(name string) (string, error) {
	if name == "" {
		return "", nil
	}
	value, ok := os.LookupEnv(name)
	if !ok || value == "" {
		return "", fmt.Errorf("MQTT credential environment variable %s is empty or unset", name)
	}
	return value, nil
}

func openMQTTProbe(ctx context.Context, cfg app.MQTTConfig, observer meshcore.Identity, packet []byte) (*mqttProbe, error) {
	username, err := mqttEnv(cfg.UsernameEnv)
	if err != nil {
		return nil, fmt.Errorf("rf_observer_mqtt credentials: %w", err)
	}
	password, err := mqttEnv(cfg.PasswordEnv)
	if err != nil {
		return nil, fmt.Errorf("rf_observer_mqtt credentials: %w", err)
	}
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return nil, err
	}
	prefix := strings.Trim(cfg.TopicPrefix, "/")
	if prefix == "" {
		prefix = "meshcore"
	}
	p := &mqttProbe{
		topic:    prefix + "/" + observer.String() + "/packets",
		received: make(chan observerEvidence, 1), failures: make(chan error, 1),
	}
	options := mqtt.NewClientOptions().AddBroker(cfg.URL).
		SetClientID("meshcore-rf-check-" + hex.EncodeToString(nonce[:])).
		SetUsername(username).SetPassword(password).
		SetCleanSession(true).SetAutoReconnect(false).SetConnectRetry(false).
		SetConnectTimeout(3 * time.Second).SetWriteTimeout(3 * time.Second).
		SetKeepAlive(10 * time.Second).SetPingTimeout(3 * time.Second).
		SetConnectionLostHandler(func(_ mqtt.Client, err error) { p.fail(err) })
	p.client = mqtt.NewClient(options)
	if err := mqttToken(ctx, p.client.Connect()); err != nil {
		p.Close()
		return nil, fmt.Errorf("rf_observer_mqtt connect: %w", err)
	}
	expected := hex.EncodeToString(packet)
	handler := func(_ mqtt.Client, message mqtt.Message) {
		if len(message.Payload()) > 65536 {
			return
		}
		var event observerEvent
		if err := json.Unmarshal(message.Payload(), &event); err != nil || event.RawPacketHex != expected {
			return
		}
		evidence, err := measuredObserverEvent(event, observer.String(), message.Retained())
		if err != nil {
			p.fail(err)
			return
		}
		select {
		case p.received <- evidence:
		default:
		}
	}
	if err := mqttToken(ctx, p.client.Subscribe(p.topic, 1, handler)); err != nil {
		p.Close()
		return nil, fmt.Errorf("rf_observer_mqtt subscribe: %w", err)
	}
	return p, nil
}

func measuredObserverEvent(event observerEvent, identity string, retained bool) (observerEvidence, error) {
	if retained {
		return observerEvidence{}, errors.New("matching MQTT packet was retained, not a fresh observer publication")
	}
	if event.ObserverIdentity != identity {
		return observerEvidence{}, errors.New("matching MQTT packet has the wrong observer identity")
	}
	if event.LocalLoopback == nil || *event.LocalLoopback || event.SNR == nil || event.RSSI == nil {
		return observerEvidence{}, errors.New("matching MQTT packet lacks measured non-loopback RF metadata")
	}
	snr, rssi := *event.SNR, *event.RSSI
	if math.IsNaN(snr) || math.IsInf(snr, 0) || math.IsNaN(rssi) || math.IsInf(rssi, 0) ||
		snr < -32 || snr > 31.75 || rssi < -128 || rssi > 127 || math.Trunc(rssi) != rssi ||
		(snr == -32 && rssi == 127) {
		return observerEvidence{}, errors.New("matching MQTT packet has invalid or synthetic signal measurements")
	}
	return observerEvidence{snr: snr, rssi: int8(rssi)}, nil
}

func mqttToken(ctx context.Context, token mqtt.Token) error {
	timer := time.NewTimer(5 * time.Second)
	defer timer.Stop()
	select {
	case <-token.Done():
		return token.Error()
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return errors.New("MQTT operation timed out")
	}
}

func (p *mqttProbe) fail(err error) {
	select {
	case p.failures <- err:
	default:
	}
}

func (p *mqttProbe) wait(ctx context.Context) (observerEvidence, error) {
	select {
	case evidence := <-p.received:
		return evidence, nil
	case err := <-p.failures:
		return observerEvidence{}, fmt.Errorf("rf_observer_mqtt: %w", err)
	case <-ctx.Done():
		return observerEvidence{}, fmt.Errorf("rf_observer_mqtt: %w (matching physical packet event missing)", ctx.Err())
	}
}

func (p *mqttProbe) Close() {
	if p.client != nil {
		p.client.Disconnect(100)
	}
}
