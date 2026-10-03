package radio

import (
	"bytes"
	"context"
	"encoding/binary"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

func TestQueuedReceiptsCorrelateDuplicatePacketsAndFinalRFResults(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 2)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	conn := <-phy.conns
	packet := []byte{0x11, 0xc0, 0xdb}
	first, err := link.SubmitWithReceipt(packet, 2, 12*time.Millisecond, 30*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	second, err := link.SubmitWithReceipt(packet, 4, 25*time.Millisecond, 40*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	packet[0] = 0xff
	firstWire, secondWire := <-phy.jobs, <-phy.jobs
	if first.Generation != binary.LittleEndian.Uint32(firstWire[1:]) ||
		first.JobID != binary.LittleEndian.Uint32(firstWire[5:]) ||
		second.Generation != binary.LittleEndian.Uint32(secondWire[1:]) ||
		second.JobID != binary.LittleEndian.Uint32(secondWire[5:]) ||
		first.JobID == second.JobID ||
		firstWire[9] != 2 || secondWire[9] != 4 ||
		binary.LittleEndian.Uint32(firstWire[14:]) != 30000 ||
		binary.LittleEndian.Uint32(secondWire[14:]) != 40000 ||
		!bytes.Equal(firstWire[18:], secondWire[18:]) {
		t.Fatalf("receipt identities or queued metadata: first=%+v %x second=%+v %x",
			first, firstWire, second, secondWire)
	}
	select {
	case result := <-first.Result:
		t.Fatalf("acceptance mistaken for RF result: %+v", result)
	default:
	}
	for _, tc := range []struct {
		wire  []byte
		state TXState
		rfMS  uint32
	}{
		{secondWire, TXFailed, 0},
		{firstWire, TXSucceeded, 156},
	} {
		event := make([]byte, 23)
		event[0] = queuedVersion
		copy(event[1:9], tc.wire[1:9])
		event[9] = byte(tc.state)
		binary.LittleEndian.PutUint32(event[15:], tc.rfMS)
		if err := conn.send(hardware.EncodeHardwareFrame(0, hwTXEvent, event)); err != nil {
			t.Fatal(err)
		}
	}
	for _, tc := range []struct {
		receipt TXReceipt
		state   TXState
		rfMS    uint32
	}{
		{first, TXSucceeded, 156},
		{second, TXFailed, 0},
	} {
		select {
		case result := <-tc.receipt.Result:
			if result.State != tc.state || result.JobID != tc.receipt.JobID ||
				result.Generation != tc.receipt.Generation ||
				result.RFAirtime != time.Duration(tc.rfMS)*time.Millisecond ||
				!bytes.Equal(result.PacketBytes(), firstWire[18:]) {
				t.Fatalf("wrong terminal receipt: %+v", result)
			}
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
	}
}

func TestQueuedReceiptBecomesUnknownOnDisconnectWithoutReplay(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 2)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	conn := <-phy.conns
	receipt, err := link.SubmitWithReceipt([]byte{0x11}, 3, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	<-phy.jobs
	conn.Close()
	select {
	case result := <-receipt.Result:
		if result.State != TXUnknown || result.JobID != receipt.JobID ||
			result.Generation != receipt.Generation {
			t.Fatalf("unknown receipt lost job identity: %+v", result)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	select {
	case packet := <-phy.jobs:
		t.Fatalf("uncertain packet replayed: %x", packet)
	default:
	}
}
