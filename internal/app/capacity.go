package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"meshcore.local/meshcore/internal/radio"
)

type capacityEvidence struct {
	external int
	local    int
	ports    int
	source   string
}

type capacityReader interface {
	ClientCapacity(context.Context) (radio.ClientCapacity, error)
}

func (c Config) checkRadioCapacity(ctx context.Context, link capacityReader, logger *slog.Logger) (capacityEvidence, error) {
	if !c.RequireParity {
		source := "configured"
		if c.RadioClientCapacity == nil {
			source = "legacy_default"
		}
		return capacityEvidence{external: c.RadioCapacity(), source: source}, nil
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	reply, err := link.ClientCapacity(ctx)
	if errors.Is(err, radio.ErrCapacityUnavailable) {
		source := "configured"
		if c.RadioClientCapacity == nil {
			source = "legacy_default"
		}
		logger.Warn("modem CAPACITY unavailable; physical connection limit unverified",
			"assumed_limit", c.RadioCapacity(), "source", source)
		return capacityEvidence{external: c.RadioCapacity(), source: source}, nil
	}
	if err != nil {
		return capacityEvidence{}, fmt.Errorf("radio CAPACITY: %w", err)
	}
	reported := int(reply.ExternalTCPSlots)
	if c.RadioClientCapacity != nil && reported < *c.RadioClientCapacity {
		return capacityEvidence{}, fmt.Errorf("radio CAPACITY reports %d external slots, below configured radio_client_capacity %d", reported, *c.RadioClientCapacity)
	}
	effective := min(reported, c.RadioCapacity())
	required := c.RadioSourceCount()
	if c.sessionActive {
		if reply.LogicalPorts == 0 {
			return capacityEvidence{}, errors.New("negotiated radio session omitted logical port capacity")
		}
		required = c.sessionReservedClientsFor(int(reply.LogicalPorts))
	} else if c.roleEnabled("bot") && c.BotRuntime != "native_lua" {
		required += c.BotMaxClients
	}
	if required > effective {
		return capacityEvidence{}, fmt.Errorf("%d reserved radio clients exceed reported external capacity %d", required, effective)
	}
	logger.Info("radio client capacity verified", "external_slots", reported, "local_sources", reply.LocalSourceSlots,
		"logical_ports", reply.LogicalPorts, "reserved", required, "limit", effective)
	return capacityEvidence{external: effective, local: int(reply.LocalSourceSlots), ports: int(reply.LogicalPorts), source: "reported"}, nil
}
