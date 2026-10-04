package kissproxy

import (
	"bytes"
	"io"
	"net"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

type deadlineConnection struct {
	net.Conn
	deadlines chan time.Time
}

func (c *deadlineConnection) SetReadDeadline(deadline time.Time) error {
	if err := c.Conn.SetReadDeadline(deadline); err != nil {
		return err
	}
	c.deadlines <- deadline
	return nil
}

func TestFrameDeadlineTracksBodiesWithoutExtendingForFragments(t *testing.T) {
	reader, writer := net.Pipe()
	conn := &deadlineConnection{Conn: reader, deadlines: make(chan time.Time, 8)}
	frames := make(chan *hardware.KissFrame, 3)
	done := make(chan error, 1)
	go func() {
		done <- readFrames(conn, func(frame *hardware.KissFrame) error {
			frames <- frame
			return nil
		})
	}()
	t.Cleanup(func() {
		writer.Close()
		select {
		case err := <-done:
			if err != io.EOF {
				t.Errorf("frame reader: %v", err)
			}
		case <-time.After(time.Second):
			t.Error("frame reader did not stop")
		}
		reader.Close()
	})
	send := func(data []byte) time.Time {
		t.Helper()
		if err := write(writer, data); err != nil {
			t.Fatal(err)
		}
		select {
		case deadline := <-conn.deadlines:
			return deadline
		case <-time.After(time.Second):
			t.Fatal("frame reader did not update its deadline")
			return time.Time{}
		}
	}
	checkFrame := func() {
		t.Helper()
		select {
		case frame := <-frames:
			if frame.Port != 0 || frame.Command != hardware.KISS_CMD_SETHARDWARE ||
				!bytes.Equal(frame.Data, []byte{hardware.HW_CMD_HASH, 0xc0, 0xdb}) {
				t.Fatalf("fragmented escaped frame changed: %+v", frame)
			}
		case <-time.After(time.Second):
			t.Fatal("completed frame not delivered")
		}
	}
	wire := hardware.EncodeHardwareFrame(0, hardware.HW_CMD_HASH, []byte{0xc0, 0xdb})
	if deadline := send(wire[:1]); !deadline.IsZero() {
		t.Fatal("inter-frame FEND started a timeout")
	}
	deadline := send(wire[1:3])
	if deadline.IsZero() {
		t.Fatal("partial frame has no completion deadline")
	}
	if got := send(wire[3:4]); got != deadline {
		t.Fatal("an escape fragment extended the completion deadline")
	}
	if got := send(wire[4:]); !got.IsZero() {
		t.Fatal("completed frame kept its deadline")
	}
	checkFrame()

	deadline = send(wire[:3])
	next := append(bytes.Clone(wire[3:]), wire[:3]...)
	if got := send(next); !got.After(deadline) {
		t.Fatal("new partial frame inherited the completed frame's deadline")
	}
	checkFrame()
	if got := send(wire[3:]); !got.IsZero() {
		t.Fatal("second completed frame kept its deadline")
	}
	checkFrame()
}
