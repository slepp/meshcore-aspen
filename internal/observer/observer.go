// Package observer exports received MeshCore frames as bounded, best-effort MQTT
// telemetry. It has no radio or node interface and cannot transmit on RF.
package observer

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	meshcore "github.com/meshcore-go/meshcore-go"
)

const (
	DefaultQueueSize = 256
	MaxQueueSize     = 4096
	MaxFrameSize     = 4096
	ioTimeout        = 3 * time.Second
)

type Config struct {
	URL, ClientID, TopicPrefix, Username, Password                string
	QueueSize                                                     int
	Logger                                                        *slog.Logger
	Format, IATA, Origin, Model, FirmwareVersion, Radio, Audience string
	Sign                                                          func([]byte) []byte
	TLSConfig                                                     *tls.Config
	PacketFilter                                                  *uint16
}

// Event describes one observation, not a decrypted or authenticated message.
// EventID stays unchanged when a QoS 1 publication is retried.
type Event struct {
	EventID          string    `json:"event_id"`
	Timestamp        time.Time `json:"timestamp"`
	ObserverIdentity string    `json:"observer_identity"`
	RawPacketHex     string    `json:"raw_packet_hex"`
	SNR              *float32  `json:"snr"`
	RSSI             *int8     `json:"rssi"`
	LocalLoopback    bool      `json:"local_loopback"`
	Packet           *Packet   `json:"packet,omitempty"`
	DecodeError      string    `json:"decode_error,omitempty"`
}

type Packet struct {
	Header       byte   `json:"header"`
	Type         byte   `json:"type"`
	TypeName     string `json:"type_name"`
	Route        byte   `json:"route"`
	RouteName    string `json:"route_name"`
	Version      byte   `json:"version"`
	PathHex      string `json:"path_hex"`
	PathLength   byte   `json:"path_length"`
	PathHashSize uint8  `json:"path_hash_size"`
	PathHopCount uint8  `json:"path_hop_count"`
}

type Stats struct {
	Observed  uint64 `json:"observed"`
	Published uint64 `json:"published"`
	Dropped   uint64 `json:"dropped"`
	Queued    int    `json:"queued"`
}

type observation struct {
	data      []byte
	timestamp time.Time
	sequence  uint64
	snr       float32
	rssi      int8
	hasSignal bool
}

type Observer struct {
	cfg       Config
	identity  string
	session   string
	topic     string
	queue     chan observation
	mu        sync.Mutex
	started   bool
	closed    bool
	client    mqtt.Client
	cancel    context.CancelFunc
	done      chan struct{}
	observed  atomic.Uint64
	published atomic.Uint64
	dropped   atomic.Uint64
	now       func() time.Time
}

func New(identityHex string, cfg Config) (*Observer, error) {
	identity, err := hex.DecodeString(identityHex)
	if err != nil || len(identity) != meshcore.PubKeySize {
		return nil, errors.New("observer identity must be a 32-byte public key in hex")
	}
	if cfg.URL == "" {
		cfg.URL = "tcp://127.0.0.1:1883"
	}
	u, err := url.Parse(cfg.URL)
	if err != nil || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" ||
		(u.Scheme != "ws" && u.Scheme != "wss" && u.Path != "" && u.Path != "/") {
		return nil, errors.New("invalid MQTT URL; use tcp://host:port or tls://host:port without credentials")
	}
	if u.Scheme != "tcp" && u.Scheme != "tls" && u.Scheme != "ssl" && u.Scheme != "ws" && u.Scheme != "wss" {
		return nil, errors.New("MQTT URL scheme must be tcp, tls, ssl, ws or wss")
	}
	if cfg.Format != "" && cfg.Format != "internal-v1" && !publicFormat(cfg.Format) {
		return nil, errors.New("MQTT format must be internal-v1, observer-v1 or capture-v1")
	}
	if publicFormat(cfg.Format) {
		if len(cfg.IATA) != 3 || strings.IndexFunc(cfg.IATA, func(r rune) bool { return r < 'A' || r > 'Z' }) >= 0 {
			return nil, errors.New("public observer requires a three-uppercase-letter IATA")
		}
		if cfg.Origin == "" {
			cfg.Origin = "MeshCore observer"
		}
		if cfg.Model == "" {
			cfg.Model = "MeshCore host"
		}
		if cfg.FirmwareVersion == "" {
			cfg.FirmwareVersion = "unknown"
		}
		if cfg.Radio == "" {
			cfg.Radio = "unknown"
		}
		if cfg.Audience != "" && (cfg.Sign == nil || cfg.Username != "" || cfg.Password != "") {
			return nil, errors.New("observer JWT requires its identity signer and no static credentials")
		}
		if cfg.TLSConfig != nil && cfg.TLSConfig.InsecureSkipVerify {
			return nil, errors.New("public observer TLS certificate verification must remain enabled")
		}
	} else if cfg.Audience != "" {
		return nil, errors.New("observer JWT requires a public MQTT format")
	}
	if cfg.QueueSize == 0 {
		cfg.QueueSize = DefaultQueueSize
	}
	if cfg.QueueSize < 1 || cfg.QueueSize > MaxQueueSize {
		return nil, fmt.Errorf("observer queue size must be 1..%d", MaxQueueSize)
	}
	cfg.TopicPrefix = strings.Trim(cfg.TopicPrefix, "/")
	if cfg.TopicPrefix == "" {
		cfg.TopicPrefix = "meshcore"
	}
	if !utf8.ValidString(cfg.TopicPrefix) || strings.ContainsAny(cfg.TopicPrefix, "+#\x00") || len(cfg.TopicPrefix) > 65000 {
		return nil, errors.New("invalid MQTT topic prefix")
	}
	identityHex = hex.EncodeToString(identity)
	if cfg.ClientID == "" {
		cfg.ClientID = "meshcore-observer-" + identityHex
	}
	if !utf8.ValidString(cfg.ClientID) || strings.ContainsRune(cfg.ClientID, 0) || len(cfg.ClientID) > 65535 {
		return nil, errors.New("invalid MQTT client ID")
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	var session [16]byte
	if _, err := rand.Read(session[:]); err != nil {
		return nil, fmt.Errorf("observer event ID entropy: %w", err)
	}
	topic := cfg.TopicPrefix + "/" + identityHex
	if publicFormat(cfg.Format) {
		topic = cfg.TopicPrefix + "/" + cfg.IATA + "/" + strings.ToUpper(identityHex)
	}
	return &Observer{
		cfg: cfg, identity: identityHex, session: hex.EncodeToString(session[:]),
		topic: topic, queue: make(chan observation, cfg.QueueSize),
		done: make(chan struct{}),
		now:  time.Now,
	}, nil
}

// Start starts one publisher worker. Broker outages do not prevent startup.
// The observer is single-use; cancellation closes it and discards queued events.
func (o *Observer) Start(ctx context.Context) error {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed || o.started {
		return errors.New("observer already started or closed")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	ctx, o.cancel = context.WithCancel(ctx)
	o.started = true
	go o.run(ctx)
	return nil
}

// Observe copies a frame without waiting for MQTT or queue capacity. Frames
// before Start, after shutdown, over MaxFrameSize or beyond capacity are dropped.
// Logging and JSON decoding occur only in the publisher worker.
func (o *Observer) Observe(data []byte, snr float32, rssi int8, hasSignal bool) {
	if publicFormat(o.cfg.Format) && (!hasSignal || (snr == -32 && rssi == 127) ||
		math.IsNaN(float64(snr)) || math.IsInf(float64(snr), 0)) {
		return
	}
	sequence := o.observed.Add(1)
	o.mu.Lock()
	defer o.mu.Unlock()
	if !o.started || o.closed || len(data) > MaxFrameSize || len(o.queue) == cap(o.queue) {
		o.dropped.Add(1)
		return
	}
	item := observation{
		data: append([]byte(nil), data...), timestamp: time.Now().UTC(), sequence: sequence,
		snr: snr, rssi: rssi, hasSignal: hasSignal,
	}
	select {
	case o.queue <- item:
	default:
		o.dropped.Add(1)
	}
}

func (o *Observer) Stats() Stats {
	return Stats{Observed: o.observed.Load(), Published: o.published.Load(), Dropped: o.dropped.Load(), Queued: len(o.queue)}
}

// Connected reports an open publisher connection after the broker acknowledges
// its online status. It uses Paho's live connection state, not packet counters.
// Startup, disconnect and shutdown return false without waiting for network I/O.
func (o *Observer) Connected() bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	return !o.closed && o.client != nil && o.client.IsConnectionOpen()
}

// Close stops admission immediately and waits for bounded network shutdown.
// It does not drain to MQTT: queued and unacknowledged events count as drops.
func (o *Observer) Close() error {
	o.mu.Lock()
	o.closed = true
	o.client = nil
	if !o.started {
		o.mu.Unlock()
		return nil
	}
	o.cancel()
	o.mu.Unlock()
	<-o.done
	return nil
}

func (o *Observer) event(item observation) Event {
	event := Event{
		EventID: fmt.Sprintf("%s-%d", o.session, item.sequence), Timestamp: item.timestamp,
		ObserverIdentity: o.identity, RawPacketHex: hex.EncodeToString(item.data),
		LocalLoopback: item.hasSignal && item.snr == -32 && item.rssi == 127,
	}
	if item.hasSignal && !event.LocalLoopback {
		event.RSSI = &item.rssi
		if !math.IsNaN(float64(item.snr)) && !math.IsInf(float64(item.snr), 0) {
			event.SNR = &item.snr
		}
	}
	packet, err := meshcore.PacketFromBytes(item.data)
	if err != nil {
		event.DecodeError = err.Error()
		return event
	}
	event.Packet = &Packet{
		Header: packet.Header, Type: packet.PayloadType(), TypeName: packet.PayloadTypeString(),
		Route: packet.RouteType(), RouteName: packet.RouteTypeString(), Version: packet.PayloadVer(),
		PathHex: hex.EncodeToString(packet.Path), PathLength: packet.PathLength,
		PathHashSize: packet.PathHashSize(), PathHopCount: packet.PathHashCount(),
	}
	return event
}

func (o *Observer) run(ctx context.Context) {
	var client mqtt.Client
	var pending []byte
	var lastDropped uint64
	var renewAt time.Time
	var issuedAt time.Time
	var lastReadyTime time.Time
	status := func(state string) string {
		if publicFormat(o.cfg.Format) {
			current := o.now()
			if current.Unix() >= 1735689600 {
				lastReadyTime = current
			}
			return o.publicStatus(state, lastReadyTime)
		}
		return state
	}
	logDrops := func() {
		if dropped := o.dropped.Load(); dropped != lastDropped {
			o.cfg.Logger.Warn("observer telemetry dropped", "dropped_total", dropped, "dropped_since_log", dropped-lastDropped)
			lastDropped = dropped
		}
	}
	defer func() {
		o.mu.Lock()
		o.closed = true
		o.mu.Unlock()
		if client != nil {
			if client.IsConnectionOpen() {
				client.Publish(o.topic+"/status", 1, true, status("offline")).WaitTimeout(time.Second)
			}
			if publicFormat(o.cfg.Format) {
				client.Disconnect(100)
			} else {
				client.Disconnect(0)
			}
		}
		if pending != nil {
			o.dropped.Add(1)
		}
		for len(o.queue) > 0 {
			<-o.queue
			o.dropped.Add(1)
		}
		logDrops()
		close(o.done)
	}()
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	retry := func() bool {
		logDrops()
		timer := time.NewTimer(time.Second)
		defer timer.Stop()
		select {
		case <-ctx.Done():
			return false
		case <-timer.C:
			return true
		}
	}
	for ctx.Err() == nil {
		currentTime := o.now()
		if client != nil && publicFormat(o.cfg.Format) &&
			(currentTime.Unix() < 1735689600 ||
				(!issuedAt.IsZero() && currentTime.Before(issuedAt)) ||
				(!renewAt.IsZero() && !currentTime.Before(renewAt))) {
			client.Disconnect(0)
			o.mu.Lock()
			o.client = nil
			o.mu.Unlock()
		}
		if client == nil || !client.IsConnectionOpen() {
			if client != nil {
				client.Disconnect(0)
			}
			// A fresh clean-session client discards any Paho inflight state.
			// Only pending is retried, keeping outages bounded to queue + 1.
			now := o.now()
			username, password := o.cfg.Username, o.cfg.Password
			if publicFormat(o.cfg.Format) && now.Unix() < 1735689600 {
				if !retry() {
					return
				}
				continue
			}
			if o.cfg.Audience != "" {
				var err error
				password, err = authToken(o.identity, o.cfg.Audience, now, o.cfg.Sign)
				if err != nil {
					o.cfg.Logger.Warn("observer identity authentication unavailable; retrying")
					if !retry() {
						return
					}
					continue
				}
				username = "v1_" + strings.ToUpper(o.identity)
				issuedAt = now
				renewAt = now.Add(tokenLifetime - renewalMargin)
			}
			opts := mqtt.NewClientOptions().AddBroker(o.cfg.URL).SetClientID(o.cfg.ClientID).
				SetUsername(username).SetPassword(password).SetTLSConfig(o.cfg.TLSConfig).
				SetCleanSession(true).SetAutoReconnect(false).SetConnectRetry(false).
				SetConnectTimeout(ioTimeout).SetWriteTimeout(ioTimeout).
				SetKeepAlive(10*time.Second).SetPingTimeout(ioTimeout).
				SetWill(o.topic+"/status", status("offline"), 1, true)
			client = mqtt.NewClient(opts)
			token := client.Connect()
			token.Wait()
			if token.Error() != nil {
				// Do not log connection errors: upstream errors can contain credentials.
				o.cfg.Logger.Warn("observer MQTT connection failed; retrying")
				if !retry() {
					return
				}
				continue
			}
			if !waitToken(ctx, client.Publish(o.topic+"/status", 1, true, status("online"))) {
				client.Disconnect(0)
				if !retry() {
					return
				}
				continue
			}
			o.mu.Lock()
			o.client = client
			o.mu.Unlock()
			o.cfg.Logger.Info("observer MQTT connected", "topic", o.topic+"/packets")
		}
		if pending == nil {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				logDrops()
				continue
			case item := <-o.queue:
				var err error
				if publicFormat(o.cfg.Format) {
					pending, err = o.publicPacket(item)
				} else {
					pending, err = json.Marshal(o.event(item))
				}
				if err != nil {
					o.dropped.Add(1)
					continue
				}
			}
		}
		if waitToken(ctx, client.Publish(o.topic+"/packets", 1, false, pending)) {
			o.published.Add(1)
			pending = nil
		} else {
			o.mu.Lock()
			o.client = nil
			o.mu.Unlock()
			client.Disconnect(0)
			if !retry() {
				return
			}
		}
	}
}

func waitToken(ctx context.Context, token mqtt.Token) bool {
	timer := time.NewTimer(ioTimeout)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return false
	case <-token.Done():
		return token.Error() == nil
	}
}
