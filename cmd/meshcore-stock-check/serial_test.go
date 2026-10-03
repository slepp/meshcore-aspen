package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
)

type serialPipe struct {
	net.Conn
	writes atomic.Int32
}

func (p *serialPipe) Read(data []byte) (int, error) {
	if err := p.SetReadDeadline(time.Now().Add(10 * time.Millisecond)); err != nil {
		return 0, err
	}
	n, err := p.Conn.Read(data)
	var timeout net.Error
	if errors.As(err, &timeout) && timeout.Timeout() {
		return n, nil
	}
	return n, err
}

func (p *serialPipe) Write(data []byte) (int, error) {
	p.writes.Add(1)
	return p.Conn.Write(data[:min(2, len(data))])
}

func serialFixture(t *testing.T) (*stockSerialTransport, net.Conn, *serialPipe, *bytes.Buffer) {
	t.Helper()
	local, peer := net.Pipe()
	port := &serialPipe{Conn: local}
	diagnostics := &bytes.Buffer{}
	transport := newStockSerialTransport("unused-test-path", diagnostics)
	var opens atomic.Int32
	transport.open = func() (io.ReadWriteCloser, error) {
		if opens.Add(1) != 1 {
			return nil, errors.New("unexpected serial reopen")
		}
		return port, nil
	}
	t.Cleanup(func() {
		_ = transport.Close()
		_ = peer.Close()
		if opens.Load() > 1 {
			t.Error("transport reconnected")
		}
	})
	return transport, peer, port, diagnostics
}

func serialDeviceFrame() []byte {
	body := make([]byte, 82)
	copy(body, []byte{13, 13, 32, 8})
	copy(body[8:20], "Sep 23 2026")
	copy(body[20:60], "Seeed Xiao S3 WIO")
	copy(body[60:80], "v1.17.1")
	return append([]byte{'>', 82, 0}, body...)
}

func serialExpectCommand(peer net.Conn, want []byte) error {
	if err := peer.SetDeadline(time.Now().Add(4 * time.Second)); err != nil {
		return err
	}
	got := make([]byte, len(want))
	if _, err := io.ReadFull(peer, got); err != nil {
		return err
	}
	if !bytes.Equal(got, want) {
		return fmt.Errorf("command %x, want %x", got, want)
	}
	return nil
}

func serialFragments(peer net.Conn, data []byte) error {
	for step := 1; len(data) > 0; step = step%11 + 1 {
		n := min(step, len(data))
		if _, err := peer.Write(data[:n]); err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}

func serialAwait[T any](t *testing.T, ch <-chan T) T {
	t.Helper()
	select {
	case result := <-ch:
		return result
	case <-time.After(4 * time.Second):
		t.Fatal("serial fixture did not finish")
		var zero T
		return zero
	}
}

func TestStockSerialInitialResyncAndSDKExchange(t *testing.T) {
	tr, peer, port, diagnostics := serialFixture(t)
	sdk := client.New(tr)
	failures := make(chan error, 1)
	sdk.SetErrorHandler(func(err error) { failures <- err })
	received := make(chan protocol.PushLogRxDataResponse, 1)
	sdk.OnPush(protocol.PushLogRxData, func(r protocol.Response) {
		received <- r.Data.(protocol.PushLogRxDataResponse)
	})
	// An actual RX-log layout, opened in the middle of its payload. Embedded
	// fake headers include both huge lengths and a plausible incomplete frame.
	oldLog := append([]byte{'>', 176, 0, 0x88, 8, 176}, bytes.Repeat([]byte{0xa5}, 173)...)
	copy(oldLog[30:], []byte{'<', 255, 255, '>', 255, 255, '>', 0, 0})
	copy(oldLog[len(oldLog)-6:], []byte{'>', 176, 0, 0x88, 8, 176})
	noise := oldLog[17:]
	if frames := protocol.NewFrameParser().Feed(append(bytes.Clone(noise), serialDeviceFrame()...)); len(frames) != 0 {
		t.Fatal("fixture no longer reproduces the SDK parser's false-length stall")
	}
	raw := bytes.Repeat([]byte{0x3c, 0xff, 0x3e, 0x00}, 44)[:173]
	rxFrame := append([]byte{'>', 176, 0, 0x88, 8, 176}, raw...)
	server := make(chan error, 1)
	go func() {
		if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
			server <- err
			return
		}
		if err := serialFragments(peer, append(bytes.Clone(noise), serialDeviceFrame()...)); err != nil {
			server <- err
			return
		}
		// The SDK's own query must really reach the peer after synchronization.
		if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
			server <- err
			return
		}
		if err := serialFragments(peer, append(serialDeviceFrame(), rxFrame...)); err != nil {
			server <- err
			return
		}
		if err := serialExpectCommand(peer, []byte{'<', 1, 0, 5}); err != nil {
			server <- err
			return
		}
		server <- serialFragments(peer, []byte{'>', 5, 0, 9, 0x78, 0x56, 0x34, 0x12})
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	if err := sdk.Connect(ctx); err != nil {
		t.Fatal(err)
	}
	info, err := sdk.DeviceQuery(ctx)
	if err != nil || info.FirmwareVersion != 13 || info.FirmwareVersionStr != "v1.17.1" {
		t.Fatalf("SDK DeviceQuery: %+v, %v", info, err)
	}
	rx := serialAwait(t, received)
	if !bytes.Equal(rx.Raw, raw) || rx.LastRSSI != -80 || rx.LastSNR != 2 {
		t.Fatalf("native maximum RX frame changed: %+v", rx)
	}
	clock, err := sdk.GetDeviceTime(ctx)
	if err != nil || clock != (protocol.CurrTimeResponse{Timestamp: 0x12345678}) {
		t.Fatalf("SDK time: %+v, %v", clock, err)
	}
	if err := serialAwait(t, server); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(diagnostics.String(), fmt.Sprintf("discarded_bytes=%d ", len(noise))) {
		t.Fatalf("missing exact quarantine count: %q", diagnostics.String())
	}
	if port.writes.Load() < 8 {
		t.Fatal("partial writes were not exercised")
	}
	if err := sdk.Close(); err != nil {
		t.Fatal(err)
	}
	serialAwait(t, tr.dispatchDone)
	select {
	case err := <-failures:
		t.Fatalf("clean shutdown reported failure: %v", err)
	default:
	}
	if err := tr.Connect(ctx); err == nil {
		t.Fatal("closed transport reopened")
	}
}

func TestStockSerialCorruptionAfterReadyIsTerminal(t *testing.T) {
	for _, tc := range []struct {
		name string
		wire []byte
		want string
	}{
		{"outgoing-echo", []byte{'<', 1, 0, 0}, "direction"},
		{"noise", []byte{0x55}, "direction"},
		{"zero", []byte{'>', 0, 0}, "length 0"},
		{"over-native-cap", []byte{'>', 177, 0}, "length 177"},
		{"huge", []byte{'>', 255, 255}, "length 65535"},
		{"bad-response", []byte{'>', 1, 0, 9}, "payload too short"},
		{"unknown-code", []byte{'>', 1, 0, 0xf0}, "unsupported response"},
		{"partial", []byte{'>', 5, 0, 9}, "receive deadline"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			tr, peer, _, _ := serialFixture(t)
			failures := make(chan error, 1)
			disconnected := make(chan struct{}, 1)
			tr.SetErrorHandler(func(err error) { failures <- err })
			tr.SetDisconnectHandler(func() { disconnected <- struct{}{} })
			server := make(chan error, 1)
			go func() {
				if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
					server <- err
					return
				}
				_, err := peer.Write(append(serialDeviceFrame(), tc.wire...))
				server <- err
			}()
			if err := tr.Connect(context.Background()); err != nil {
				t.Fatal(err)
			}
			err := serialAwait(t, failures)
			if !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("failure %v, want %q", err, tc.want)
			}
			serialAwait(t, disconnected)
			serialAwait(t, tr.dispatchDone)
			if err := serialAwait(t, server); err != nil {
				t.Fatal(err)
			}
			if err := tr.Send([]byte{5}); err == nil {
				t.Fatal("accepted a command after corruption")
			}
			if err := tr.Close(); err == nil {
				t.Fatal("Close lost the terminal transport error")
			}
		})
	}
}

func TestStockSerialInitialSyncBudgetAndCancellation(t *testing.T) {
	t.Run("bounded-noise", func(t *testing.T) {
		tr, peer, _, diagnostics := serialFixture(t)
		server := make(chan error, 1)
		go func() {
			if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
				server <- err
				return
			}
			_, err := peer.Write(bytes.Repeat([]byte{'>'}, serialSyncLimit))
			server <- err
		}()
		err := tr.Connect(context.Background())
		if err == nil || !strings.Contains(err.Error(), "byte budget") ||
			!strings.Contains(err.Error(), "discarded_bytes=65536") {
			t.Fatalf("expected bounded initial failure, got %v", err)
		}
		if err := serialAwait(t, server); err != nil {
			t.Fatal(err)
		}
		if diagnostics.Len() != 0 {
			t.Fatal("failed synchronization reported success")
		}
	})
	t.Run("cancel-blocked-initial-write", func(t *testing.T) {
		tr, peer, _, _ := serialFixture(t)
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		result := make(chan error, 1)
		go func() { result <- tr.Connect(ctx) }()
		if err := peer.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
			t.Fatal(err)
		}
		var first [1]byte
		if _, err := peer.Read(first[:]); err != nil || first[0] != '<' {
			t.Fatalf("initial partial write: %x, %v", first, err)
		}
		cancel()
		if err := serialAwait(t, result); !errors.Is(err, context.Canceled) {
			t.Fatalf("cancellation: %v", err)
		}
	})
	t.Run("close-blocked-initial-read", func(t *testing.T) {
		tr, peer, _, _ := serialFixture(t)
		result := make(chan error, 1)
		go func() { result <- tr.Connect(context.Background()) }()
		if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
			t.Fatal(err)
		}
		if err := tr.Close(); err != nil {
			t.Fatal(err)
		}
		if err := serialAwait(t, result); err == nil {
			t.Fatal("closed initial synchronization succeeded")
		}
	})
}

type serialShortWriter struct {
	bytes.Buffer
	zero bool
}

func (w *serialShortWriter) Write(data []byte) (int, error) {
	if w.zero {
		return 0, nil
	}
	return w.Buffer.Write(data[:min(2, len(data))])
}

func TestStockSerialWriteProgressAndHandlerClose(t *testing.T) {
	w := &serialShortWriter{}
	want := []byte{'<', 2, 0, 22, 3}
	if err := serialWriteAll(w, want); err != nil || !bytes.Equal(w.Bytes(), want) {
		t.Fatalf("short write lost bytes: %x, %v", w.Bytes(), err)
	}
	w.zero = true
	if err := serialWriteAll(w, want); !errors.Is(err, io.ErrNoProgress) {
		t.Fatalf("zero write did not fail: %v", err)
	}
	tr, peer, _, _ := serialFixture(t)
	closed := make(chan error, 1)
	tr.SetResponseHandler(func(protocol.Response) { closed <- tr.Close() })
	server := make(chan error, 1)
	go func() {
		if err := serialExpectCommand(peer, want); err != nil {
			server <- err
			return
		}
		_, err := peer.Write(append(serialDeviceFrame(), '>', 1, 0, 0x83))
		server <- err
	}()
	if err := tr.Connect(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := serialAwait(t, closed); err != nil {
		t.Fatal(err)
	}
	serialAwait(t, tr.dispatchDone)
	if err := serialAwait(t, server); err != nil {
		t.Fatal(err)
	}
}

func TestStockSerialKeepsCoalescedFrameAfterAnchor(t *testing.T) {
	tr, peer, _, _ := serialFixture(t)
	response := make(chan protocol.Response, 1)
	tr.SetResponseHandler(func(r protocol.Response) { response <- r })
	go func() {
		if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
			t.Error(err)
			return
		}
		frame := append(serialDeviceFrame(), '>', 5, 0, 9)
		frame = binary.LittleEndian.AppendUint32(frame, 12345)
		if _, err := peer.Write(frame); err != nil {
			t.Error(err)
		}
	}()
	if err := tr.Connect(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := serialAwait(t, response); got.Data != (protocol.CurrTimeResponse{Timestamp: 12345}) {
		t.Fatalf("coalesced reply lost: %+v", got)
	}
}

func serialConnectFixture(t *testing.T, tr *stockSerialTransport, peer net.Conn) {
	t.Helper()
	server := make(chan error, 1)
	go func() {
		if err := serialExpectCommand(peer, []byte{'<', 2, 0, 22, 3}); err != nil {
			server <- err
			return
		}
		_, err := peer.Write(serialDeviceFrame())
		server <- err
	}()
	if err := tr.Connect(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := serialAwait(t, server); err != nil {
		t.Fatal(err)
	}
}

func TestStockSerialBoundedWritesAndShutdown(t *testing.T) {
	for _, expire := range []bool{false, true} {
		t.Run(fmt.Sprintf("write-deadline-%t", expire), func(t *testing.T) {
			tr, peer, _, _ := serialFixture(t)
			failures := make(chan error, 1)
			tr.SetErrorHandler(func(err error) { failures <- err })
			serialConnectFixture(t, tr, peer)
			if err := tr.Send([]byte{5}); err != nil {
				t.Fatal(err)
			}
			var first [1]byte
			if _, err := peer.Read(first[:]); err != nil || first[0] != '<' {
				t.Fatalf("partial command: %x, %v", first, err)
			}
			// The worker is blocked writing its second byte, not draining this queue.
			for range 16 {
				if err := tr.Send([]byte{5}); err != nil {
					t.Fatal(err)
				}
			}
			if err := tr.Send([]byte{5}); err == nil || !strings.Contains(err.Error(), "queue full") {
				t.Fatalf("overflow did not reject command: %v", err)
			}
			if expire {
				if err := serialAwait(t, failures); !strings.Contains(err.Error(), "write deadline") {
					t.Fatalf("blocked write failure: %v", err)
				}
			}
			err := tr.Close()
			if (err != nil) != expire {
				t.Fatalf("Close: %v", err)
			}
			serialAwait(t, tr.dispatchDone)
		})
	}
}

func TestStockSerialInboundOverflowFailsEvidence(t *testing.T) {
	tr, peer, _, _ := serialFixture(t)
	entered := make(chan struct{})
	handlerCtx, release := context.WithCancel(context.Background())
	defer release()
	failures := make(chan error, 1)
	tr.SetResponseHandler(func(protocol.Response) {
		close(entered)
		<-handlerCtx.Done()
	})
	tr.SetErrorHandler(func(err error) { failures <- err })
	serialConnectFixture(t, tr, peer)
	if _, err := peer.Write([]byte{'>', 1, 0, 0x83}); err != nil {
		t.Fatal(err)
	}
	serialAwait(t, entered)
	server := make(chan error, 1)
	go func() {
		_, err := peer.Write(bytes.Repeat([]byte{'>', 1, 0, 0x83}, 1025))
		server <- err
	}()
	serialAwait(t, tr.done)
	release()
	if err := serialAwait(t, failures); !strings.Contains(err.Error(), "receive queue full; responses lost") {
		t.Fatalf("lost evidence without terminal overflow: %v", err)
	}
	if err := serialAwait(t, server); err != nil && !errors.Is(err, io.ErrClosedPipe) {
		t.Fatal(err)
	}
	serialAwait(t, tr.dispatchDone)
}
