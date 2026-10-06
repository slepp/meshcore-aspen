package observer

import (
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"sync"

	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/mochi-mqtt/server/v2/packets"
)

type BrokerConfig struct {
	Address, Username, Password string
	Logger                      *slog.Logger
}

type Broker struct {
	server *mqtt.Server
	addr   net.Addr
	once   sync.Once
	err    error
}

// StartBroker starts an in-memory TCP MQTT broker. Empty Address means
// 127.0.0.1:1883. Anonymous access is restricted to literal loopback binds.
// With credentials all clients must authenticate, and have full topic access.
// TCP credentials are not encrypted: use an external TLS broker on untrusted networks.
func StartBroker(cfg BrokerConfig) (*Broker, error) {
	if cfg.Address == "" {
		cfg.Address = "127.0.0.1:1883"
	}
	host, _, err := net.SplitHostPort(cfg.Address)
	if err != nil {
		return nil, fmt.Errorf("MQTT bind address: %w", err)
	}
	if (cfg.Username == "") != (cfg.Password == "") {
		return nil, errors.New("MQTT broker username and password must both be set")
	}
	if cfg.Username == "" {
		ip := net.ParseIP(host)
		if ip == nil || !ip.IsLoopback() {
			return nil, errors.New("anonymous MQTT broker requires a literal loopback bind address")
		}
	}
	server := mqtt.New(&mqtt.Options{Logger: cfg.Logger})
	server.Options.Capabilities.MaximumClients = 128
	server.Options.Capabilities.MaximumPacketSize = 64 * 1024
	server.Options.Capabilities.MaximumInflight = 128
	server.Options.Capabilities.MaximumClientWritesPending = 256
	if err := server.AddHook(&brokerAuth{username: cfg.Username, password: cfg.Password}, nil); err != nil {
		_ = server.Close()
		return nil, fmt.Errorf("MQTT broker authentication: %w", err)
	}
	listener := listeners.NewTCP(listeners.Config{ID: "observer", Address: cfg.Address})
	if err := server.AddListener(listener); err != nil {
		_ = server.Close()
		return nil, fmt.Errorf("MQTT broker listen: %w", err)
	}
	if err := server.Serve(); err != nil {
		_ = server.Close()
		return nil, fmt.Errorf("MQTT broker start: %w", err)
	}
	addr, err := net.ResolveTCPAddr("tcp", listener.Address())
	if err != nil {
		_ = server.Close()
		return nil, err
	}
	return &Broker{server: server, addr: addr}, nil
}

func (b *Broker) Addr() net.Addr { return b.addr }

func (b *Broker) Close() error {
	b.once.Do(func() {
		// Mochi v2.7.9 GetByListener recursively RLocks the client registry
		// and can deadlock against disconnect cleanup (upstream issue #488).
		// Close listeners with a single snapshot first; Server.Close's
		// idempotent listener close then skips that lookup.
		b.server.Listeners.CloseAll(func(id string) {
			for _, client := range b.server.Clients.GetAll() {
				if client.Net.Listener == id && !client.Closed() {
					_ = b.server.DisconnectClient(client, packets.ErrServerShuttingDown)
				}
			}
		})
		b.err = b.server.Close()
	})
	return b.err
}

type brokerAuth struct {
	mqtt.HookBase
	username, password string
}

func (*brokerAuth) ID() string { return "observer-auth" }

func (*brokerAuth) Provides(event byte) bool {
	return event == mqtt.OnConnectAuthenticate || event == mqtt.OnACLCheck
}

func (a *brokerAuth) OnConnectAuthenticate(_ *mqtt.Client, packet packets.Packet) bool {
	if a.username == "" {
		return true
	}
	username := sha256.Sum256(packet.Connect.Username)
	password := sha256.Sum256(packet.Connect.Password)
	wantUsername := sha256.Sum256([]byte(a.username))
	wantPassword := sha256.Sum256([]byte(a.password))
	return subtle.ConstantTimeCompare(username[:], wantUsername[:])&
		subtle.ConstantTimeCompare(password[:], wantPassword[:]) == 1
}

func (*brokerAuth) OnACLCheck(_ *mqtt.Client, _ string, _ bool) bool { return true }
