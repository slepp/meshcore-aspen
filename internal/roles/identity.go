package roles

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	hoststate "meshcore.local/meshcore/internal/state"
)

type identityStage struct {
	key    []byte
	public meshcore.Identity
	prefix string
}

func parseIdentityStage(value string) (*identityStage, error) {
	if len(value) != 128 {
		return nil, errors.New("invalid native key")
	}
	key, err := hex.DecodeString(value)
	if err != nil {
		clear(key)
		return nil, errors.New("invalid native key")
	}
	if key[0]&7 != 0 || key[31]&0xc0 != 0x40 {
		clear(key)
		return nil, errors.New("invalid native key")
	}
	id, err := meshcore.NewLocalIdentityFromExpandedKey(key)
	if err != nil || id.PublicKey()[0] == 0 || id.PublicKey()[0] == 255 {
		clear(key)
		return nil, errors.New("invalid native key")
	}
	return &identityStage{key: key, public: id.Identity}, nil
}

func (s *Service) stageIdentity(e event, stage *identityStage) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	err := s.cfg.StageIdentity(ctx, stage.key)
	cancel()
	clear(stage.key)
	if err != nil {
		// Provider errors can contain submitted key material. Preserve only the
		// indeterminate classification, never their text or wrapped cause.
		safe := errors.New("role identity staging failed")
		if errors.Is(err, hoststate.ErrCommitIndeterminate) || errors.Is(err, ErrCommitUncertain) {
			safe = errors.Join(safe, ErrCommitUncertain, hoststate.ErrCommitIndeterminate)
			s.mu.Lock()
			s.failClosed(safe)
			s.mu.Unlock()
			s.report(safe)
			return
		}
		s.report(safe)
		s.sendCLIResult(e, stage.prefix+"Error, key import failed")
		return
	}
	s.sendCLIResult(e, stage.prefix+"OK, reboot to apply! New pubkey: "+strings.ToUpper(stage.public.String()))
}

// RebindStateIdentity prepares a role-state blob for a parent-owned identity
// transaction. It performs no I/O. The parent must quiesce the role and commit
// the expanded identity and returned state together with crash recovery.
// Raw fields preserve legacy history and unknown future extension data.
func RebindStateIdentity(data []byte, previous, next meshcore.Identity) ([]byte, error) {
	if previous.IsZero() || next.IsZero() || len(data) > maxStateBytes {
		return nil, errors.New("invalid role identity transition")
	}
	var envelope struct {
		Version  int
		Identity string
	}
	if err := json.Unmarshal(data, &envelope); err != nil {
		return nil, err
	}
	if (envelope.Version != 1 && envelope.Version != 2) || envelope.Identity != previous.String() {
		return nil, errors.New("role state does not match previous identity or supported version")
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(data, &fields); err != nil {
		return nil, err
	}
	seen := make(map[string]bool, len(fields))
	for key := range fields {
		folded := strings.ToLower(key)
		if seen[folded] {
			return nil, errors.New("ambiguous role-state field casing")
		}
		seen[folded] = true
		if strings.EqualFold(key, "Identity") {
			delete(fields, key)
		}
	}
	identity, err := json.Marshal(next.String())
	if err != nil {
		return nil, err
	}
	fields["Identity"] = identity
	prepared, err := json.Marshal(fields)
	if err != nil {
		return nil, err
	}
	if len(prepared) > maxStateBytes {
		return nil, errors.New("prepared role state exceeds persistence limit")
	}
	return prepared, nil
}
