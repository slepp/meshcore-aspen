package app

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"unicode/utf8"

	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/nodebackup"
	"meshcore.local/meshcore/internal/policy"
	"meshcore.local/meshcore/internal/radio"
)

type Config struct {
	RadioAddress          string               `json:"radio_address"`
	Radio                 hardware.RadioConfig `json:"radio"`
	TxPower               uint8                `json:"tx_power"`
	RequireParity         bool                 `json:"require_parity"`
	PHYProfile            *PHYProfile          `json:"phy_profile,omitempty"`
	PHYAuthority          PHYAuthority         `json:"phy_authority"`
	PHYTracking           string               `json:"phy_tracking,omitempty"`
	RadioClientCapacity   *int                 `json:"radio_client_capacity,omitempty"`
	RadioSession          RadioSessionMode     `json:"radio_session,omitempty"`
	StateDir              string               `json:"state_dir"`
	EnabledRoles          RoleSelection        `json:"enabled_roles,omitempty"`
	CompanionListen       string               `json:"companion_listen"`
	CompanionAllowRemote  bool                 `json:"companion_allow_remote,omitempty"`
	BotCompanionListen    string               `json:"bot_companion_listen,omitempty"`
	BotListen             string               `json:"bot_listen"`
	BotMaxClients         int                  `json:"bot_max_clients"`
	BotRuntime            string               `json:"bot_runtime,omitempty"`
	BotNativeWorker       string               `json:"bot_native_worker,omitempty"`
	StatusListen          string               `json:"status_listen"`
	AdminHTTPTokenEnv     string               `json:"admin_http_token_env,omitempty"`
	RepeaterName          string               `json:"repeater_name"`
	RoomName              string               `json:"room_name"`
	CompanionName         string               `json:"companion_name"`
	BotCompanionName      string               `json:"bot_companion_name"`
	RoomPassword          string               `json:"room_password"`
	RoomPasswordEnv       string               `json:"room_password_env"`
	RoomPublic            bool                 `json:"room_public,omitempty"`
	AdminPasswordEnv      string               `json:"admin_password_env"`
	AdvertIntervalSecs    *uint32              `json:"advert_interval_seconds,omitempty"`
	RepeaterPolicy        policy.Overrides     `json:"repeater_policy"`
	RoomPolicy            policy.Overrides     `json:"room_policy"`
	CompanionPolicy       policy.Overrides     `json:"companion_policy"`
	CompanionRetention    string               `json:"companion_retention"`
	BotCompanionPolicy    policy.Overrides     `json:"bot_companion_policy"`
	BotCompanionRetention string               `json:"bot_companion_retention"`
	CompanionKeyExport    bool                 `json:"companion_key_export"`
	CompanionKeyImport    bool                 `json:"companion_key_import"`
	BotCompanionKeyImport bool                 `json:"bot_companion_key_import"`
	CompanionFactoryReset bool                 `json:"companion_factory_reset"`
	RoleKeyImport         bool                 `json:"role_key_import"`
	MQTT                  MQTTConfig           `json:"mqtt"`
	radioCapacity         *capacityEvidence
	phyGroup              *radio.PHYGroup
	sessionActive         bool
	nodeBackup            *nodebackup.Service
}

// RoleSelection distinguishes an omitted selection from an explicit empty list.
type RoleSelection []string

func (r *RoleSelection) UnmarshalJSON(data []byte) error {
	if bytes.Equal(bytes.TrimSpace(data), []byte("null")) {
		return errors.New("enabled_roles must be a list, not null")
	}
	return json.Unmarshal(data, (*[]string)(r))
}

type PHYAuthority string

type RadioSessionMode string

func (m *RadioSessionMode) UnmarshalJSON(data []byte) error {
	if bytes.Equal(bytes.TrimSpace(data), []byte("null")) {
		return errors.New("radio_session must be per_role, auto or required, not null")
	}
	var value string
	if err := json.Unmarshal(data, &value); err != nil {
		return err
	}
	*m = RadioSessionMode(value)
	return nil
}

func (a *PHYAuthority) UnmarshalJSON(data []byte) error {
	if bytes.Equal(bytes.TrimSpace(data), []byte("null")) {
		return errors.New("phy_authority must be host or modem, not null")
	}
	var value string
	if err := json.Unmarshal(data, &value); err != nil {
		return err
	}
	*a = PHYAuthority(value)
	return nil
}

type MQTTConfig struct {
	URL          string  `json:"url"`
	BrokerListen string  `json:"broker_listen"`
	TopicPrefix  string  `json:"topic_prefix"`
	UsernameEnv  string  `json:"username_env"`
	PasswordEnv  string  `json:"password_env"`
	Format       string  `json:"format,omitempty"`
	IATA         string  `json:"iata,omitempty"`
	Origin       string  `json:"origin,omitempty"`
	Audience     string  `json:"audience,omitempty"`
	PacketFilter *uint16 `json:"packet_filter,omitempty"`
}

type PHYProfile struct {
	AirtimeFactor         float64 `json:"airtime_factor"`
	CADEnabled            bool    `json:"cad_enabled"`
	InterferenceThreshold int16   `json:"interference_threshold"`
}

func DefaultConfig() Config {
	return Config{
		RadioAddress: "meshcore-radio.local:8001",
		Radio:        hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5},
		TxPower:      22, RequireParity: true, PHYAuthority: "host", StateDir: ".state/host",
		PHYProfile:      &PHYProfile{AirtimeFactor: 1, CADEnabled: true},
		RadioSession:    "per_role",
		CompanionListen: "127.0.0.1:5000", BotListen: "127.0.0.1:8105",
		BotMaxClients: 2, StatusListen: "127.0.0.1:9080",
		BotRuntime:   "kiss_proxy",
		RepeaterName: "Shared Radio Repeater", RoomName: "Shared Radio Room",
		CompanionName: "Shared Radio Base", CompanionRetention: "durable_replay",
		BotCompanionName: "Shared Radio Bot", BotCompanionRetention: "durable_replay",
		MQTT: MQTTConfig{URL: "tcp://127.0.0.1:1883", BrokerListen: "127.0.0.1:1883", TopicPrefix: "meshcore"},
	}
}

func LoadConfig(path string) (Config, error) {
	cfg := DefaultConfig()
	file, err := os.Open(path)
	if err != nil {
		return cfg, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return cfg, err
	}
	if info.Size() > 64*1024 {
		return cfg, errors.New("configuration exceeds 64 KiB")
	}
	decoder := json.NewDecoder(io.LimitReader(file, 64*1024))
	decoder.DisallowUnknownFields()
	object := &cfg
	if err := decoder.Decode(&object); err != nil {
		return cfg, err
	}
	if object == nil {
		return cfg, errors.New("configuration must be a JSON object, not null")
	}
	var extra any
	if err := decoder.Decode(&extra); !errors.Is(err, io.EOF) {
		return cfg, errors.New("configuration must contain exactly one JSON object")
	}
	if err := cfg.Validate(); err != nil {
		return cfg, err
	}
	if !filepath.IsAbs(cfg.StateDir) {
		cfg.StateDir = filepath.Join(filepath.Dir(path), cfg.StateDir)
	}
	cfg.StateDir, err = filepath.Abs(cfg.StateDir)
	if err != nil {
		return cfg, fmt.Errorf("state_dir: %w", err)
	}
	return cfg, nil
}

func (c Config) Preflight(brokerOnly bool) error {
	if err := c.Validate(); err != nil {
		return err
	}
	if brokerOnly {
		if c.MQTT.BrokerListen == "" {
			return errors.New("mqtt.broker_listen is required for broker-only mode")
		}
	} else {
		if _, err := c.roomAccessPassword(); err != nil {
			return err
		}
		if c.roleEnabled("room") || c.roleEnabled("repeater") {
			if _, err := envSecret(c.AdminPasswordEnv); err != nil {
				return fmt.Errorf("role administrator password: %w", err)
			}
		}
		if !filepath.IsAbs(c.StateDir) {
			return errors.New("state_dir must be an absolute path")
		}
		if c.roleEnabled("bot") && c.BotRuntime == "native_lua" {
			info, err := os.Stat(c.BotNativeWorker)
			if err != nil {
				return fmt.Errorf("bot_native_worker %s: %w", c.BotNativeWorker, err)
			}
			if !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
				return fmt.Errorf("bot_native_worker %s must be an executable file", c.BotNativeWorker)
			}
		}
	}
	if brokerOnly || c.roleEnabled("observer") {
		for _, name := range []string{c.MQTT.UsernameEnv, c.MQTT.PasswordEnv} {
			if _, err := envSecret(name); err != nil {
				return fmt.Errorf("MQTT credentials: %w", err)
			}
		}
	}
	return nil
}

func (c Config) Validate() error {
	if c.AdminHTTPTokenEnv == "" && os.Getenv("MESHCORE_HOST_ADMIN_HTTP") == "1" &&
		(c.AdminPasswordEnv == "" || !hostAdminPassword.MatchString(os.Getenv(c.AdminPasswordEnv))) {
		return errors.New("MESHCORE_HOST_ADMIN_HTTP=1 requires configured admin_password_env with a 12..256-byte printable ASCII owner password without whitespace")
	}
	if c.AdminHTTPTokenEnv != "" {
		if !regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`).MatchString(c.AdminHTTPTokenEnv) {
			return errors.New("admin_http_token_env must name an environment variable")
		}
		value := os.Getenv(c.AdminHTTPTokenEnv)
		if !hostAdminToken.MatchString(value) {
			return errors.New("admin_http_token_env must contain a separate 32..256-byte printable ASCII admin token without whitespace")
		}
	}
	switch c.RadioSession {
	case "", "per_role":
	case "auto", "required":
		if !c.RequireParity {
			return errors.New("radio_session=auto or required requires queued parity mode")
		}
	default:
		return errors.New("radio_session must be per_role, auto or required")
	}
	if c.PHYAuthority != "host" && c.PHYAuthority != "modem" {
		return errors.New("phy_authority must be host or modem")
	}
	if c.PHYAuthority == "modem" && !c.RequireParity {
		return errors.New("phy_authority=modem requires readback-only queued parity mode")
	}
	switch c.PHYTracking {
	case "", "fixed":
	case "follow":
		if c.PHYAuthority != "modem" {
			return errors.New("phy_tracking=follow requires phy_authority=modem")
		}
	default:
		return errors.New("phy_tracking must be follow or fixed")
	}
	known := map[string]bool{"repeater": true, "room": true, "companion": true, "observer": true, "bot": true}
	seen := make(map[string]bool)
	for _, role := range c.EnabledRoles {
		if !known[role] {
			return fmt.Errorf("enabled_roles: unknown role %q", role)
		}
		if seen[role] {
			return fmt.Errorf("enabled_roles: duplicate role %q", role)
		}
		seen[role] = true
	}
	if _, _, err := net.SplitHostPort(c.RadioAddress); err != nil {
		return fmt.Errorf("radio_address: %w", err)
	}
	listeners := []struct{ name, address string }{{"status_listen", c.StatusListen}}
	if c.roleEnabled("companion") {
		listeners = append(listeners, struct{ name, address string }{"companion_listen", c.CompanionListen})
	}
	if c.roleEnabled("bot") && c.BotRuntime != "native_lua" {
		listeners = append(listeners, struct{ name, address string }{"bot_listen", c.BotListen})
	}
	if c.BotCompanionListen != "" {
		listeners = append(listeners, struct{ name, address string }{"bot_companion_listen", c.BotCompanionListen})
	}
	for _, listener := range listeners {
		if listener.name != "companion_listen" && listener.name != "bot_companion_listen" {
			continue
		}
		host, _, err := net.SplitHostPort(listener.address)
		if err != nil {
			return fmt.Errorf("%s: %w", listener.name, err)
		}
		ip := net.ParseIP(host)
		if (ip == nil || !ip.IsLoopback()) && !strings.EqualFold(host, "localhost") && !c.CompanionAllowRemote {
			return fmt.Errorf("%s: unauthenticated remote access requires companion_allow_remote=true; use a loopback address for local access", listener.name)
		}
	}
	if c.roleEnabled("observer") && c.MQTT.BrokerListen != "" {
		listeners = append(listeners, struct{ name, address string }{"mqtt.broker_listen", c.MQTT.BrokerListen})
	}
	ports := make(map[int]string)
	for _, listener := range listeners {
		_, service, err := net.SplitHostPort(listener.address)
		if err != nil {
			return fmt.Errorf("%s: %w", listener.name, err)
		}
		if service == "" {
			return fmt.Errorf("%s: TCP port must not be empty", listener.name)
		}
		port, err := net.LookupPort("tcp", service)
		if err != nil {
			return fmt.Errorf("%s: %w", listener.name, err)
		}
		if port != 0 {
			if prior := ports[port]; prior != "" {
				return fmt.Errorf("%s and %s cannot share TCP port %d", prior, listener.name, port)
			}
			ports[port] = listener.name
		}
	}
	if c.StateDir == "" {
		return errors.New("state_dir must not be empty")
	}
	switch c.BotRuntime {
	case "", "kiss_proxy":
		if c.BotNativeWorker != "" {
			return errors.New("bot_native_worker requires bot_runtime=native_lua")
		}
	case "native_lua":
		if !c.roleEnabled("bot") {
			return errors.New("bot_runtime=native_lua requires the bot role")
		}
		if !c.RequireParity {
			return errors.New("bot_runtime=native_lua requires queued PHY parity")
		}
		if !filepath.IsAbs(c.BotNativeWorker) {
			return errors.New("bot_native_worker must be an absolute executable path")
		}
	default:
		return errors.New("bot_runtime must be kiss_proxy or native_lua")
	}
	if c.BotCompanionKeyImport {
		if c.BotCompanionListen == "" {
			return errors.New("bot_companion_key_import requires bot_companion_listen")
		}
		host, _, err := net.SplitHostPort(c.BotCompanionListen)
		if err != nil {
			return fmt.Errorf("bot_companion_listen: %w", err)
		}
		if host != "127.0.0.1" && host != "::1" && !strings.EqualFold(host, "localhost") {
			return errors.New("bot_companion_key_import requires a loopback-only bot_companion_listen")
		}
	}
	if c.PHYProfile != nil {
		if err := radio.PHYProfile(*c.PHYProfile).Validate(); err != nil {
			return fmt.Errorf("phy_profile: %w", err)
		}
	}
	if c.Radio.FreqHz < 150000000 || c.Radio.FreqHz > 960000000 ||
		c.Radio.BwHz == 0 || c.Radio.SF < 5 || c.Radio.SF > 12 ||
		c.Radio.CR < 5 || c.Radio.CR > 8 || c.TxPower > 22 {
		return errors.New("invalid shared PHY frequency/bandwidth/SF/CR/transmit power")
	}
	if !radio.SupportedBandwidthHz(c.Radio.BwHz) {
		return errors.New("unsupported LoRa bandwidth")
	}
	capacity := c.RadioCapacity()
	if c.RadioClientCapacity != nil {
		if capacity < 1 || capacity > 8 {
			return errors.New("radio_client_capacity must be 1..8")
		}
	}
	if c.PHYAuthority == "modem" {
		if capacity > 4 {
			return errors.New("phy_authority=modem supports at most four external KISS connections")
		}
	}
	sources := c.RadioSourceCount()
	if c.RadioSession == "auto" || c.RadioSession == "required" {
		sources = c.sessionPhysicalSources()
	}
	if sources > capacity {
		return fmt.Errorf("%d host radio sources exceed radio_client_capacity %d", sources, capacity)
	}
	if c.roleEnabled("bot") && c.BotRuntime != "native_lua" && (c.BotMaxClients < 1 || c.BotMaxClients > capacity-sources) {
		return fmt.Errorf("bot_max_clients must be 1..%d (%d host radio sources share %d modem connections with bot clients)", capacity-sources, sources, capacity)
	}
	if c.CompanionRetention != "native_queue" && c.CompanionRetention != "durable_replay" {
		return errors.New("companion_retention must be native_queue or durable_replay")
	}
	if c.BotCompanionRetention != "native_queue" && c.BotCompanionRetention != "durable_replay" {
		return errors.New("bot_companion_retention must be native_queue or durable_replay")
	}
	for role, name := range map[string]string{
		"repeater": c.RepeaterName, "room": c.RoomName, "companion": c.CompanionName,
		"bot_companion": c.BotCompanionName,
	} {
		if role != "bot_companion" && !c.roleEnabled(role) || role == "bot_companion" && c.BotCompanionListen == "" {
			continue
		}
		if len(name) == 0 || len(name) > 31 || !utf8.ValidString(name) || strings.ContainsRune(name, 0) {
			return fmt.Errorf("%s name must contain 1..31 valid UTF-8 bytes without NUL", role)
		}
	}
	for profile, overrides := range map[policy.Profile]policy.Overrides{
		policy.Repeater: c.RepeaterPolicy, policy.Room: c.RoomPolicy, policy.Companion: c.CompanionPolicy,
	} {
		if !c.roleEnabled(string(profile)) {
			continue
		}
		if _, err := policy.ApplyOverrides(profile, policy.Defaults(profile), c.withLegacyAdvert(overrides)); err != nil {
			return fmt.Errorf("%s policy: %w", profile, err)
		}
	}
	if c.BotCompanionListen != "" {
		if _, err := policy.ApplyOverrides(policy.Companion, policy.Defaults(policy.Companion), c.BotCompanionPolicy); err != nil {
			return fmt.Errorf("bot_companion policy: %w", err)
		}
	}
	if c.roleEnabled("observer") && (c.MQTT.URL == "" || c.MQTT.TopicPrefix == "") {
		return errors.New("MQTT URL and topic prefix are required")
	}
	return nil
}

// FollowsModemPHY reports whether host links adopt the mast's effective PHY.
// Modem authority follows unless phy_tracking is explicitly fixed; host
// authority always applies and then verifies its configured profile.
func (c Config) FollowsModemPHY() bool {
	return c.PHYAuthority == "modem" && c.PHYTracking != "fixed"
}

func (c Config) radioPHYTracking() radio.PHYTracking {
	if c.FollowsModemPHY() {
		return radio.PHYFollow
	}
	return radio.PHYFixed
}

// RadioCapacity is the configured external KISS connection budget.
func (c Config) RadioCapacity() int {
	if c.RadioClientCapacity != nil {
		return *c.RadioClientCapacity
	}
	if c.PHYAuthority == "modem" {
		return 4
	}
	return 8
}

// RadioSourceCount includes the persistent observer/controller connection.
func (c Config) RadioSourceCount() int {
	sources := 1
	for _, role := range []string{"repeater", "room", "companion"} {
		if c.roleEnabled(role) {
			sources++
		}
	}
	if c.BotCompanionListen != "" {
		sources++
	}
	if c.roleEnabled("bot") && c.BotRuntime == "native_lua" {
		sources++
	}
	return sources
}

// sessionPhysicalSources reserves a direct socket for each source beyond the
// negotiated virtual ports; bot proxy clients keep their own direct sockets.
func (c Config) sessionPhysicalSources() int {
	return c.sessionPhysicalSourcesFor(4)
}

func (c Config) sessionPhysicalSourcesFor(ports int) int {
	return 1 + max(0, c.RadioSourceCount()-ports)
}

// sessionMinPorts accepts fewer than four logical ports only when the
// remaining roles and reserved bot clients can use direct TCP sockets.
func (c Config) sessionMinPorts() int {
	botClients := 0
	if c.roleEnabled("bot") && c.BotRuntime != "native_lua" {
		botClients = c.BotMaxClients
	}
	return min(4, max(min(2, c.RadioSourceCount()), c.RadioSourceCount()+botClients+1-c.RadioCapacity()))
}

func (c Config) reservedClients(multiplexed bool) int {
	sources := c.RadioSourceCount()
	if multiplexed {
		sources = c.sessionPhysicalSources()
	}
	if c.roleEnabled("bot") && c.BotRuntime != "native_lua" {
		sources += c.BotMaxClients
	}
	return sources
}

func (c Config) sessionReservedClientsFor(ports int) int {
	sources := c.sessionPhysicalSourcesFor(ports)
	if c.roleEnabled("bot") && c.BotRuntime != "native_lua" {
		sources += c.BotMaxClients
	}
	return sources
}

func (c Config) roleEnabled(role string) bool {
	switch role {
	case "repeater", "room", "companion", "observer", "bot":
	default:
		return false
	}
	if c.EnabledRoles == nil {
		return true
	}
	for _, enabled := range c.EnabledRoles {
		if enabled == role {
			return true
		}
	}
	return false
}

// RoleEnabled reports whether a configurable host role is selected.
func (c Config) RoleEnabled(role string) bool {
	return c.roleEnabled(role)
}

func (c Config) withLegacyAdvert(overrides policy.Overrides) policy.Overrides {
	if overrides.FloodAdvertSeconds == nil {
		overrides.FloodAdvertSeconds = c.AdvertIntervalSecs
	}
	return overrides
}

func envSecret(name string) (string, error) {
	if name == "" {
		return "", nil
	}
	value, ok := os.LookupEnv(name)
	if !ok || value == "" {
		return "", fmt.Errorf("required environment variable %s is empty or unset", name)
	}
	return value, nil
}

func (c Config) roomAccessPassword() (string, error) {
	if !c.roleEnabled("room") {
		return "", nil
	}
	if c.RoomPublic {
		if c.RoomPassword != "" || c.RoomPasswordEnv != "" {
			return "", errors.New("room_public=true requires empty room_password and room_password_env")
		}
		return "", nil
	}
	password := c.RoomPassword
	if c.RoomPasswordEnv != "" {
		var err error
		password, err = envSecret(c.RoomPasswordEnv)
		if err != nil {
			return "", fmt.Errorf("room access: %w", err)
		}
	}
	if password == "" {
		return "", errors.New("room access requires room_password_env or room_password; set room_public=true only for deliberate public access")
	}
	return password, nil
}
