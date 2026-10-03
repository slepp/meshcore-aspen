package radio

import (
	"context"
	"errors"
	"log/slog"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/node"
)

// A submission racing a changed readback must not be sent while the new
// profile is only read (the modem's stale fence already cleared) but not yet
// adopted with its airtime model.
func TestFollowRetuneFencesSubmissionsUntilAirtimeModelIsAdopted(t *testing.T) {
	phy := followPHY(t)
	blocked, gate := make(chan struct{}), make(chan struct{})
	release := sync.OnceFunc(func() { close(gate) })
	t.Cleanup(release)
	base := phy.airtimeResponse
	phy.airtimeResponse = func(length byte) []byte {
		phy.mu.Lock()
		retuned := phy.profile[8] == 9
		phy.mu.Unlock()
		if retuned && length == 0 {
			close(blocked)
			<-gate
		}
		return base(length)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	link, err := Open(ctx, followConfig(phy, NewPHYGroup()))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	<-phy.conns
	drain(phy.airtimeRequests)
	results := make(chan TXResult, 8)
	link.AddTXResultHandler(func(result TXResult) { results <- result })
	radio, stop := link.Radio()
	defer stop()
	tx := radio.(node.TxRadio)

	phy.retune(t, func(p []byte) { p[8] = 9 })
	link.requestPHYRefresh()
	select {
	case <-blocked:
	case <-time.After(5 * time.Second):
		t.Fatal("retune readback did not start rebuilding the airtime model")
	}
	// CONFIG GET has cleared the mast's fence; the host must still fence.
	if phy.configReads.Load() < 2 {
		t.Fatal("changed CONFIG was not read before the airtime rebuild")
	}
	done := make(chan error, 1)
	go func() {
		_, err := link.submit([]byte{0x22}, 1, 0, 0)
		done <- err
	}()
	select {
	case err := <-done:
		if !errors.Is(err, ErrPHYSettling) || !errors.Is(err, ErrOffline) {
			t.Fatalf("submission during settle: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("submission blocked behind the airtime rebuild")
	}
	if tx.Enqueue([]byte{0x23}, 1, 0) {
		t.Fatal("node enqueue admitted during settle")
	}
	noMoreJobs(t, phy)
	if state, _ := effective(link); state.ConfigurationGeneration != 1 || !link.Online() {
		t.Fatalf("profile adopted before its airtime model: %+v", state)
	}

	release()
	waitUntil(t, "retune adoption", func() bool {
		state, ok := effective(link)
		return ok && state.ConfigurationGeneration == 2
	})
	if estimate := link.AirtimeEstimator()(10); estimate != 9010 {
		t.Fatalf("adopted with stale airtime model: %d", estimate)
	}
	if !tx.Enqueue([]byte{0x33}, 1, 0) {
		t.Fatal("enqueue after adoption")
	}
	nextJob(t, phy, []byte{0x33})
	if result := nextResult(t, results); result.State != TXAccepted {
		t.Fatalf("post-adoption submission: %+v", result)
	}
	noMoreJobs(t, phy)
}

// blockingHandler holds the connection loop at its readback-failure log line,
// between the mismatched CONFIG GET and the connection being retired.
type blockingHandler struct {
	slog.Handler
	match   string
	reached chan struct{}
	release chan struct{}
}

func (h *blockingHandler) Handle(ctx context.Context, record slog.Record) error {
	if strings.Contains(record.Message, h.match) {
		select {
		case <-h.reached:
		default:
			close(h.reached)
			<-h.release
		}
	}
	return nil
}
func (h *blockingHandler) Enabled(context.Context, slog.Level) bool { return true }
func (h *blockingHandler) WithAttrs([]slog.Attr) slog.Handler       { return h }
func (h *blockingHandler) WithGroup(string) slog.Handler            { return h }

func TestFixedRetuneFencesSubmissionsUntilDisconnect(t *testing.T) {
	phy := followPHY(t)
	handler := &blockingHandler{
		Handler: slog.DiscardHandler, match: "shared PHY readback failed",
		reached: make(chan struct{}), release: make(chan struct{}),
	}
	release := sync.OnceFunc(func() { close(handler.release) })
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	cfg := followConfig(phy, NewPHYGroup())
	cfg.PHYTracking, cfg.Radio, cfg.TxPower = PHYFixed, mastSettings.Radio, mastSettings.TxPower
	cfg.Logger = slog.New(handler)
	link, err := Open(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	// Unblock the connection loop before Close waits for it.
	defer release()
	<-phy.conns

	phy.retune(t, func(p []byte) { p[8] = 9 })
	link.requestPHYRefresh()
	select {
	case <-handler.reached:
	case <-time.After(5 * time.Second):
		t.Fatal("mismatched readback was not reported")
	}
	// The mast has accepted this connection's CONFIG GET for the mismatched
	// profile, and the connection is still up.
	if !link.Online() {
		t.Fatal("connection retired before the race window")
	}
	if _, err := link.submit([]byte{0x22}, 1, 0, 0); !errors.Is(err, ErrPHYSettling) {
		t.Fatalf("submission on mismatched fixed profile: %v", err)
	}
	noMoreJobs(t, phy)
	release()
	waitUntil(t, "mismatch disconnect", func() bool { return !link.Online() })
	if _, err := link.submit([]byte{0x33}, 1, 0, 0); !errors.Is(err, ErrOffline) {
		t.Fatalf("submission after disconnect: %v", err)
	}
	noMoreJobs(t, phy)
}
