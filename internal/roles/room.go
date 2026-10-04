package roles

import (
	"errors"
	"time"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func (s *Service) addPost(author [32]byte, text string) {
	if len(text) > nativePostBytes {
		text = text[:nativePostBytes]
	}
	p := post{Author: author, Timestamp: s.uniqueTime(), Text: text}
	if !utf8.ValidString(text) {
		p.RawText = []byte(text)
		p.Text = ""
	}
	if len(s.state.History) == s.cfg.MaxHistory {
		copy(s.state.History, s.state.History[1:])
		s.state.History[len(s.state.History)-1] = p
	} else {
		s.state.History = append(s.state.History, p)
	}
	s.state.Posted++
	s.nextPush = time.Now().Add(2 * time.Second)
}

func (s *Service) pendingACK(crc uint32) bool {
	if crc == 0 {
		return false
	}
	for _, m := range s.state.Members {
		if m.Pending && m.PendingACK == crc {
			return true
		}
	}
	return false
}

func (s *Service) acknowledge(crc uint32) bool {
	if crc == 0 {
		return false
	}
	for _, m := range s.state.Members {
		if m.Pending && m.PendingACK == crc {
			m.Pending, m.Failures, m.SyncSince = false, 0, m.PendingPost
			return true
		}
	}
	return false
}

// syncAt is called with mu held. Pending state is deliberately transient:
// reconnecting clients resume from their persisted, ACK-confirmed cursor.
func (s *Service) syncAt(now time.Time) ([]transmission, error) {
	if s.restartPending {
		return nil, nil
	}
	if s.commitUncertain {
		return nil, ErrCommitUncertain
	}
	if now.Before(s.nextPush) {
		return nil, nil
	}
	for _, m := range s.state.Members {
		if m.Pending && !now.Before(m.Deadline) {
			m.Pending = false
			m.Failures++
		}
	}
	keys := s.memberKeys()
	if len(keys) == 0 {
		return nil, nil
	}
	s.nextUser %= len(keys)
	m := s.state.Members[keys[s.nextUser]]
	s.nextUser++
	s.nextPush = now.Add(150 * time.Millisecond)
	if m.Pending || !m.Active || m.Failures >= 3 {
		return nil, nil
	}
	for _, p := range s.state.History {
		if p.Timestamp <= m.SyncSince || p.Author == m.Key ||
			now.Add(time.Duration(s.state.RTCOffset)*time.Second).Before(time.Unix(int64(p.Timestamp), 0).Add(s.postDelay)) {
			continue
		}
		before := *m
		// Unlike ordinary DMs, signed room posts use the recipient public key
		// in the ACK proof (simple_room_server/MyMesh.cpp pushPostToClient).
		var attempt [1]byte
		if _, err := s.random(attempt[:]); err != nil {
			return nil, err
		}
		plain := make([]byte, 5)
		put32(plain, p.Timestamp)
		plain[4] = 2<<2 | attempt[0]&3
		plain = append(plain, p.Author[:4]...)
		plain = append(plain, p.text()...)
		packet, err := s.datagram(m, meshcore.PayloadTypeTxtMsg, plain)
		if err != nil {
			return nil, err
		}
		m.PendingACK = meshcore.CalcAckHash(plain, m.Key[:])
		m.Pending, m.PendingPost = m.PendingACK != 0, p.Timestamp
		timeout := 12 * time.Second
		if m.KnownPath {
			timeout = time.Duration(4+2*(int(m.PathLength&63)+1)) * time.Second
		}
		m.Deadline = now.Add(timeout)
		s.state.Pushed++
		if err := s.save(); err != nil {
			if errors.Is(err, ErrCommitUncertain) {
				s.failClosed(err)
			} else {
				*m = before
				s.state.Pushed--
			}
			s.lastErr = err
			return nil, err
		}
		s.nextPush = now.Add(1200 * time.Millisecond)
		return one(packet), nil
	}
	return nil, nil
}

func validPost(text string) error {
	if len(text) == 0 || len(text) > maxPostBytes {
		return errors.New("stored post must contain 1-151 bytes")
	}
	return nil
}
