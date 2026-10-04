package companion

import (
	"context"
	"errors"
	"io"
	"net"
	"testing"
	"time"
)

func TestInitialFrameDeadlineReclaimsIdleAndTricklingSlots(t *testing.T) {
	t.Parallel()
	cfg := testConfig(stateDir(t))
	cfg.ErrorHandler = func(error) {}
	server, _, address := startTestServer(t, testIdentity(97), cfg)
	start := time.Now()
	connections := make([]net.Conn, maxClients)
	for i := range connections {
		conn, err := net.Dial("tcp", address)
		if err != nil {
			t.Fatal(err)
		}
		connections[i] = conn
		t.Cleanup(func() { _ = conn.Close() })
		var initial []byte
		switch i % 4 {
		case 1, 3:
			initial = []byte{'<'}
		case 2:
			initial = []byte{'<', 32, 0}
		}
		if len(initial) > 0 {
			if _, err := conn.Write(initial); err != nil {
				t.Fatal(err)
			}
		}
	}
	admissionDeadline := time.Now().Add(time.Second)
	for {
		server.mu.Lock()
		count := len(server.clients)
		server.mu.Unlock()
		if count == maxClients {
			break
		}
		if time.Now().After(admissionDeadline) {
			t.Fatalf("only %d of %d adversarial connections occupied slots", count, maxClients)
		}
		time.Sleep(time.Millisecond)
	}
	for step := 1; step <= 3; step++ {
		time.Sleep(time.Until(start.Add(time.Duration(step) * frameReadTimeout * 3 / 10)))
		for i, conn := range connections {
			var fragment []byte
			switch i % 4 {
			case 1:
				fragment = []byte{0}
				if step == 1 {
					fragment[0] = 32
				}
			case 2:
				fragment = []byte{0}
			case 3:
				if step == 3 {
					fragment = []byte{32, 0}
				}
			}
			if len(fragment) > 0 {
				if _, err := conn.Write(fragment); err != nil {
					t.Fatal(err)
				}
			}
		}
	}
	for i, conn := range connections {
		if err := conn.SetReadDeadline(start.Add(frameReadTimeout + 2*time.Second)); err != nil {
			t.Fatal(err)
		}
		var response [1]byte
		_, err := io.ReadFull(conn, response[:])
		var timeout net.Error
		if err == nil || errors.As(err, &timeout) && timeout.Timeout() {
			t.Fatalf("incomplete initial frame retained slot %d beyond the whole-frame deadline: %v", i, err)
		}
	}
	deadline := time.Now().Add(time.Second)
	for {
		server.mu.Lock()
		count := len(server.clients)
		server.mu.Unlock()
		if count == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("%d companion slots were not reclaimed", count)
		}
		time.Sleep(time.Millisecond)
	}
	client := testClient(t, address)
	if _, err := client.DeviceQuery(testContext(t)); err != nil {
		t.Fatalf("legitimate companion could not use reclaimed capacity: %v", err)
	}
}

func TestEstablishedCompanionCanRemainIdle(t *testing.T) {
	t.Parallel()
	_, _, address := startTestServer(t, testIdentity(98), testConfig(stateDir(t)))
	client := testClient(t, address)
	if _, err := client.DeviceQuery(testContext(t)); err != nil {
		t.Fatal(err)
	}
	time.Sleep(frameReadTimeout + 100*time.Millisecond)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if _, err := client.DeviceQuery(ctx); err != nil {
		t.Fatalf("normal established companion was disconnected while idle: %v", err)
	}
}
