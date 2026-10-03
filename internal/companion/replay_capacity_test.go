package companion

import (
	"bytes"
	"testing"

	protocol "github.com/meshcore-go/meshcore-go/companion"
)

func TestReplayCapacityReclaimsOnlyFetchedDirectMessages(t *testing.T) {
	for _, tc := range []struct {
		name      string
		retention RetentionProfile
		cursors   []uint64
		admit     bool
	}{
		{"all readers fetched", DurableReplay, []uint64{maxMessages, maxMessages}, true},
		{"one reader fetched oldest", DurableReplay, []uint64{maxMessages, 1}, true},
		{"lagging reader has unread oldest", DurableReplay, []uint64{maxMessages, 0}, false},
		{"offline inbox", DurableReplay, nil, false},
		{"native unread inbox", NativeQueue, []uint64{0}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := &Server{state: state{Retention: tc.retention}, clients: make(map[*session]struct{})}
			s.cfg.ErrorHandler = func(error) {}
			for i := uint32(1); i <= maxMessages; i++ {
				if !s.queueMessage(privateFrame(i)) {
					t.Fatal("initial inbox fill rejected")
				}
			}
			for _, cursor := range tc.cursors {
				s.clients[&session{cursor: cursor, done: make(chan struct{}), out: make(chan []byte, 1)}] = struct{}{}
			}
			before := append([]message(nil), s.state.Messages...)
			if got := s.queueMessage(privateFrame(300)); got != tc.admit {
				t.Fatalf("admission=%v, want %v", got, tc.admit)
			}
			if len(s.state.Messages) != maxMessages {
				t.Fatal("response capacity changed")
			}
			if !tc.admit {
				if s.state.Sequence != maxMessages {
					t.Fatal("rejected response advanced sequence")
				}
				for i, m := range s.state.Messages {
					if !bytes.Equal(m.Frame, before[i].Frame) || m.Sequence != before[i].Sequence {
						t.Fatal("rejected response discarded an unread message")
					}
				}
				return
			}
			if s.state.Messages[0].Sequence != 2 || s.state.Sequence != maxMessages+1 {
				t.Fatal("did not reclaim exactly the oldest fetched message")
			}
			if !bytes.Equal(s.state.Messages[maxMessages-1].Frame, privateFrame(300)) {
				t.Fatal("new response was not retained")
			}
			for c := range s.clients {
				if frame := <-c.out; !bytes.Equal(frame, []byte{protocol.PushMsgWaiting}) {
					t.Fatalf("new response notification=%x", frame)
				}
			}
			if !s.queueMessage(privateFrame(300)) || s.state.Sequence != maxMessages+1 ||
				s.state.Messages[0].Sequence != 2 {
				t.Fatal("duplicate response reclaimed another record")
			}
			for c := range s.clients {
				if len(c.out) != 0 {
					t.Fatal("duplicate response produced another notification")
				}
			}
		})
	}
}

func TestFetchedReplayReclamationSurvivesRestart(t *testing.T) {
	id := testIdentity(1)
	cfg := testConfig(stateDir(t))
	s, _, address := startTestServer(t, id, cfg)
	c := testClient(t, address)
	ctx := testContext(t)
	if _, err := c.DeviceQuery(ctx); err != nil {
		t.Fatal(err)
	}
	s.mu.Lock()
	for i := uint32(1); i <= maxMessages; i++ {
		if !s.queueMessage(privateFrame(i)) {
			s.mu.Unlock()
			t.Fatal("persistent inbox fill rejected")
		}
	}
	s.mu.Unlock()
	if messages, err := c.GetWaitingMessages(ctx); err != nil || len(messages) != maxMessages {
		t.Fatalf("fetch persistent replay: count=%d error=%v", len(messages), err)
	}
	s.mu.Lock()
	admitted := s.queueMessage(privateFrame(300))
	s.mu.Unlock()
	if !admitted {
		t.Fatal("fetched persistent replay blocked new response")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	reloaded, _, _ := startTestServer(t, id, cfg)
	reloaded.mu.Lock()
	defer reloaded.mu.Unlock()
	if len(reloaded.state.Messages) != maxMessages || reloaded.state.Sequence != maxMessages+1 ||
		reloaded.state.Messages[0].Sequence != 2 ||
		!bytes.Equal(reloaded.state.Messages[maxMessages-1].Frame, privateFrame(300)) {
		t.Fatal("restart did not preserve reclaimed replay and new response")
	}
	if reloaded.queueMessage(privateFrame(301)) {
		t.Fatal("restart treated disconnected readers as acknowledgement of retained messages")
	}
}
