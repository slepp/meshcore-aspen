package companion

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	hoststate "meshcore.local/meshcore/internal/state"
)

// The command boundary supplies exactly 64 native scalar-prefix bytes.
func validateImportedIdentity(key []byte) (meshcore.LocalIdentity, error) {
	var zero meshcore.LocalIdentity
	if key[0]&7 != 0 || key[31]&0xc0 != 0x40 {
		return zero, errors.New("identity import: scalar is not clamped")
	}
	id, err := meshcore.NewLocalIdentityFromExpandedKey(key)
	if err != nil {
		return zero, err
	}
	if pub := id.PublicKey(); pub[0] == 0 || pub[0] == 255 {
		return zero, errors.New("identity import: reserved public-key prefix")
	}
	// Like native validation, test ECDH both ways against a public fixed probe.
	probe := meshcore.NewLocalIdentityFromSeed([32]byte{})
	ours, err := id.SharedSecret(probe.Identity)
	if err != nil {
		return zero, err
	}
	theirs, err := probe.SharedSecret(id.Identity)
	if err != nil {
		return zero, err
	}
	if !bytes.Equal(ours, theirs) || bytes.Equal(ours, make([]byte, 32)) {
		return zero, errors.New("identity import: invalid shared secret")
	}
	return id, nil
}

func (s *Server) importIdentity(c *session, key []byte) (byte, error) {
	id, err := validateImportedIdentity(key)
	if err != nil {
		return protocol.ErrCodeIllegalArg, err
	}
	next := s.state
	next.PublicKey = id.PublicKey()
	data, err := json.Marshal(next)
	if err != nil {
		return protocol.ErrCodeBadState, err
	}
	ctx, cancel := context.WithTimeout(s.ctx, 3*time.Second)
	defer cancel()
	id, err = s.cfg.ImportIdentity(ctx, key, data)
	if err != nil {
		if errors.Is(err, hoststate.ErrCommitIndeterminate) {
			s.failClosed(err)
		}
		return protocol.ErrCodeFileIoError, err
	}
	if id.PublicKey() != next.PublicKey {
		err := errors.New("identity import committed an unexpected identity")
		s.failClosed(err)
		return protocol.ErrCodeBadState, err
	}

	// Commit success remains authoritative after cancellation. Activate under the
	// command lock so Close cannot save the previous state's public key.
	s.state = next
	s.Node.SetIdentity(id)
	s.identity.Store(&id)
	clear(s.pending)
	clear(s.loginFences)
	clear(s.acks)
	clear(s.connections)
	clear(s.traces)
	clear(s.transient)
	c.signData = nil
	for client := range s.clients {
		client.signData = nil
	}
	s.hydrate()
	return 0, nil
}
