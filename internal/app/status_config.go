package app

import (
	"time"

	"meshcore.local/meshcore/internal/radio"
)

type physicalSettings struct {
	FrequencyHz           uint32  `json:"frequency_hz"`
	BandwidthHz           uint32  `json:"bandwidth_hz"`
	SpreadingFactor       uint8   `json:"spreading_factor"`
	CodingRate            uint8   `json:"coding_rate"`
	TxPowerDBm            uint8   `json:"tx_power_dbm"`
	AirtimeFactor         float64 `json:"airtime_factor"`
	CADEnabled            bool    `json:"cad_enabled"`
	InterferenceThreshold int16   `json:"interference_threshold"`
}

type sharedPHYStatus struct {
	ConfigurationOwner     string `json:"configuration_owner"`
	PHYAuthority           string `json:"phy_authority"`
	PHYTracking            string `json:"phy_tracking"`
	ExternalClientCapacity int    `json:"external_client_capacity"`
	LocalSourceCapacity    int    `json:"local_source_capacity,omitempty"`
	CapacitySource         string `json:"capacity_source"`
	OwnerConnected         bool   `json:"owner_connected"`
	// Requested is the host configuration. With phy_tracking follow it is a
	// reference, not an expectation; effective is always the mast readback.
	Requested               physicalSettings    `json:"requested"`
	Effective               *physicalSettings   `json:"effective"`
	ConfigurationGeneration uint32              `json:"configuration_generation,omitempty"`
	EffectiveSince          *time.Time          `json:"effective_since,omitempty"`
	MatchesRequested        *bool               `json:"matches_requested,omitempty"`
	Transitions             uint64              `json:"transitions"`
	RecentTransitions       []phyTransitionView `json:"recent_transitions,omitempty"`
	Valid                   bool                `json:"valid"`
	Error                   string              `json:"error"`
}

type phyTransitionView struct {
	At             time.Time        `json:"at"`
	Cause          string           `json:"cause"`
	FromGeneration uint32           `json:"from_generation"`
	ToGeneration   uint32           `json:"to_generation"`
	From           physicalSettings `json:"from"`
	To             physicalSettings `json:"to"`
}

type retentionStatus struct {
	Requested string  `json:"requested"`
	Effective *string `json:"effective"`
	Valid     bool    `json:"valid"`
	Error     string  `json:"error"`
}

func settingsView(s radio.PHYSettings) physicalSettings {
	return physicalSettings{
		FrequencyHz: s.Radio.FreqHz, BandwidthHz: s.Radio.BwHz,
		SpreadingFactor: s.Radio.SF, CodingRate: s.Radio.CR, TxPowerDBm: s.TxPower,
		AirtimeFactor: s.Profile.AirtimeFactor, CADEnabled: s.Profile.CADEnabled,
		InterferenceThreshold: s.Profile.InterferenceThreshold,
	}
}

// sharedPHYStatus reports the configuration link's own readback. The HTTP
// handler performs no modem I/O; the link refreshes CONFIG on connection,
// every heartbeat and whenever a submission is rejected as stale.
func (c Config) sharedPHYStatus(phy radio.PHYStatus) sharedPHYStatus {
	owner := "observer"
	if !c.roleEnabled("observer") {
		owner = "controller"
	}
	authority := owner
	if c.PHYAuthority == "modem" {
		authority = "modem"
	}
	profile := physicalSettings{
		FrequencyHz: c.Radio.FreqHz, BandwidthHz: c.Radio.BwHz,
		SpreadingFactor: c.Radio.SF, CodingRate: c.Radio.CR, TxPowerDBm: c.TxPower,
		AirtimeFactor: 1,
	}
	if c.PHYProfile != nil {
		profile.AirtimeFactor = c.PHYProfile.AirtimeFactor
		profile.CADEnabled = c.PHYProfile.CADEnabled
		profile.InterferenceThreshold = c.PHYProfile.InterferenceThreshold
	}
	source := "configured"
	if c.RadioClientCapacity == nil {
		source = "legacy_default"
		if c.PHYAuthority == "modem" {
			source = "unverified"
		}
	}
	capacity := c.RadioCapacity()
	localSources := 0
	if c.radioCapacity != nil {
		capacity = c.radioCapacity.external
		localSources = c.radioCapacity.local
		source = c.radioCapacity.source
	}
	status := sharedPHYStatus{
		ConfigurationOwner: authority, PHYAuthority: string(c.PHYAuthority),
		PHYTracking:            c.radioPHYTracking().String(),
		ExternalClientCapacity: capacity, LocalSourceCapacity: localSources,
		CapacitySource: source, Requested: profile, Transitions: phy.TransitionCount,
	}
	for _, transition := range phy.Transitions {
		status.RecentTransitions = append(status.RecentTransitions, phyTransitionView{
			At: transition.At, Cause: transition.Cause,
			FromGeneration: transition.FromGeneration, ToGeneration: transition.ToGeneration,
			From: settingsView(transition.From), To: settingsView(transition.To),
		})
	}
	switch {
	case !c.RequireParity:
		status.Error = "queued PHY readback is unavailable in legacy mode"
	case !phy.Online || phy.Effective == nil:
		status.Error = owner + " radio disconnected; current PHY is unverified"
		if phy.Error != "" {
			status.Error += ": " + phy.Error
		}
	default:
		effective := settingsView(phy.Effective.Settings)
		// The wire carries the airtime factor as float32.
		requested := profile
		requested.AirtimeFactor = float64(float32(requested.AirtimeFactor))
		matches := effective == requested
		since := phy.Effective.Since
		status.Effective, status.MatchesRequested = &effective, &matches
		status.ConfigurationGeneration, status.EffectiveSince = phy.Effective.ConfigurationGeneration, &since
		status.OwnerConnected, status.Valid = true, true
	}
	return status
}

// ownerView is the configuration link as seen by the status handler.
type ownerView interface {
	PHYStatus() radio.PHYStatus
	AirtimeTable() []uint32
	Snapshot() (radio.Telemetry, error)
}

func (c Config) ownerStatus(started time.Time, link ownerView) roleStatus {
	status := link.PHYStatus()
	phy := c.sharedPHYStatus(status)
	online := status.Online
	entry := roleStatus{
		ApplicationStartedAt: &started, State: "running", RadioConnected: &online,
		SharedPHY: &phy, AirtimeMS: link.AirtimeTable(),
	}
	if status.Effective != nil {
		entry.PHYGeneration = status.Effective.ConfigurationGeneration
	}
	telemetry, err := link.Snapshot()
	if err != nil {
		entry.TelemetryError = err.Error()
	} else {
		entry.Telemetry = &telemetry
		entry.TelemetryCounterScope = "shared_physical_modem"
	}
	return entry
}

func (c Config) retentionStatusFor(role string, active bool) retentionStatus {
	retention := c.CompanionRetention
	if role == "bot_companion" {
		retention = c.BotCompanionRetention
	}
	status := retentionStatus{Requested: retention}
	if !active {
		status.Error = role + " application is not active"
		return status
	}
	status.Effective = &status.Requested
	status.Valid = true
	return status
}
