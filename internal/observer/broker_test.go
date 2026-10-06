package observer

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"runtime/pprof"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	mqtt "github.com/mochi-mqtt/server/v2"
)

func closeBrokerWithinDeadline(t *testing.T, broker *Broker) {
	t.Helper()
	done := make(chan error, 1)
	start := time.Now()
	go func() { done <- broker.Close() }()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
		t.Logf("broker shutdown: %s (deadline 10s)", time.Since(start))
	case <-time.After(10 * time.Second):
		var dump bytes.Buffer
		_ = pprof.Lookup("goroutine").WriteTo(&dump, 2)
		t.Fatalf("broker shutdown exceeded 10s:\n%s", &dump)
	}
}

func brokerWireClient(t *testing.T, address, id string, wantReason byte) net.Conn {
	t.Helper()
	conn, err := net.DialTimeout("tcp", address, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	if err := conn.SetDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatal(err)
	}
	connect := []byte{0x10, byte(12 + len(id)), 0, 4, 'M', 'Q', 'T', 'T', 4, 2, 0, 10, 0, byte(len(id))}
	if _, err := conn.Write(append(connect, id...)); err != nil {
		t.Fatal(err)
	}
	var ack [4]byte
	if _, err := io.ReadFull(conn, ack[:]); err != nil {
		t.Fatal(err)
	}
	if want := [4]byte{0x20, 2, 0, wantReason}; ack != want {
		t.Fatalf("CONNACK for %s: %x, want %x", id, ack, want)
	}
	return conn
}

func TestBrokerCapacityDisconnectShutdown(t *testing.T) {
	broker, err := StartBroker(BrokerConfig{Address: "127.0.0.1:0", Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	address := broker.Addr().String()
	clients := make([]net.Conn, 0, 130)
	for index := range 128 {
		clients = append(clients, brokerWireClient(t, address, fmt.Sprintf("capacity-%d", index), 0))
	}
	clients = append(clients, brokerWireClient(t, address, "over-capacity", 3))
	if _, err := clients[0].Write([]byte{0xe0, 0}); err != nil {
		t.Fatal(err)
	}
	if err := clients[0].Close(); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for atomic.LoadInt64(&broker.server.Info.ClientsConnected) != 127 {
		if time.Now().After(deadline) {
			t.Fatal("closed client did not release its capacity slot")
		}
		time.Sleep(time.Millisecond)
	}
	clients = append(clients, brokerWireClient(t, address, "capacity-recovered", 0))
	for _, conn := range clients[1:] {
		if err := conn.Close(); err != nil {
			t.Fatal(err)
		}
	}
	closeBrokerWithinDeadline(t, broker)
	if count := atomic.LoadInt64(&broker.server.Info.ClientsConnected); count != 0 {
		t.Fatalf("shutdown left %d connected clients", count)
	}
	listener, err := net.Listen("tcp", address)
	if err != nil {
		t.Fatalf("closed listener cannot be rebound: %v", err)
	}
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	if err := broker.Close(); err != nil {
		t.Fatalf("repeated Close: %v", err)
	}
}

func TestBrokerCloseWithConcurrentRegistryDeletes(t *testing.T) {
	for iteration := range 16 {
		t.Run(fmt.Sprint(iteration), func(t *testing.T) {
			broker, err := StartBroker(BrokerConfig{Address: "127.0.0.1:0", Logger: quietLogger()})
			if err != nil {
				t.Fatal(err)
			}
			stop := make(chan struct{})
			stopWriters := sync.OnceFunc(func() { close(stop) })
			defer stopWriters()
			var ready, writers sync.WaitGroup
			for index := range 4 {
				ready.Add(1)
				writers.Go(func() {
					client := &mqtt.Client{ID: fmt.Sprintf("closed-%d", index)}
					ready.Done()
					for {
						select {
						case <-stop:
							return
						default:
							broker.server.Clients.Add(client)
							broker.server.Clients.Delete(client.ID)
						}
					}
				})
			}
			ready.Wait()
			closeBrokerWithinDeadline(t, broker)
			// Stop the registry writers before the next broker instance.
			stopWriters()
			writers.Wait()
		})
	}
}

func TestBrokerConcurrentCloseWithLiveClient(t *testing.T) {
	broker, err := StartBroker(BrokerConfig{Address: "127.0.0.1:0", Logger: quietLogger()})
	if err != nil {
		t.Fatal(err)
	}
	client := brokerWireClient(t, broker.Addr().String(), "live-close", 0)
	results := make(chan error, 8)
	for range cap(results) {
		go func() { results <- broker.Close() }()
	}
	deadline := time.After(10 * time.Second)
	for range cap(results) {
		select {
		case err := <-results:
			if err != nil {
				t.Fatal(err)
			}
		case <-deadline:
			t.Fatal("concurrent Close exceeded 10s")
		}
	}
	var disconnect [2]byte
	if _, err := io.ReadFull(client, disconnect[:]); err != nil {
		t.Fatal(err)
	}
	if disconnect != [2]byte{0xe0, 0} {
		t.Fatalf("shutdown DISCONNECT: %x", disconnect)
	}
	if _, err := client.Read(make([]byte, 1)); err != io.EOF {
		t.Fatalf("shutdown connection read: %v, want EOF", err)
	}
}
