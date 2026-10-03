package companion

import (
	"bytes"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
)

func loginTestContact(t *testing.T, s *Server, peer meshcore.LocalIdentity, kind byte) *contact {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	s.state.Contacts = append(s.state.Contacts, contact{ContactResponse: protocol.ContactResponse{
		PublicKey: peer.PublicKey(), Type: kind, OutPathLen: 0,
	}, SyncSince: 42})
	s.hydrate()
	return s.findContact(peer.PublicKeyBytes())
}

func loginTestSend(t *testing.T, c *session, r *testRadio, peer meshcore.LocalIdentity, kind byte, data []byte) uint32 {
	t.Helper()
	cmd := []byte{kind}
	if kind == protocol.CmdSendTelemetryReq {
		cmd = append(cmd, 0, 0, 0)
	}
	cmd = append(cmd, peer.PublicKeyBytes()...)
	frame := sessionCommand(t, c, append(cmd, data...))
	if len(frame) != 10 || frame[0] != protocol.RespSent {
		t.Fatalf("request was not admitted: %x", frame)
	}
	pkt := r.packet(t)
	secret, err := peer.SharedSecret(c.server.Node.Identity().Identity)
	if err != nil {
		t.Fatal(err)
	}
	var plain []byte
	if kind == protocol.CmdSendLogin {
		req, err := meshcore.AnonReqFromBytes(pkt.Payload)
		if err != nil {
			t.Fatal(err)
		}
		plain = req.Decrypt(secret)
		offset := 4
		c.server.mu.Lock()
		p := c.server.findContact(peer.PublicKeyBytes())
		room := p.Type == meshcore.AdvertTypeRoom
		c.server.mu.Unlock()
		if room {
			offset = 8
			if len(plain) < offset || u32(plain[4:8]) != 42 {
				t.Fatal("room retry changed the sync timestamp")
			}
		}
		if len(plain) <= offset || string(bytes.TrimRight(plain[offset:], "\x00")) != string(data) {
			t.Fatal("retry did not transmit the supplied credential")
		}
		if u32(frame[2:6]) != u32(peer.PublicKeyBytes()[:4]) {
			t.Fatal("login changed the native SENT peer-prefix tag")
		}
	} else {
		req, err := meshcore.RequestFromBytes(pkt.Payload)
		if err != nil {
			t.Fatal(err)
		}
		plain = req.Decrypt(secret)
	}
	if len(plain) < 4 {
		t.Fatal("request had no correlation tag")
	}
	return u32(plain)
}

func loginTestReply(s *Server, p *contact, tag uint32, body []byte) {
	data := make([]byte, 4, 4+len(body))
	put32(data, tag)
	s.mu.Lock()
	defer s.mu.Unlock()
	s.response(p, append(data, body...))
}

func loginTestNoPush(t *testing.T, clients ...*session) {
	t.Helper()
	for _, c := range clients {
		select {
		case frame := <-c.out:
			t.Fatalf("stale or ambiguous reply produced a push: %x", frame)
		default:
		}
	}
}

func loginTestPush(t *testing.T, c *session, code byte) {
	t.Helper()
	select {
	case frame := <-c.out:
		if frame[0] != code {
			t.Fatalf("push = %x, want code %x", frame, code)
		}
	default:
		t.Fatalf("missing push %x", code)
	}
}

func TestLoginRetrySupersedesOnlySamePeerLogin(t *testing.T) {
	for _, kind := range []byte{meshcore.AdvertTypeRepeater, meshcore.AdvertTypeRoom} {
		t.Run(map[byte]string{meshcore.AdvertTypeRepeater: "repeater", meshcore.AdvertTypeRoom: "room"}[kind], func(t *testing.T) {
			s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
			a, b := commandSession(t, s), commandSession(t, s)
			s.mu.Lock()
			s.clients[a], s.clients[b] = struct{}{}, struct{}{}
			s.mu.Unlock()
			peer, other := testIdentity(2), testIdentity(3)
			p := loginTestContact(t, s, peer, kind)
			op := loginTestContact(t, s, other, meshcore.AdvertTypeRoom)
			// Adding a contact can reallocate the slice.
			s.mu.Lock()
			p = s.findContact(peer.PublicKeyBytes())
			s.mu.Unlock()
			oldTag := loginTestSend(t, a, r, peer, protocol.CmdSendLogin, []byte("wrong"))
			statusTag := loginTestSend(t, a, r, peer, protocol.CmdSendStatusReq, nil)
			otherTag := loginTestSend(t, a, r, other, protocol.CmdSendLogin, []byte("other"))
			s.mu.Lock()
			statusBefore, otherBefore := s.pending[statusTag], s.pending[otherTag]
			s.mu.Unlock()

			newTag := loginTestSend(t, b, r, peer, protocol.CmdSendLogin, []byte("correct"))
			s.mu.Lock()
			_, oldPending := s.pending[oldTag]
			current := s.pending[newTag]
			if oldPending || newTag == oldTag || current.Client != b || current.Kind != protocol.CmdSendLogin ||
				time.Until(current.Expires) < 119*time.Second ||
				s.pending[statusTag] != statusBefore || s.pending[otherTag] != otherBefore || len(s.pending) != 3 {
				t.Fatal("retry changed another operation or failed to replace the login/deadline")
			}
			s.mu.Unlock()
			loginTestReply(s, p, oldTag, []byte("OK"))
			loginTestReply(s, p, oldTag, []byte("NO"))
			loginTestNoPush(t, a, b)
			// Untagged native login replies remain ambiguous while status is pending.
			native := []byte{0, 1, 1, 3, 0, 0, 0, 0, 7}
			loginTestReply(s, p, newTag^0xffffffff, native)
			loginTestNoPush(t, a, b)
			loginTestReply(s, p, statusTag, []byte{1, 2, 3})
			loginTestPush(t, a, protocol.PushStatusResponse)
			loginTestNoPush(t, b)
			loginTestReply(s, p, oldTag, []byte("OK"))
			loginTestReply(s, p, oldTag, []byte("NO"))
			loginTestNoPush(t, a, b)
			loginTestReply(s, p, newTag^0xffffffff, native)
			loginTestPush(t, a, protocol.PushLoginSuccess)
			loginTestPush(t, b, protocol.PushLoginSuccess)
			s.mu.Lock()
			_, pendingNew := s.pending[newTag]
			_, oldOwner := s.connections[p.PublicKey].Owners[a]
			_, newOwner := s.connections[p.PublicKey].Owners[b]
			if pendingNew || oldOwner || !newOwner || len(s.pending) != 1 || s.pending[otherTag] != otherBefore {
				t.Fatal("login reply revived the superseded owner or consumed another peer's request")
			}
			s.mu.Unlock()
			statusTag = loginTestSend(t, b, r, peer, protocol.CmdSendStatusReq, nil)
			loginTestReply(s, p, oldTag, []byte("OK"))
			loginTestNoPush(t, a, b)
			loginTestReply(s, p, statusTag, []byte{4, 5, 6})
			loginTestPush(t, b, protocol.PushStatusResponse)
			loginTestNoPush(t, a)
			loginTestReply(s, op, otherTag, native)
			loginTestPush(t, a, protocol.PushLoginSuccess)
			loginTestPush(t, b, protocol.PushLoginSuccess)
			s.mu.Lock()
			remaining := len(s.pending)
			s.mu.Unlock()
			if remaining != 0 {
				t.Fatalf("valid unrelated response left %d pending requests", remaining)
			}
		})
	}
}

func TestLoginRetrySendFailurePreservesPriorRequest(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	a, b := commandSession(t, s), commandSession(t, s)
	peer := testIdentity(2)
	loginTestContact(t, s, peer, meshcore.AdvertTypeRepeater)
	oldTag := loginTestSend(t, a, r, peer, protocol.CmdSendLogin, []byte("wrong"))
	s.mu.Lock()
	before := s.pending[oldTag]
	reported := make(chan error, 1)
	s.cfg.ErrorHandler = func(err error) { reported <- err }
	s.mu.Unlock()
	for len(r.out) < cap(r.out) {
		r.out <- []byte{0}
	}
	cmd := append([]byte{protocol.CmdSendLogin}, peer.PublicKeyBytes()...)
	frame := sessionCommand(t, b, append(cmd, []byte("correct")...))
	if !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeBadState}) {
		t.Fatalf("failed TX claimed admission: %x", frame)
	}
	select {
	case <-reported:
	default:
		t.Fatal("TX admission failure was not reported")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.pending) != 1 || s.pending[oldTag] != before || len(s.loginFences) != 0 || len(r.out) != cap(r.out) {
		t.Fatal("failed retry changed the prior request or created a response fence")
	}
}

func TestLoginRetryCapacity(t *testing.T) {
	for _, fencesFull := range []bool{false, true} {
		t.Run(map[bool]string{false: "pending-full", true: "fences-full"}[fencesFull], func(t *testing.T) {
			s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
			c := commandSession(t, s)
			peer, other := testIdentity(2), testIdentity(3)
			loginTestContact(t, s, peer, meshcore.AdvertTypeRepeater)
			oldTag := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("wrong"))
			s.mu.Lock()
			before := s.pending[oldTag]
			for tag := uint32(1); tag < maxPendingRequests; tag++ {
				s.pending[tag] = pending{Client: c, Key: other.PublicKey(), Kind: protocol.CmdSendStatusReq, Expires: before.Expires}
			}
			if fencesFull {
				for tag := uint32(1); tag <= maxPendingRequests; tag++ {
					s.loginFences[tag] = loginFence{Key: other.PublicKey(), Expires: before.Expires}
				}
			}
			s.mu.Unlock()
			cmd := append(append([]byte{protocol.CmdSendLogin}, peer.PublicKeyBytes()...), []byte("correct")...)
			if frame := sessionCommand(t, c, append([]byte{protocol.CmdSendStatusReq}, peer.PublicKeyBytes()...)); !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeTableFull}) {
				t.Fatalf("replacement exception admitted an unrelated request at capacity: %x", frame)
			}
			if fencesFull {
				frame := sessionCommand(t, c, cmd)
				if !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeBadState}) {
					t.Fatalf("unfenced replacement was admitted: %x", frame)
				}
				s.mu.Lock()
				if len(s.pending) != maxPendingRequests || s.pending[oldTag] != before || len(r.out) != 0 {
					t.Fatal("fence capacity rejection changed pending requests or sent a packet")
				}
				for tag, fence := range s.loginFences {
					fence.Expires = time.Now().Add(-time.Second)
					s.loginFences[tag] = fence
				}
				s.mu.Unlock()
			}
			newTag := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("correct"))
			s.mu.Lock()
			defer s.mu.Unlock()
			if _, ok := s.pending[oldTag]; ok || len(s.pending) != maxPendingRequests || len(s.loginFences) != 1 ||
				s.pending[newTag].Client != c {
				t.Fatal("replacement at capacity failed or retained expired fences")
			}
		})
	}
}

func TestLoginRetryNativeServerTimeMayEqualSupersededTag(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	a, b := commandSession(t, s), commandSession(t, s)
	s.mu.Lock()
	s.clients[a], s.clients[b] = struct{}{}, struct{}{}
	s.mu.Unlock()
	peer := testIdentity(2)
	p := loginTestContact(t, s, peer, meshcore.AdvertTypeRoom)
	oldTag := loginTestSend(t, a, r, peer, protocol.CmdSendLogin, []byte("wrong"))
	newTag := loginTestSend(t, b, r, peer, protocol.CmdSendLogin, []byte("correct"))
	loginTestReply(s, p, oldTag, []byte{0, 1, 1, 3, 0, 0, 0, 0, 7})
	loginTestPush(t, a, protocol.PushLoginSuccess)
	loginTestPush(t, b, protocol.PushLoginSuccess)
	s.mu.Lock()
	defer s.mu.Unlock()
	_, pendingNew := s.pending[newTag]
	_, oldOwner := s.connections[p.PublicKey].Owners[a]
	_, newOwner := s.connections[p.PublicKey].Owners[b]
	if pendingNew || oldOwner || !newOwner {
		t.Fatal("native server-time collision rejected the current login or revived the superseded owner")
	}
}

func TestLoginRetryFailureTargetsLatestRequester(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	a, b := commandSession(t, s), commandSession(t, s)
	peer := testIdentity(2)
	p := loginTestContact(t, s, peer, meshcore.AdvertTypeRepeater)
	oldTag := loginTestSend(t, a, r, peer, protocol.CmdSendLogin, []byte("wrong"))
	newTag := loginTestSend(t, b, r, peer, protocol.CmdSendLogin, []byte("correct"))
	loginTestReply(s, p, oldTag, []byte("NO"))
	loginTestNoPush(t, a, b)
	loginTestReply(s, p, newTag, []byte("NO"))
	loginTestPush(t, b, protocol.PushLoginFail)
	loginTestNoPush(t, a)
	loginTestReply(s, p, oldTag, []byte("OK"))
	loginTestNoPush(t, a, b)
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.pending) != 0 || len(s.connections) != 0 {
		t.Fatal("failure or stale success revived the superseded login")
	}
}

func TestLoginRetryExpiredReplacementCannotComplete(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	c := commandSession(t, s)
	s.mu.Lock()
	s.clients[c] = struct{}{}
	s.mu.Unlock()
	peer := testIdentity(2)
	p := loginTestContact(t, s, peer, meshcore.AdvertTypeRepeater)
	oldTag := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("wrong"))
	newTag := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("correct"))
	s.mu.Lock()
	req := s.pending[newTag]
	req.Expires = time.Now().Add(-time.Second)
	s.pending[newTag] = req
	s.loginFences[oldTag] = loginFence{Key: peer.PublicKey(), Expires: req.Expires}
	s.mu.Unlock()
	loginTestReply(s, p, oldTag, []byte("OK"))
	loginTestReply(s, p, newTag, []byte{0, 1, 1, 3, 0, 0, 0, 0, 7})
	loginTestNoPush(t, c)
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.pending) != 0 || len(s.connections) != 0 {
		t.Fatal("an expired replacement was completed by a late response")
	}
}

func TestLoginRetryExtendsAllSamePeerFencesThroughLatestDeadline(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	c := commandSession(t, s)
	peer := testIdentity(2)
	loginTestContact(t, s, peer, meshcore.AdvertTypeRepeater)
	first := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("first"))
	second := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("second"))
	s.mu.Lock()
	s.loginFences[first] = loginFence{Key: peer.PublicKey(), Expires: time.Now().Add(time.Second)}
	s.mu.Unlock()
	third := loginTestSend(t, c, r, peer, protocol.CmdSendLogin, []byte("third"))
	s.mu.Lock()
	defer s.mu.Unlock()
	deadline := s.pending[third].Expires
	if len(s.pending) != 1 || len(s.loginFences) != 2 ||
		s.loginFences[first].Expires != deadline || s.loginFences[second].Expires != deadline {
		t.Fatal("an earlier superseded tag became unfenced while the latest retry remained pending")
	}
}

func TestSupersededLoginFenceDoesNotDropAnotherPeersLegacyReply(t *testing.T) {
	s, r, _ := startTestServer(t, testIdentity(1), testConfig(""))
	a, b := commandSession(t, s), commandSession(t, s)
	peer, other := testIdentity(2), testIdentity(3)
	loginTestContact(t, s, peer, meshcore.AdvertTypeRepeater)
	op := loginTestContact(t, s, other, meshcore.AdvertTypeRepeater)
	oldTag := loginTestSend(t, a, r, peer, protocol.CmdSendLogin, []byte("wrong"))
	loginTestSend(t, a, r, peer, protocol.CmdSendLogin, []byte("correct"))
	statusTag := loginTestSend(t, b, r, other, protocol.CmdSendStatusReq, nil)
	loginTestReply(s, op, oldTag, []byte{1, 2, 3})
	loginTestPush(t, b, protocol.PushStatusResponse)
	loginTestNoPush(t, a)
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.pending[statusTag]; ok || len(s.pending) != 1 {
		t.Fatal("peer-scoped fence dropped the valid unrelated legacy response")
	}
}
