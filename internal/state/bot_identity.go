package state

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"

	meshcore "github.com/meshcore-go/meshcore-go"
)

var ErrBotIdentityActive = errors.New("bot identity is already active")

// BotIdentityChange keeps active and staged keys in the same Go authority.
// Pending keys are never activated by Identity or by host startup.
func BotIdentityChange(dir, action string, expanded []byte) (meshcore.LocalIdentity, error) {
	return botIdentityChange(dir, action, expanded, syncDirectory)
}

func botIdentityChange(dir, action string, expanded []byte, syncDir func(string) error) (meshcore.LocalIdentity, error) {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	var zero meshcore.LocalIdentity
	roleDir := filepath.Join(dir, "bot")
	envelope, err := readRoleEnvelope(roleDir)
	if errors.Is(err, os.ErrNotExist) {
		key, exportErr := ExportIdentity(dir, "bot")
		if exportErr != nil {
			return zero, exportErr
		}
		id, decodeErr := decodeExpandedIdentity(key)
		if decodeErr != nil {
			return zero, decodeErr
		}
		envelope = &roleEnvelope{Version: 1, Expanded: key, Document: "bot", State: []byte("{}"), identity: id}
	} else if err != nil {
		return zero, err
	}
	if envelope.Document != "bot" {
		return zero, errors.New("bot identity authority belongs to a different document")
	}
	switch action {
	case "pending":
		if envelope.Pending == nil {
			return zero, errors.New("no pending bot identity")
		}
		return envelope.pending, nil
	case "stage":
		next, err := decodeExpandedIdentity(expanded)
		if err != nil {
			return zero, err
		}
		if envelope.identity.PublicKey() == next.PublicKey() {
			return envelope.identity, ErrBotIdentityActive
		}
		if envelope.Pending != nil && !bytes.Equal(envelope.Pending, expanded) {
			return zero, errors.New("different bot identity pending; inspect or cancel it first")
		}
		if err := checkRoleIdentity(dir, "bot", next.Identity); err != nil {
			return zero, err
		}
		envelope.Pending = bytes.Clone(expanded)
		zero = next
	case "cancel":
		envelope.Pending = nil
		zero = envelope.identity
	case "apply":
		if envelope.Pending == nil {
			if !bytes.Equal(envelope.Expanded, expanded) {
				return zero, errors.New("pending bot identity changed or absent")
			}
			// A stopped service may retry a visible but unconfirmed commit.
			// Republish/sync its active authority before enabling RF again.
			zero = envelope.identity
			break
		}
		if !bytes.Equal(envelope.Pending, expanded) {
			return zero, errors.New("pending bot identity changed or absent")
		}
		if err := checkRoleIdentity(dir, "bot", envelope.pending.Identity); err != nil {
			return zero, err
		}
		envelope.Expanded, envelope.Pending = envelope.Pending, nil
		zero = envelope.pending
	default:
		return zero, errors.New("unsupported bot identity operation")
	}
	if err := writeRoleEnvelope(roleDir, envelope, syncDir); err != nil {
		return meshcore.LocalIdentity{}, err
	}
	return zero, nil
}

func PendingBotIdentity(dir string) ([]byte, error) {
	roleStateMu.Lock()
	defer roleStateMu.Unlock()
	envelope, err := readRoleEnvelope(filepath.Join(dir, "bot"))
	if err != nil {
		return nil, err
	}
	if envelope.Document != "bot" || envelope.Pending == nil {
		return nil, errors.New("no pending bot identity")
	}
	return bytes.Clone(envelope.Pending), nil
}
