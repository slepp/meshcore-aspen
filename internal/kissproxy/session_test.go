package kissproxy

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
)

func TestIncompleteClientFrameExpiresWithoutDisconnectingIdleClient(t *testing.T) {
	upstream, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { upstream.Close() })
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	identity := meshcore.NewLocalIdentityFromSeed([32]byte{1})
	server := New(identity, Config{
		Upstream: upstream.Addr().String(), MaxClients: 2,
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- server.Serve(ctx, listener) }()
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Error(err)
			}
		case <-time.After(time.Second):
			t.Error("proxy did not shut down")
		}
	})
	connect := func() net.Conn {
		t.Helper()
		conn, err := net.DialTimeout("tcp", listener.Addr().String(), time.Second)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { conn.Close() })
		if err := upstream.(*net.TCPListener).SetDeadline(time.Now().Add(time.Second)); err != nil {
			t.Fatal(err)
		}
		physical, err := upstream.Accept()
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { physical.Close() })
		return conn
	}
	checkIdentity := func(conn net.Conn) {
		t.Helper()
		if err := conn.SetDeadline(time.Now().Add(time.Second)); err != nil {
			t.Fatal(err)
		}
		if err := write(conn, hardware.EncodeHardwareFrame(0, hardware.HW_CMD_GET_IDENTITY, nil)); err != nil {
			t.Fatal(err)
		}
		want := hardware.EncodeHardwareFrame(0, hardware.HwResp(hardware.HW_CMD_GET_IDENTITY), identity.PublicKeyBytes())
		got := make([]byte, len(want))
		if _, err := io.ReadFull(conn, got); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, want) {
			t.Fatalf("wrong virtual identity: %x", got)
		}
	}
	stalled, idle := connect(), connect()
	checkIdentity(stalled)
	checkIdentity(idle)

	partial := hardware.EncodeHardwareFrame(0, hardware.HW_CMD_GET_RANDOM, []byte{1})
	if err := write(stalled, partial[:len(partial)-1]); err != nil {
		t.Fatal(err)
	}
	if err := stalled.SetReadDeadline(time.Now().Add(7 * time.Second)); err != nil {
		t.Fatal(err)
	}
	if _, err := stalled.Read(make([]byte, 1)); !errors.Is(err, io.EOF) {
		t.Fatalf("incomplete control kept its session instead of closing: %v", err)
	}
	checkIdentity(idle)
}
