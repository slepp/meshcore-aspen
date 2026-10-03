package radio

import (
	"bytes"
	"context"
	"testing"
	"time"
)

func TestCancellationInterruptsHeartbeatBehindStalledControl(t *testing.T) {
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 1)
	phy.statsRequests = make(chan struct{}, 1)
	ctx, cancel := context.WithCancel(context.Background())
	requestCtx, cancelRequest := context.WithCancel(context.Background())
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		cancel()
		cancelRequest()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cancelRequest()
		cancel()
		link.Close()
	})
	results := make(chan TXResult, 2)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	packet := []byte{0x15, 0, 1, 2}
	if _, err := link.submit(packet, 0, 0, 0); err != nil {
		t.Fatal(err)
	}
	select {
	case result := <-results:
		if result.State != TXAccepted {
			t.Fatalf("submission was not accepted: %+v", result)
		}
	case <-time.After(time.Second):
		t.Fatal("missing submission acceptance")
	}
	<-phy.jobs

	requestDone := make(chan error, 1)
	go func() {
		_, err := link.PHYStatistics(requestCtx)
		requestDone <- err
	}()
	select {
	case <-phy.statsRequests:
	case <-time.After(time.Second):
		t.Fatal("statistics request did not reach PHY")
	}
	// The statistics request holds the modem's request slot. Wait until the
	// heartbeat holds the link lock while waiting for that same slot.
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	timeout := time.NewTimer(18 * time.Second)
	defer timeout.Stop()
	for link.request.TryLock() {
		link.request.Unlock()
		select {
		case <-ticker.C:
		case <-timeout.C:
			t.Fatal("heartbeat did not start")
		}
	}
	cancel()
	select {
	case <-link.done:
	case <-time.After(time.Second):
		t.Fatal("link cancellation could not interrupt heartbeat behind stalled control")
	}
	select {
	case err := <-requestDone:
		if err == nil {
			t.Fatal("stalled statistics request succeeded on shutdown")
		}
	case <-time.After(time.Second):
		t.Fatal("stalled statistics request survived link shutdown")
	}
	select {
	case result := <-results:
		if result.State != TXUnknown || !bytes.Equal(result.PacketBytes(), packet) {
			t.Fatalf("shutdown lost uncertain transmission: %+v", result)
		}
	case <-time.After(time.Second):
		t.Fatal("shutdown did not resolve pending transmission")
	}
	if link.Online() {
		t.Fatal("cancelled link remains online")
	}
	select {
	case job := <-phy.jobs:
		t.Fatalf("shutdown replayed uncertain transmission: %x", job)
	default:
	}
}
