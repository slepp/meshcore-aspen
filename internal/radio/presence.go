package radio

import (
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

type MastRole uint8

const (
	RepeaterRole MastRole = iota
	RoomRole
	CompanionRole
	ObserverRole
)

func (r MastRole) String() string {
	switch r {
	case RepeaterRole:
		return "repeater"
	case RoomRole:
		return "room"
	case CompanionRole:
		return "companion"
	case ObserverRole:
		return "observer"
	default:
		return fmt.Sprintf("role %d", r)
	}
}

type RoleAnnouncement struct {
	Role      MastRole
	PublicKey [32]byte
}

type RolePresenceStatus struct {
	State              string   `json:"state"`
	Role               string   `json:"role"`
	PublicKey          string   `json:"public_key"`
	SourceGeneration   uint32   `json:"source_generation"`
	NativeMask         uint8    `json:"native_mask"`
	AppliedNativeRoles []string `json:"applied_native_roles"`
	NativeRoleRunning  *bool    `json:"native_role_running,omitempty"`
	WarningFlags       uint8    `json:"warning_flags"`
	Warning            string   `json:"warning,omitempty"`
	Online             bool     `json:"online"`
}

const (
	presenceNativeRole = 1 << iota
	presenceHostRole
	presenceNativeKey
	presenceHostKey
)

func (l *Link) setRolePresenceStatus(status RolePresenceStatus) {
	l.mu.Lock()
	l.presenceStatus = &status
	l.mu.Unlock()
}

func (l *Link) RolePresenceStatus() *RolePresenceStatus {
	l.mu.RLock()
	defer l.mu.RUnlock()
	if l.config.RoleAnnouncement == nil {
		return nil
	}
	status := RolePresenceStatus{
		State: "negotiating", Role: l.config.RoleAnnouncement.Role.String(),
		PublicKey: hex.EncodeToString(l.config.RoleAnnouncement.PublicKey[:]),
	}
	if l.presenceStatus != nil {
		status = *l.presenceStatus
	}
	status.Online = l.modem != nil
	return &status
}

// presenceTimeout bounds the optional ROLE_PRESENCE exchange so a silent mast
// cannot consume the whole connection budget.
var presenceTimeout = 5 * time.Second

// errPresenceUncertain means optional discovery left request/response
// correlation on this connection uncertain: a late reply, including an
// uncorrelated error frame, could be taken as the answer to a later control
// request. The connection is retired and the next one skips discovery.
var errPresenceUncertain = errors.New("ROLE_PRESENCE outcome uncertain; retiring connection")

func uncertainExchange(err error) bool {
	return errors.Is(err, context.DeadlineExceeded) || errors.Is(err, context.Canceled) ||
		errors.Is(err, hardware.ErrDisconnected) || errors.Is(err, hardware.ErrModemClosed)
}

// announceRole performs advisory discovery. Overlap never denies admission:
// unsupported, rejected, malformed or unrecognized replies are reported as
// state "unsupported" or "unknown" and the connection continues. Only an
// exchange whose correlation is uncertain returns an error.
func (l *Link) announceRole(ctx context.Context, modem *hardware.KissModem, generation uint32) error {
	announcement := l.config.RoleAnnouncement
	role, key := announcement.Role.String(), hex.EncodeToString(announcement.PublicKey[:])
	identity := fmt.Sprintf("%s (public key %s)", role, key)
	status := RolePresenceStatus{
		State: "announced", Role: role, PublicKey: key, SourceGeneration: generation,
	}
	unknown := func(detail string, args ...any) error {
		status.State = "unknown"
		status.Warning = fmt.Sprintf("Overlap unknown for %s: %s", identity, fmt.Sprintf(detail, args...))
		l.config.Logger.Warn("role presence unknown", "role", role, "public_key", key,
			"native_mask", status.NativeMask, "warning_flags", status.WarningFlags, "warning", status.Warning)
		l.setRolePresenceStatus(status)
		return nil
	}
	l.mu.Lock()
	skip := l.skipPresence
	l.skipPresence = false
	l.mu.Unlock()
	if skip {
		return unknown("discovery skipped after an uncertain ROLE_PRESENCE exchange on the previous connection")
	}
	ctx, cancel := context.WithTimeout(ctx, presenceTimeout)
	defer cancel()
	request := make([]byte, 34)
	request[0], request[1] = QueuedProtocolVersion, byte(announcement.Role)
	copy(request[2:], announcement.PublicKey[:])
	reply, err := modem.Request(ctx, HWQueuedRolePresence, request)
	if errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_UNKNOWN_CMD)) ||
		errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_NO_CALLBACK)) {
		capacity, capacityErr := clientCapacity(ctx, modem)
		if uncertainExchange(capacityErr) {
			return fmt.Errorf("%w: CAPACITY fallback: %v", errPresenceUncertain, capacityErr)
		}
		status.State = "unsupported"
		switch {
		case errors.Is(capacityErr, ErrCapacityUnavailable):
			status.Warning = fmt.Sprintf("Cannot check %s against mast or other hosts: presence and capacity queries are unsupported", identity)
		case capacityErr != nil:
			status.Warning = fmt.Sprintf("Cannot check %s against mast or other hosts: presence query is unsupported and CAPACITY failed: %v", identity, capacityErr)
		case capacity.LocalSourceSlots != 0:
			status.Warning = fmt.Sprintf("Cannot check %s against mast or other hosts: presence query is unsupported; mast reports %d local sources", identity, capacity.LocalSourceSlots)
		default:
			status.Warning = fmt.Sprintf("Cannot check %s against other hosts: presence query is unsupported; mast reports zero local sources", identity)
		}
		l.config.Logger.Warn("role presence unavailable", "role", role, "public_key", key, "warning", status.Warning)
		l.setRolePresenceStatus(status)
		return nil
	}
	if uncertainExchange(err) {
		return fmt.Errorf("%w: %v", errPresenceUncertain, err)
	}
	if err != nil {
		return unknown("ROLE_PRESENCE failed: %v", err)
	}
	if len(reply) != 8 || reply[0] != QueuedProtocolVersion {
		return unknown("invalid ROLE_PRESENCE response %x", reply)
	}
	if got := binary.LittleEndian.Uint32(reply[2:]); got != generation {
		return unknown("stale ROLE_PRESENCE response for generation %d (expected %d)", got, generation)
	}
	status.NativeMask = reply[6]
	status.WarningFlags = reply[7]
	status.AppliedNativeRoles = make([]string, 0, 4)
	for role := RepeaterRole; role <= ObserverRole; role++ {
		if reply[6]&(1<<role) != 0 {
			status.AppliedNativeRoles = append(status.AppliedNativeRoles, role.String())
		}
	}
	running := reply[7]&presenceNativeRole != 0
	status.NativeRoleRunning = &running
	if reply[1] != 0 {
		return unknown("ROLE_PRESENCE reason %d, native mask 0x%02x", reply[1], reply[6])
	}
	var warnings []string
	if reply[7]&presenceNativeRole != 0 {
		warnings = append(warnings, "same role also running on mast")
	}
	if reply[7]&presenceHostRole != 0 {
		warnings = append(warnings, "another connected host announced the same role")
	}
	if reply[7]&presenceNativeKey != 0 {
		warnings = append(warnings, "mast also reports the same public key")
	}
	if reply[7]&presenceHostKey != 0 {
		warnings = append(warnings, "another connected host announced the same public key")
	}
	if extra := reply[6]&^0x0f | reply[7]&^0x0f; extra != 0 {
		detail := fmt.Sprintf("unrecognized native mask 0x%02x or warning flags 0x%02x", reply[6]&^0x0f, reply[7]&^0x0f)
		if len(warnings) > 0 {
			detail += "; also " + strings.Join(warnings, "; ")
		}
		return unknown("%s", detail)
	}
	if len(warnings) > 0 {
		status.State = "overlap"
		status.Warning = fmt.Sprintf("%s: %s; concurrent instances may be intentional", identity, strings.Join(warnings, "; "))
		l.config.Logger.Warn("role presence overlap", "role", role, "public_key", key,
			"native_mask", reply[6], "warning_flags", reply[7], "warning", status.Warning)
	}
	l.setRolePresenceStatus(status)
	return nil
}
