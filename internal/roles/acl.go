package roles

import (
	"errors"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
)

type AccessEntry struct {
	PublicKey   [32]byte
	Permissions policy.Permission
}

// AccessList is the complete local-console ACL view. The native binary query
// has its own role-specific filter and datagram/path-return size limits.
func (s *Service) AccessList() []AccessEntry {
	s.mu.RLock()
	defer s.mu.RUnlock()
	var entries []AccessEntry
	for _, key := range s.memberKeys() {
		m := s.state.Members[key]
		if m.Permissions != 0 {
			entries = append(entries, AccessEntry{m.Key, policy.Permission(m.Permissions)})
		}
	}
	return entries
}

// Match ClientACL::putClient's least-active non-admin eviction, except that a
// table containing only administrators must never overwrite an administrator.
func (s *Service) putMember(id meshcore.Identity) (*member, error) {
	key := id.String()
	if m := s.state.Members[key]; m != nil {
		return m, nil
	}
	order := s.memberKeys()
	slot := len(order)
	if len(s.state.Members) >= s.cfg.MaxMembers {
		slot = -1
		for i, key := range order {
			m := s.state.Members[key]
			if m.Permissions&3 == 3 {
				continue
			}
			if slot == -1 || m.LastActivity < s.state.Members[order[slot]].LastActivity {
				slot = i
			}
		}
		if slot == -1 {
			return nil, errors.New("role membership is full of administrators")
		}
		delete(s.state.Members, order[slot])
	}
	m := &member{Key: id.PublicKey()}
	if slot == len(order) {
		order = append(order, key)
	} else {
		order[slot] = key
	}
	s.state.MemberOrder = order
	s.state.Members[key] = m
	return m, nil
}

type rateLimiter struct {
	start uint32
	count uint32
}

func (r *rateLimiter) allow(now, seconds, maximum uint32) bool {
	if now >= r.start+seconds {
		r.start, r.count = now, 1
		return true
	}
	if r.count >= maximum {
		return false
	}
	r.count++
	return true
}
