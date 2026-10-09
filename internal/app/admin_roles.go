package app

import (
	"context"
	"errors"
	"regexp"
	"sort"
	"strings"

	"meshcore.local/meshcore/internal/roles"
)

type hostRoleAdmin map[string]func(context.Context, string) (string, error)

type roleOwnerAdmin interface {
	OwnerAdmin(context.Context, string) (string, error)
}

var hostRoleRead = regexp.MustCompile(`^(?:help(?: (?:get|set|stats|region))?|region(?: (?:home|default|list (?:allowed|denied)|get [#a-zA-Z0-9*-]{1,30}))?|ver|board|clock|stats(?: (?:role|help|radio|signal|tx|airtime|admission|sensors|memory))?|get (?:name|owner\.info|radio|freq|tx|stats|repeat|path\.hash\.mode|advert\.interval|flood\.advert\.interval|rxdelay|txdelay|direct\.txdelay|af|dutycycle))$`)
var hostRoleWrite = regexp.MustCompile(`^(?:advert(?:\.zerohop)?|set (?:name [\x20-\x7e]{1,31}|repeat (?:on|off)|path\.hash\.mode [012]|advert\.interval [0-9]{1,3}|flood\.advert\.interval [0-9]{1,3})|region (?:save|(?:home|allowf|denyf|remove) [#a-zA-Z0-9*-]{1,30}|default (?:<null>|[#a-zA-Z0-9*-]{1,30})|put [#a-zA-Z0-9-]{1,30}(?: [#a-zA-Z0-9*-]{1,30})?|def [#a-zA-Z0-9*|,-]+(?: [#a-zA-Z0-9*|,-]+)*))$`)
var hostRoleError = regexp.MustCompile(`^(?:Error(?:[:, ]|$)|Err(?:[ -]|$))`)

func (a hostRoleAdmin) names() []string {
	names := make([]string, 0, len(a))
	for _, name := range []string{"repeater", "room"} {
		if a[name] != nil {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	return names
}

// The application supervisor owns replacement. Borrow its current service,
// then release the supervisor lock before queueing normal role processing.
func (w *roleWorker) ownerAdmin(ctx context.Context, command string) (string, error) {
	w.mu.Lock()
	current, available := w.current, w.phase == "running" && !w.pending
	w.mu.Unlock()
	if !available || current == nil {
		return "", errors.New("role is not running or is restarting")
	}
	if command == "get name" {
		return "> " + current.service.Name(), nil
	}
	if strings.HasPrefix(command, "set name ") {
		return current.service.ConfigureName(ctx, strings.TrimPrefix(command, "set name "))
	}
	owner, ok := any(current.service).(roleOwnerAdmin)
	if !ok {
		return "", errors.New("role owner command adapter is unavailable")
	}
	return owner.OwnerAdmin(ctx, command)
}

func newHostRoleAdmin(workers map[string]*roleWorker) hostRoleAdmin {
	admin := make(hostRoleAdmin)
	if _, ok := any((*roles.Service)(nil)).(roleOwnerAdmin); !ok {
		return admin
	}
	for _, name := range []string{"repeater", "room"} {
		if worker := workers[name]; worker != nil {
			admin[name] = worker.ownerAdmin
		}
	}
	return admin
}
