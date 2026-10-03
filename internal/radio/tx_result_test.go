package radio

import (
	"bytes"
	"context"
	"encoding/binary"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
)

func TestTXResultPacketCorrelationAndCopyIsolation(t *testing.T) {
	for _, tc := range []struct {
		name       string
		state      TXState
		disconnect bool
	}{
		{"rejected", TXRejected, false},
		{"succeeded", TXSucceeded, false},
		{"failed", TXFailed, false},
		{"rf-unknown", TXUnknown, false},
		{"disconnected", TXUnknown, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			phy := newTestPHY(t)
			phy.parity, phy.jobs = true, make(chan []byte, 1)
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			link, err := Open(ctx, queuedConfig(phy, true))
			if err != nil {
				t.Fatal(err)
			}
			defer link.Close()
			conn := <-phy.conns
			radio, stop := link.Radio()
			defer stop()
			results := make(chan TXResult, 2)
			sent := make(chan []byte, 1)
			link.AddTXResultHandler(func(result TXResult) {
				packet := result.PacketBytes()
				if len(packet) > 0 {
					packet[0] ^= 0xff
				}
			})
			link.AddTXResultHandler(func(result TXResult) { results <- result })
			link.AddOutboundHandler(func(packet []byte) { packet[0] ^= 0xff })
			link.AddOutboundHandler(func(packet []byte) { sent <- packet })
			original := []byte{0x11, 0xc0, 0xdb}
			packet := bytes.Clone(original)
			if !radio.(node.TxRadio).Enqueue(packet, 1, 0) {
				t.Fatal("enqueue failed")
			}
			packet[0] = 0xff
			var submitted []byte
			select {
			case submitted = <-phy.jobs:
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			var accepted TXResult
			select {
			case accepted = <-results:
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			if accepted.State != TXAccepted || !bytes.Equal(accepted.PacketBytes(), original) {
				t.Fatalf("acceptance lost packet: state=%d packet=%x", accepted.State, accepted.PacketBytes())
			}
			select {
			case <-sent:
				t.Fatal("acceptance reported RF success")
			default:
			}
			if tc.disconnect {
				conn.Close()
			} else {
				event := make([]byte, 23)
				event[0] = queuedVersion
				copy(event[1:9], submitted[1:9])
				event[9] = byte(tc.state)
				if err := conn.send(hardware.EncodeHardwareFrame(0, hwTXEvent, event)); err != nil {
					t.Fatal(err)
				}
			}
			var terminal TXResult
			select {
			case terminal = <-results:
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			if terminal.State != tc.state ||
				terminal.Generation != binary.LittleEndian.Uint32(submitted[1:]) ||
				terminal.JobID != binary.LittleEndian.Uint32(submitted[5:]) ||
				!bytes.Equal(terminal.PacketBytes(), original) {
				t.Fatalf("terminal lost correlation: state=%d id=%d packet=%x",
					terminal.State, terminal.JobID, terminal.PacketBytes())
			}
			copy := terminal.PacketBytes()
			copy[0] = 0xff
			if !bytes.Equal(terminal.PacketBytes(), original) || !bytes.Equal(accepted.PacketBytes(), original) {
				t.Fatal("packet copy mutation escaped into stored results")
			}
			if tc.state == TXSucceeded {
				select {
				case packet := <-sent:
					if !bytes.Equal(packet, original) {
						t.Fatalf("success handlers shared mutable packet: %x", packet)
					}
				case <-ctx.Done():
					t.Fatal(ctx.Err())
				}
			} else {
				select {
				case <-sent:
					t.Fatal("unsuccessful terminal outcome reported RF success")
				default:
				}
			}
		})
	}
}
