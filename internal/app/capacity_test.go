package app

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"strings"
	"testing"

	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/radio"
)

type legacyCapacityReader struct{ probes int }

func (reader *legacyCapacityReader) ClientCapacity(context.Context) (radio.ClientCapacity, error) {
	reader.probes++
	return radio.ClientCapacity{}, errors.New("legacy connection cannot negotiate CAPACITY")
}

func TestLegacyRadioDoesNotProbeQueuedCapacity(t *testing.T) {
	cfg := DefaultConfig()
	cfg.RequireParity = false
	reader := &legacyCapacityReader{}
	for _, tc := range []struct {
		limit, want int
		source      string
	}{
		{0, 8, "legacy_default"},
		{4, 4, "configured"},
	} {
		if tc.limit != 0 {
			cfg.RadioClientCapacity = &tc.limit
		}
		result, err := cfg.checkRadioCapacity(context.Background(), reader, slog.Default())
		if err != nil || result.external != tc.want || result.source != tc.source || reader.probes != 0 {
			t.Fatalf("legacy capacity negotiation changed: %+v, %v, probes=%d", result, err, reader.probes)
		}
	}
}

func openCapacityLink(t *testing.T, cfg Config) *radio.Link {
	t.Helper()
	link, err := radio.Open(context.Background(), cfg.configurationLink("controller", slog.Default(), nil, nil))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = link.Close() })
	return link
}

func TestRadioCapacityProbeHonorsReportedFourSlotMast(t *testing.T) {
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	cfg.EnabledRoles = []string{"repeater", "room"}
	mast := newAuthorityMast(t, cfg)
	mast.externalSlots, mast.localSlots = 4, 3
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	link := openCapacityLink(t, cfg)
	evidence, err := cfg.checkRadioCapacity(context.Background(), link, logger)
	if err != nil || evidence.external != 4 || evidence.local != 3 || evidence.source != "reported" {
		t.Fatalf("CAPACITY evidence lost: %+v, %v", evidence, err)
	}
	mast.mu.Lock()
	requests, sets := mast.capacityRequests, mast.sets
	owners := append([]byte(nil), mast.owners...)
	mast.mu.Unlock()
	if requests != 1 || sets != 0 || len(owners) != 1 || owners[0] != 0 {
		t.Fatalf("probe changed PHY authority: CAPACITY=%d SET=%d HELLO=%v", requests, sets, owners)
	}
	mast.mu.Lock()
	mast.externalSlots = 2
	mast.mu.Unlock()
	if _, err := cfg.checkRadioCapacity(context.Background(), link, logger); err == nil ||
		!strings.Contains(err.Error(), "reserved radio clients") {
		t.Fatalf("overbooked mast accepted: %v", err)
	}
	capacity := 4
	cfg.RadioClientCapacity = &capacity
	if _, err := cfg.checkRadioCapacity(context.Background(), link, logger); err == nil ||
		!strings.Contains(err.Error(), "below configured") {
		t.Fatalf("false configured capacity accepted: %v", err)
	}
}

func TestCapacityProbeLegacyFallbackAndModemAuthorityWarning(t *testing.T) {
	cfg := DefaultConfig()
	mast := newAuthorityMast(t, cfg)
	mast.capacitySupported = false
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	hostLink := openCapacityLink(t, cfg)
	evidence, err := cfg.checkRadioCapacity(context.Background(), hostLink, logger)
	if err != nil || evidence.external != 8 || evidence.source != "legacy_default" {
		t.Fatalf("legacy host authority lost eight-slot fallback: %+v, %v", evidence, err)
	}
	cfg.PHYAuthority = "modem"
	cfg.EnabledRoles = []string{"repeater", "room"}
	if err := cfg.Validate(); err != nil {
		t.Fatalf("query-only modem authority config rejected: %v", err)
	}
	modemLink := openCapacityLink(t, cfg)
	evidence, err = cfg.checkRadioCapacity(context.Background(), modemLink, logger)
	if err != nil || evidence.external != 4 || evidence.source != "legacy_default" {
		t.Fatalf("modem authority did not preserve older radio compatibility: %+v, %v", evidence, err)
	}
	capacity := 4
	cfg.RadioClientCapacity = &capacity
	evidence, err = cfg.checkRadioCapacity(context.Background(), modemLink, logger)
	if err != nil || evidence.external != 4 || evidence.source != "configured" {
		t.Fatalf("explicit modem budget rejected by older firmware: %+v, %v", evidence, err)
	}
	mast.mu.Lock()
	sets, requests := mast.sets, mast.capacityRequests
	owners := append([]byte(nil), mast.owners...)
	mast.unsupportedCode = hardware.HW_ERR_NO_CALLBACK
	mast.mu.Unlock()
	if sets != 1 || requests != 3 {
		t.Fatalf("capacity checks retuned or skipped the modem: SET=%d CAPACITY=%d", sets, requests)
	}
	if len(owners) != 2 || owners[0] != 1 || owners[1] != 0 {
		t.Fatalf("CAPACITY changed owner claims: %v", owners)
	}
	evidence, err = cfg.checkRadioCapacity(context.Background(), modemLink, logger)
	if err != nil || evidence.source != "configured" {
		t.Fatalf("old NO_CALLBACK firmware rejected explicit modem budget: %+v, %v", evidence, err)
	}
}

func TestCapacityProbeRejectsMalformedResponse(t *testing.T) {
	for _, tc := range []struct {
		name      string
		external  byte
		version   byte
		malformed bool
	}{
		{"zero", 0, radio.QueuedProtocolVersion, false},
		{"short", 4, radio.QueuedProtocolVersion, true},
		{"version", 4, 2, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := DefaultConfig()
			mast := newAuthorityMast(t, cfg)
			mast.externalSlots = tc.external
			mast.capacityVersion = tc.version
			mast.malformedCapacity = tc.malformed
			cfg.RadioAddress = mast.listener.Addr().String()
			link := openCapacityLink(t, cfg)
			if _, err := cfg.checkRadioCapacity(context.Background(), link, slog.Default()); err == nil ||
				!strings.Contains(err.Error(), "invalid queued CAPACITY response") {
				t.Fatalf("invalid response was treated as legacy fallback: %v", err)
			}
		})
	}
}
