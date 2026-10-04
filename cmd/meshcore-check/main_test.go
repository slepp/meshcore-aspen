package main

import (
	"fmt"
	"strings"
	"testing"

	"github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/app"
)

func selectedStatus(cfg app.Config) map[string]roleStatus {
	connected := true
	status := make(map[string]roleStatus)
	for index, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
		if !cfg.RoleEnabled(role) {
			status[role] = roleStatus{State: "disabled"}
			continue
		}
		status[role] = roleStatus{
			PublicKey: fmt.Sprintf("%064x", index+1), Connected: &connected,
			State: "running", MQTTConnected: &connected,
		}
	}
	owner := "observer"
	if !cfg.RoleEnabled("observer") {
		owner = "controller"
		status[owner] = roleStatus{State: "running", Connected: &connected}
	}
	entry := status[owner]
	entry.AirtimeMS = make([]uint32, hardware.KISS_MAX_PACKET_SIZE+1)
	authority := owner
	if cfg.PHYAuthority == "modem" {
		authority = "modem"
	}
	tracking := "fixed"
	if cfg.FollowsModemPHY() {
		tracking = "follow"
	}
	entry.SharedPHY = &sharedPHYStatus{
		ConfigurationOwner: authority, PHYAuthority: string(cfg.PHYAuthority), PHYTracking: tracking,
		ExternalClientCapacity: cfg.RadioCapacity(), CapacitySource: "configured",
		OwnerConnected: true, Valid: true,
	}
	entry.SharedPHY.Effective = &effectivePHY{
		FrequencyHz: cfg.Radio.FreqHz, BandwidthHz: cfg.Radio.BwHz, SpreadingFactor: cfg.Radio.SF,
		CodingRate: cfg.Radio.CR, TxPowerDBm: cfg.TxPower, AirtimeFactor: 1,
	}
	status[owner] = entry
	if cfg.BotCompanionListen != "" {
		status["bot_companion"] = roleStatus{PublicKey: fmt.Sprintf("%064x", 6), Connected: &connected, State: "running"}
	}
	return status
}

func TestSelectiveHostStatusChecksOwnerAndOnlyEnabledIdentities(t *testing.T) {
	for _, tc := range []struct {
		name  string
		roles []string
		bot   bool
		want  int
	}{
		{"legacy", nil, false, 5},
		{"repeater-companion", []string{"repeater", "companion"}, false, 2},
		{"repeater-room", []string{"repeater", "room"}, false, 2},
		{"repeater-observer", []string{"repeater", "observer"}, false, 2},
		{"controller-only", []string{}, false, 0},
		{"bot-companion-without-base", []string{}, true, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := app.DefaultConfig()
			cfg.EnabledRoles = app.RoleSelection(tc.roles)
			if tc.bot {
				cfg.BotCompanionListen = "127.0.0.1:5001"
			}
			status := selectedStatus(cfg)
			got, err := checkRoleStatus(cfg, checks{}, status)
			if err != nil || got != tc.want {
				t.Fatalf("identity count=%d, want %d: %v", got, tc.want, err)
			}
			owner := "controller"
			if cfg.RoleEnabled("observer") {
				owner = "observer"
			}
			delete(status, owner)
			if _, err := checkRoleStatus(cfg, checks{}, status); err == nil {
				t.Fatal("missing physical configuration owner accepted")
			}
		})
	}
}

func TestSelectiveHostStatusRejectsPhantomPublisherAndDisabledRole(t *testing.T) {
	cfg := app.DefaultConfig()
	cfg.EnabledRoles = []string{"repeater", "observer"}
	status := selectedStatus(cfg)
	observer := status["observer"]
	disconnected := false
	observer.MQTTConnected = &disconnected
	status["observer"] = observer
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil || !strings.Contains(err.Error(), "MQTT") {
		t.Fatalf("missing publisher accepted: %v", err)
	}
	cfg.EnabledRoles = []string{"repeater"}
	status = selectedStatus(cfg)
	status["observer"] = observer
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil || !strings.Contains(err.Error(), "disabled") {
		t.Fatalf("unexpected observer identity accepted: %v", err)
	}
	status = selectedStatus(cfg)
	controller := status["controller"]
	controller.AirtimeMS = nil
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil || !strings.Contains(err.Error(), "airtime") {
		t.Fatalf("missing owner airtime model accepted: %v", err)
	}
	status = selectedStatus(cfg)
	controller = status["controller"]
	controller.SharedPHY.Valid = false
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil || !strings.Contains(err.Error(), "shared PHY") {
		t.Fatalf("unverified physical profile accepted: %v", err)
	}
}

func TestModemAuthorityReadinessRejectsUnverifiedOrMisownedPHY(t *testing.T) {
	cfg := app.DefaultConfig()
	cfg.PHYAuthority = "modem"
	capacity := 4
	cfg.RadioClientCapacity = &capacity
	cfg.EnabledRoles = []string{"repeater"}
	status := selectedStatus(cfg)
	if _, err := checkRoleStatus(cfg, checks{}, status); err != nil {
		t.Fatalf("valid modem-owned PHY rejected: %v", err)
	}
	controller := status["controller"]
	controller.SharedPHY.ConfigurationOwner = "controller"
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil {
		t.Fatal("host ownership reported for modem authority was accepted")
	}
	status = selectedStatus(cfg)
	controller = status["controller"]
	controller.SharedPHY.Effective = nil
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil {
		t.Fatal("missing effective modem PHY was accepted")
	}
	status = selectedStatus(cfg)
	controller = status["controller"]
	controller.SharedPHY.ExternalClientCapacity = 1
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil {
		t.Fatal("reported capacity below reserved sources was accepted")
	}
	cfg.EnabledRoles = []string{"repeater", "room", "companion"}
	if err := checkSelection(cfg, checks{airtime: true}); err == nil {
		t.Fatal("airtime probe would exceed the four-client mast connection budget")
	}
	cfg.RadioClientCapacity = nil
	cfg.EnabledRoles = []string{"repeater", "room"}
	status = selectedStatus(cfg)
	controller = status["controller"]
	controller.SharedPHY.ExternalClientCapacity = 3
	controller.SharedPHY.CapacitySource = "reported"
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err != nil {
		t.Fatal(err)
	}
	controller.SharedPHY.CapacitySource = "configured"
	status["controller"] = controller
	if _, err := checkRoleStatus(cfg, checks{}, status); err == nil {
		t.Fatal("query-only modem authority claimed an unqueried capacity")
	}
}

func TestSelectiveCheckOptionsRequireTheirEndpoints(t *testing.T) {
	cfg := app.DefaultConfig()
	cfg.EnabledRoles = []string{"repeater", "companion"}
	for _, selected := range []checks{
		{reboot: true}, {factoryReset: true},
		{rebootRoles: true}, {rekeyRoles: true}, {transmit: true},
	} {
		if err := checkSelection(cfg, selected); err == nil {
			t.Fatalf("optional check %+v silently skipped unavailable endpoints", selected)
		}
	}
	if err := checkSelection(cfg, checks{airtime: true}); err != nil {
		t.Fatalf("physical airtime measurement unnecessarily required bot KISS: %v", err)
	}
	cfg.EnabledRoles = []string{"repeater", "companion", "bot"}
	if err := checkSelection(cfg, checks{rebootRoles: true}); err != nil {
		t.Fatalf("repeater-only restart check rejected: %v", err)
	}
	if err := checkSelection(cfg, checks{airtime: true}); err != nil {
		t.Fatalf("bot airtime check rejected: %v", err)
	}
	if err := checkSelection(cfg, checks{onchip: true, transmit: true}); err != nil {
		t.Fatalf("on-chip roles incorrectly constrained by host selection: %v", err)
	}
}

func TestOnchipCheckUsesBootProfileRatherThanHostRoles(t *testing.T) {
	cfg := app.DefaultConfig()
	cfg.EnabledRoles = []string{"repeater", "room"}
	status := selectedStatus(app.DefaultConfig())
	for _, role := range []string{"room", "observer", "companion"} {
		status[role] = roleStatus{State: "disabled"}
	}
	ready := true
	status["management"] = roleStatus{
		PublicKey: fmt.Sprintf("%064x", 7), State: "running", Connected: &ready,
	}
	selected := checks{onchip: true}
	selected.setOnchipRoles(status)
	count, err := checkRoleStatus(cfg, selected, status)
	if err != nil || count != 3 {
		t.Fatalf("standalone repeater, KISS and management should be ready: %d, %v", count, err)
	}
	if err := checkSelection(cfg, selected); err != nil {
		t.Fatalf("repeater-only readiness rejected: %v", err)
	}
	if err := checkSelection(cfg, checks{
		onchip: true, onchipRoles: selected.onchipRoles, rebootRoles: true,
	}); err == nil || !strings.Contains(err.Error(), "companion") {
		t.Fatalf("reboot check used a disabled companion: %v", err)
	}
	entry := status["room"]
	entry.PublicKey = fmt.Sprintf("%064x", 3)
	status["room"] = entry
	if _, err := checkRoleStatus(cfg, selected, status); err == nil {
		t.Fatal("disabled standalone room reported an active identity")
	}
	status["room"] = roleStatus{State: "disabled"}
	delete(status, "observer")
	if _, err := checkRoleStatus(cfg, selected, status); err == nil {
		t.Fatal("missing standalone observer status accepted")
	}
}

func TestOnchipManagementReadinessRequiresProvisioning(t *testing.T) {
	cfg := app.DefaultConfig()
	status := selectedStatus(cfg)
	selected := checks{onchip: true}
	selected.setOnchipRoles(status)
	if _, err := checkRoleStatus(cfg, selected, status); err == nil {
		t.Fatal("standalone status without a management identity accepted")
	}
	ready := true
	status["management"] = roleStatus{
		PublicKey: fmt.Sprintf("%064x", 6), State: "unprovisioned", Connected: &ready,
	}
	if _, err := checkRoleStatus(cfg, selected, status); err == nil {
		t.Fatal("unprovisioned RF management accepted")
	}
	mgmt := status["management"]
	mgmt.State = "running"
	status["management"] = mgmt
	if count, err := checkRoleStatus(cfg, selected, status); err != nil || count != 6 {
		t.Fatalf("provisioned standalone identity rejected: count=%d, %v", count, err)
	}
}

func TestSharedPHYReferenceFollowsOnlyValidLiveProfiles(t *testing.T) {
	retune := func(phy *sharedPHYStatus) {
		phy.Effective.SpreadingFactor, phy.Effective.TxPowerDBm = 9, 14
	}
	for _, tc := range []struct {
		name, authority, tracking string
		mutate                    func(*sharedPHYStatus)
		want                      string
	}{
		{"host-matching", "host", "", func(*sharedPHYStatus) {}, ""},
		{"host-retuned", "host", "", retune, "differs from the fixed"},
		{"fixed-retuned", "modem", "fixed", retune, "differs from the fixed"},
		{"follow-retuned", "modem", "", retune, ""},
		{"follow-older-host", "modem", "", func(phy *sharedPHYStatus) { phy.PHYTracking = "" }, "phy_tracking"},
		{"follow-invalid", "modem", "", func(phy *sharedPHYStatus) { phy.Effective.SpreadingFactor = 13 }, "invalid"},
		{"follow-bad-power", "modem", "", func(phy *sharedPHYStatus) { phy.Effective.TxPowerDBm = 31 }, "invalid"},
		{"follow-offline", "modem", "", func(phy *sharedPHYStatus) { phy.Valid, phy.Effective = false, nil }, "unverified"},
		{"fixed-reported-follow", "modem", "fixed", func(phy *sharedPHYStatus) { phy.PHYTracking = "follow" }, "phy_tracking"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := app.DefaultConfig()
			cfg.PHYAuthority, cfg.PHYTracking = app.PHYAuthority(tc.authority), tc.tracking
			capacity := 8
			cfg.RadioClientCapacity = &capacity
			cfg.EnabledRoles = []string{"repeater"}
			status := selectedStatus(cfg)
			tc.mutate(status["controller"].SharedPHY)
			reference, err := sharedPHYReference(cfg, status["controller"].SharedPHY)
			_, statusErr := checkRoleStatus(cfg, checks{}, status)
			if tc.want == "" {
				if err != nil || statusErr != nil {
					t.Fatalf("valid shared PHY rejected: %v / %v", err, statusErr)
				}
				want := cfg.Radio
				if tc.name == "follow-retuned" {
					want.SF = 9
				}
				if reference != want {
					t.Fatalf("reference %+v, want %+v", reference, want)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.want) || statusErr == nil ||
				!strings.Contains(statusErr.Error(), tc.want) {
				t.Fatalf("errors %v / %v, want %q", err, statusErr, tc.want)
			}
		})
	}
}

func TestCompanionSelfInfoMustMatchSharedPHYReference(t *testing.T) {
	reference := hardware.RadioConfig{FreqHz: 915000000, BwHz: 250000, SF: 9, CR: 8}
	info := companion.SelfInfoResponse{RadioFrequency: 915000, RadioBandwidth: 250000, RadioSpreadFactor: 9, RadioCodingRate: 8}
	if !selfInfoMatches(info, reference) {
		t.Fatal("live SELF_INFO rejected")
	}
	info.RadioSpreadFactor = 7
	if selfInfoMatches(info, reference) {
		t.Fatal("stale SELF_INFO accepted")
	}
}
