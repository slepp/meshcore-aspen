package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	"meshcore.local/meshcore/internal/radio"
)

type sessionCapacityReader struct{ session *radio.Session }

func (r sessionCapacityReader) ClientCapacity(context.Context) (radio.ClientCapacity, error) {
	return r.session.Capacity()
}

func (c Config) sessionRolePorts(logicalPorts int) map[string]int {
	ports := make(map[string]int)
	next := 1
	for _, role := range []string{"repeater", "room", "companion", "bot_companion", "bot"} {
		if role == "bot_companion" && c.BotCompanionListen == "" ||
			role == "bot" && c.BotRuntime != "native_lua" ||
			role != "bot_companion" && !c.roleEnabled(role) {
			continue
		}

		if next < logicalPorts {
			ports[role] = next
			next++
		}
	}
	return ports
}

func (c Config) openOwnerRadio(ctx context.Context, config radio.Config, logger *slog.Logger) (*radio.Link, *radio.Session, error) {
	var session *radio.Session
	if c.RadioSession == "auto" || c.RadioSession == "required" {
		minSlots := c.reservedClients(true)
		if c.RadioClientCapacity != nil {
			minSlots = max(minSlots, *c.RadioClientCapacity)
		}
		session = radio.NewSessionWithPorts(c.RadioAddress, minSlots, c.sessionMinPorts())
		config.Session = session
	}
	link, err := radio.Open(ctx, config)
	if (errors.Is(err, radio.ErrSubportsUnavailable) ||
		errors.Is(err, radio.ErrSubportsProbeTimeout) ||
		errors.Is(err, radio.ErrSubportsInsufficient)) && c.RadioSession == "auto" {
		if c.reservedClients(false) > c.RadioCapacity() {
			return nil, nil, fmt.Errorf("KISS session unavailable: %w; per-role fallback needs %d connections, configured limit is %d",
				err, c.reservedClients(false), c.RadioCapacity())
		}
		logger.Warn("KISS subports unavailable; using independent TCP connections", "error", err)
		session, config.Session = nil, nil
		link, err = radio.Open(ctx, config)
	}
	if err != nil {
		return nil, nil, err
	}
	return link, session, nil
}
