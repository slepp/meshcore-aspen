package roles

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"
)

var (
	ErrRoleStopped         = errors.New("role is stopped or restarting")
	ErrOwnerCommandDenied  = errors.New("role owner command is outside the supported configuration scope")
	ErrOwnerCommandUnknown = errors.New("role owner command outcome unknown; inspect affected settings before repeating")
	ErrNameChangeUnknown   = ErrOwnerCommandUnknown
)

type ownerCommand struct {
	ctx     context.Context
	command string
	mutates bool
	result  chan ownerCommandResult
	reply   string
	err     error
}

type ownerCommandResult struct {
	reply string
	err   error
}

// ConfigureName executes the existing "set name" command on the role worker.
// The caller must authorize host-owner access before calling this method.
// It returns the existing CLI response after persistence and policy publication.
// A canceled admitted operation may already have committed: inspect Name before
// retrying when the error is ErrNameChangeUnknown.
func (s *Service) ConfigureName(ctx context.Context, name string) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if len(name) == 0 || len(name) > 31 || !utf8.ValidString(name) ||
		strings.IndexFunc(name, unicode.IsControl) >= 0 {
		return "", errors.New("role name must be 1–31 UTF-8 bytes without control characters")
	}
	return s.executeOwnerCommand(ctx, "set name "+name, true)
}

// OwnerAdmin runs only scoped configuration commands on the existing role
// worker. The caller must authenticate and authorize host-owner access.
// Passwords, ACLs, identity, shared RF changes and restart are not exposed.
// Native CLI validation and error replies remain authoritative.
func (s *Service) OwnerAdmin(ctx context.Context, command string) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if len(command) == 0 || len(command) > 160 {
		return "", ErrOwnerCommandDenied
	}
	for i := range command {
		if command[i] < 32 || command[i] > 126 {
			return "", ErrOwnerCommandDenied
		}
	}
	mutates, allowed := ownerCommandScope(command)
	if !allowed {
		return "", ErrOwnerCommandDenied
	}
	return s.executeOwnerCommand(ctx, command, mutates)
}

func ownerCommandScope(command string) (bool, bool) {
	switch command {
	case "help", "help get", "help set", "help stats", "help region", "region", "region home", "region default", "ver", "board", "clock",
		"stats", "stats role", "stats help", "stats radio", "stats signal", "stats tx",
		"stats airtime", "stats admission", "stats sensors", "stats memory",
		"get name", "get owner.info", "get radio", "get freq", "get tx", "get stats",
		"get repeat", "get path.hash.mode", "get advert.interval", "get flood.advert.interval",
		"get rxdelay", "get txdelay", "get direct.txdelay", "get af", "get dutycycle":
		return false, true
	}
	if strings.HasPrefix(command, "region ") {
		parts := strings.Fields(command)
		if len(parts) < 2 {
			return false, false
		}
		switch parts[1] {
		case "save":
			return true, len(parts) == 2
		case "list":
			return false, len(parts) == 3 && (parts[2] == "allowed" || parts[2] == "denied")
		case "get":
			return false, len(parts) == 3
		case "home", "default", "allowf", "denyf", "remove":
			return true, len(parts) == 3
		case "put":
			return true, len(parts) == 3 || len(parts) == 4
		case "def":
			return true, len(parts) >= 3
		}
		return false, false
	}
	parts := strings.SplitN(command, " ", 3)
	if len(parts) != 3 || parts[0] != "set" {
		return false, false
	}
	switch parts[1] {
	case "name":
		return true, len(parts[2]) >= 1 && len(parts[2]) <= 31
	case "repeat":
		return true, parts[2] == "on" || parts[2] == "off"
	case "path.hash.mode", "advert.interval", "flood.advert.interval":
		if len(parts[2]) == 0 || len(parts[2]) > 3 {
			return false, false
		}
		for i := range parts[2] {
			if parts[2][i] < '0' || parts[2][i] > '9' {
				return false, false
			}
		}
		return true, true
	}
	return false, false
}

func (s *Service) executeOwnerCommand(ctx context.Context, command string, mutates bool) (string, error) {
	change := &ownerCommand{
		ctx: ctx, command: strings.Clone(command), mutates: mutates,
		result: make(chan ownerCommandResult, 1),
	}
	s.ingress.RLock()
	select {
	case <-s.done:
		s.ingress.RUnlock()
		return "", ErrRoleStopped
	default:
	}
	select {
	case s.queue <- event{ownerCommand: change}:
		s.ingress.RUnlock()
	default:
		s.ingress.RUnlock()
		return "", ErrQueueFull
	}
	select {
	case result := <-change.result:
		return result.reply, result.err
	case <-ctx.Done():
		select {
		case result := <-change.result:
			return result.reply, result.err
		default:
			if mutates {
				return "", fmt.Errorf("%w: %w", ErrOwnerCommandUnknown, ctx.Err())
			}
			return "", ctx.Err()
		}
	}
}
