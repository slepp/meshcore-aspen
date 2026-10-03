package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"

	protocol "github.com/meshcore-go/meshcore-go/companion"
	"go.bug.st/serial"
)

const (
	serialSyncTimeout  = 5 * time.Second
	serialFrameTimeout = 2 * time.Second
	serialSyncLimit    = 64 * 1024
	serialDeviceSize   = 82 // MeshCore companion protocol 13 DeviceInfo, including response code.
)

var (
	errStockSerialClosed = errors.New("serial connection closed")
	errSerialReadIdle    = errors.New("serial read timeout")
)

// The SDK's serial dial/parser are private and its transport automatically
// reconnects. This adapter reuses its protocol codecs, not that connection loop.
type stockSerialTransport struct {
	mu           sync.Mutex
	open         func() (io.ReadWriteCloser, error)
	diagnostics  io.Writer
	conn         io.ReadWriteCloser
	started      bool
	ready        bool
	closed       bool
	failure      error
	closeErr     error
	done         chan struct{}
	closeDone    chan struct{}
	dispatchDone chan struct{}
	ioWorkers    sync.WaitGroup
	outbound     chan []byte
	inbound      chan protocol.Response
	onResponse   func(protocol.Response)
	onError      func(error)
	onDisconnect func()
}

var _ managedTransport = (*stockSerialTransport)(nil)

// Connect performs one DeviceQuery to establish framing before the check. Its
// reply is consumed before the SDK's subsequent query. Diagnostics receives
// the number of bytes discarded during synchronization.
func newStockSerialTransport(path string, diagnostics io.Writer) *stockSerialTransport {
	return &stockSerialTransport{
		open: func() (io.ReadWriteCloser, error) {
			port, err := serial.Open(path, &serial.Mode{BaudRate: 115200})
			if err != nil {
				return nil, fmt.Errorf("open companion serial port: %w", err)
			}
			if err := port.SetReadTimeout(100 * time.Millisecond); err != nil {
				return nil, errors.Join(fmt.Errorf("serial read timeout: %w", err), port.Close())
			}
			return port, nil
		},
		diagnostics: diagnostics,
		done:        make(chan struct{}), closeDone: make(chan struct{}), dispatchDone: make(chan struct{}),
		outbound: make(chan []byte, 16), inbound: make(chan protocol.Response, 1024),
	}
}

func (t *stockSerialTransport) SetResponseHandler(h func(protocol.Response)) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.onResponse = h
}

func (t *stockSerialTransport) SetErrorHandler(h func(error)) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.onError = h
}

func (t *stockSerialTransport) SetDisconnectHandler(h func()) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.onDisconnect = h
}

func (t *stockSerialTransport) Connect(ctx context.Context) error {
	t.mu.Lock()
	if t.closed || t.started {
		t.mu.Unlock()
		return errors.New("serial connection already used; start a new RF check")
	}
	t.started = true
	t.ioWorkers.Add(1)
	t.mu.Unlock()
	defer t.ioWorkers.Done()

	syncCtx, cancel := context.WithTimeout(ctx, serialSyncTimeout)
	defer cancel()
	if err := syncCtx.Err(); err != nil {
		t.stop(err)
		return err
	}
	if t.diagnostics == nil {
		err := errors.New("serial synchronization requires diagnostics")
		t.stop(err)
		return err
	}
	conn, err := t.open()
	if err != nil {
		t.stop(err)
		return err
	}
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return errors.Join(errStockSerialClosed, conn.Close())
	}
	t.conn = conn
	t.mu.Unlock()
	stopTimeout := context.AfterFunc(syncCtx, func() { t.stop(syncCtx.Err()) })
	defer stopTimeout()

	reader := bufio.NewReaderSize(serialTimeoutReader{conn}, 256)
	discarded, err := t.synchronize(reader, conn)
	if err == nil {
		_, err = fmt.Fprintf(t.diagnostics,
			"companion serial connected: discarded_bytes=%d frame_limit=%d\n",
			discarded, protocol.MaxFrameSize)
	}
	if syncCtx.Err() != nil {
		err = syncCtx.Err()
	}
	if err != nil {
		err = fmt.Errorf("serial synchronization (discarded_bytes=%d): %w", discarded, err)
		t.stop(err)
		return err
	}
	stopTimeout()
	t.mu.Lock()
	if t.closed || syncCtx.Err() != nil {
		t.mu.Unlock()
		t.stop(syncCtx.Err())
		return errors.Join(errStockSerialClosed, syncCtx.Err())
	}
	t.ready = true
	t.ioWorkers.Add(2)
	t.mu.Unlock()
	go func() {
		defer t.ioWorkers.Done()
		if err := t.readLoop(reader); err != nil {
			t.stop(err)
		}
	}()
	go func() {
		defer t.ioWorkers.Done()
		if err := t.writeLoop(conn); err != nil {
			t.stop(err)
		}
	}()
	go t.dispatchLoop()
	return nil
}

func (t *stockSerialTransport) stop(cause error) {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	t.closed, t.ready, t.failure = true, false, cause
	conn := t.conn
	close(t.done)
	t.mu.Unlock()
	var err error
	if conn != nil {
		err = conn.Close()
	}
	t.mu.Lock()
	t.closeErr = err
	t.mu.Unlock()
	close(t.closeDone)
}

func (t *stockSerialTransport) Close() error {
	t.stop(nil)
	<-t.closeDone
	t.ioWorkers.Wait()
	t.mu.Lock()
	defer t.mu.Unlock()
	return errors.Join(t.failure, t.closeErr)
}

func (t *stockSerialTransport) Send(command []byte) error {
	frame, err := protocol.FrameEncode(protocol.FrameTypeOutgoing, command)
	if err != nil {
		return err
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	if !t.ready || t.closed {
		return errStockSerialClosed
	}
	select {
	case t.outbound <- frame:
		return nil
	default:
		return errors.New("serial transmit queue full; command not accepted")
	}
}

func (t *stockSerialTransport) writeLoop(conn io.Writer) error {
	for {
		select {
		case <-t.done:
			return nil
		case frame := <-t.outbound:
			select {
			case <-t.done:
				return nil
			default:
			}
			timer := time.AfterFunc(serialFrameTimeout, func() {
				t.stop(errors.New("serial write deadline exceeded; command outcome unknown"))
			})
			err := serialWriteAll(conn, frame)
			timer.Stop()
			if err != nil {
				return fmt.Errorf("serial write: %w", err)
			}
		}
	}
}

func serialWriteAll(writer io.Writer, data []byte) error {
	for len(data) != 0 {
		n, err := writer.Write(data)
		if n < 0 || n > len(data) {
			return errors.New("serial writer returned an invalid byte count")
		}
		data = data[n:]
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrNoProgress
		}
	}
	return nil
}

type serialTimeoutReader struct{ io.Reader }

func (r serialTimeoutReader) Read(data []byte) (int, error) {
	n, err := r.Reader.Read(data)
	if n == 0 && err == nil {
		err = errSerialReadIdle
	}
	return n, err
}

func (t *stockSerialTransport) readByte(reader *bufio.Reader, deadline time.Time) (byte, error) {
	for {
		select {
		case <-t.done:
			return 0, errStockSerialClosed
		default:
		}
		if !deadline.IsZero() && !time.Now().Before(deadline) {
			return 0, errors.New("incomplete serial frame exceeded receive deadline")
		}
		b, err := reader.ReadByte()
		if errors.Is(err, errSerialReadIdle) {
			continue
		}
		return b, err
	}
}

// A bounded sliding window cannot become captive to a false '<' or a plausible
// but incomplete '>' header in an old RX-log payload. Only the stock DeviceInfo
// layout anchors initial framing. Everything before it is explicitly quarantined.
func (t *stockSerialTransport) synchronize(reader *bufio.Reader, writer io.Writer) (int, error) {
	query := protocol.DeviceQueryCommand{AppTargetVersion: protocol.SupportedProtocolVersion}
	frame, err := protocol.FrameEncode(protocol.FrameTypeOutgoing, query.ToBytes())
	if err != nil {
		return 0, err
	}
	if err := serialWriteAll(writer, frame); err != nil {
		return 0, err
	}
	var window [serialDeviceSize + protocol.FrameHeaderSize]byte
	for count := 0; count < serialSyncLimit; {
		b, err := t.readByte(reader, time.Time{})
		if err != nil {
			return count, err
		}
		copy(window[:], window[1:])
		window[len(window)-1] = b
		count++
		if count < len(window) || !bytes.Equal(window[:4], []byte{'>', serialDeviceSize, 0, protocol.RespDeviceInfo}) {
			continue
		}
		body := window[protocol.FrameHeaderSize:]
		if !serialCString(body[8:20]) || !serialCString(body[20:60]) || !serialCString(body[60:80]) {
			continue
		}
		if _, err := protocol.ParseResponse(body); err != nil {
			continue
		}
		if body[1] != 13 {
			return count, fmt.Errorf("unsupported companion protocol %d; this check requires 13", body[1])
		}
		return count - len(window), nil
	}
	return serialSyncLimit, errors.New("initial serial noise exceeded byte budget")
}

func serialCString(field []byte) bool {
	end := bytes.IndexByte(field, 0)
	if end <= 0 {
		return false
	}
	for _, b := range field[:end] {
		if b < 0x20 || b > 0x7e {
			return false
		}
	}
	return true
}

func (t *stockSerialTransport) readLoop(reader *bufio.Reader) error {
	for {
		first, err := t.readByte(reader, time.Time{})
		if err != nil {
			return fmt.Errorf("serial receive: %w", err)
		}
		if first != protocol.FrameTypeIncoming {
			return fmt.Errorf("serial frame direction invalid: 0x%02x", first)
		}
		deadline := time.Now().Add(serialFrameTimeout)
		var header [2]byte
		for i := range header {
			header[i], err = t.readByte(reader, deadline)
			if err != nil {
				return err
			}
		}
		size := int(binary.LittleEndian.Uint16(header[:]))
		if size < 1 || size > protocol.MaxFrameSize {
			return fmt.Errorf("serial frame length %d exceeds 1..%d", size, protocol.MaxFrameSize)
		}
		body := make([]byte, size)
		for i := range body {
			body[i], err = t.readByte(reader, deadline)
			if err != nil {
				return err
			}
		}
		if code := body[0]; code > protocol.RespDefaultFloodScope && (code < protocol.PushAdvert || code > protocol.PushContactsFull) {
			return fmt.Errorf("unsupported response code 0x%02x", code)
		}
		response, err := protocol.ParseResponse(body)
		if err != nil {
			return fmt.Errorf("companion response: %w", err)
		}
		select {
		case <-t.done:
			return nil
		case t.inbound <- response:
		default:
			return errors.New("serial receive queue full; responses lost")
		}
	}
}

// Dispatch is separate from I/O, so handlers may Close without waiting on
// themselves. Handlers must return promptly; shutdown discards queued responses.
func (t *stockSerialTransport) dispatchLoop() {
	defer close(t.dispatchDone)
	for {
		select {
		case <-t.done:
			t.mu.Lock()
			err, onError, onDisconnect := t.failure, t.onError, t.onDisconnect
			t.mu.Unlock()
			if err != nil {
				if onError != nil {
					onError(err)
				}
				if onDisconnect != nil {
					onDisconnect()
				}
			}
			return
		case response := <-t.inbound:
			t.mu.Lock()
			handler, closed := t.onResponse, t.closed
			t.mu.Unlock()
			if handler != nil && !closed {
				handler(response)
			}
		}
	}
}
