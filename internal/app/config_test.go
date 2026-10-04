package app

import (
	"os"
	"path/filepath"
	"testing"

	"meshcore.local/meshcore/internal/policy"
)

func TestRadioAddressDefaultsToMDNSAndPreservesExplicitAddress(t *testing.T) {
	for _, tc := range []struct {
		name, input, want string
	}{
		{"default", `{}`, "meshcore-radio.local:8001"},
		{"explicit", `{"radio_address":"192.0.2.42:8001"}`, "192.0.2.42:8001"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "config.json")
			if err := os.WriteFile(path, []byte(tc.input), 0600); err != nil {
				t.Fatal(err)
			}
			cfg, err := LoadConfig(path)
			if err != nil {
				t.Fatal(err)
			}
			if cfg.RadioAddress != tc.want {
				t.Fatalf("radio address = %q, want %q", cfg.RadioAddress, tc.want)
			}
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if string(data) != tc.input {
				t.Fatal("loading configuration rewrote its contents")
			}
		})
	}
}

func TestExampleUsesNegotiatedSessionWithoutChangingLegacyDefaults(t *testing.T) {
	cfg, err := LoadConfig("../../meshcore-host.json.example")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.RadioSession != "auto" || DefaultConfig().RadioSession != "per_role" {
		t.Fatalf("example should negotiate MKISS while existing default configurations retain per-role links: %s", cfg.RadioSession)
	}
	if cfg.reservedClients(true) != 3 || cfg.reservedClients(false) != 6 {
		t.Fatalf("example omitted physical role or bot client reservations: aggregate=%d direct=%d",
			cfg.reservedClients(true), cfg.reservedClients(false))
	}
}

func TestGlobalPHYProfilePreservesNativeFactorAndNaming(t *testing.T) {
	for _, tc := range []struct {
		input string
		want  float64
	}{
		{`{"phy_profile":{"airtime_factor":99,"cad_enabled":true,"interference_threshold":8}}`, 99},
		{`{"phy_profile":{"airtime_factor":0,"cad_enabled":true,"interference_threshold":8}}`, 0},
	} {
		path := filepath.Join(t.TempDir(), "config.json")
		if err := os.WriteFile(path, []byte(tc.input), 0600); err != nil {
			t.Fatal(err)
		}
		cfg, err := LoadConfig(path)
		if err != nil {
			t.Fatal(err)
		}
		if cfg.PHYProfile == nil || cfg.PHYProfile.AirtimeFactor != tc.want ||
			!cfg.PHYProfile.CADEnabled || cfg.PHYProfile.InterferenceThreshold != 8 {
			t.Fatal("global profile lost explicit factor or carrier configuration")
		}
	}
	defaults := DefaultConfig()
	if defaults.PHYProfile == nil || defaults.PHYProfile.AirtimeFactor != 1 ||
		!defaults.PHYProfile.CADEnabled || defaults.PHYProfile.InterferenceThreshold != 0 {
		t.Fatal("default shared PHY must enable CAD without changing airtime or interference policy")
	}
	if defaults.CompanionKeyExport || defaults.CompanionFactoryReset || defaults.RoleKeyImport {
		t.Fatal("private-key export, factory reset or identity import enabled by default")
	}
	for _, tc := range []struct {
		input string
		cad   bool
	}{
		{`{}`, true},
		{`{"phy_profile":{"airtime_factor":1,"cad_enabled":false}}`, false},
		{`{"phy_profile":null}`, false},
	} {
		path := filepath.Join(t.TempDir(), "config.json")
		if err := os.WriteFile(path, []byte(tc.input), 0600); err != nil {
			t.Fatal(err)
		}
		cfg, err := LoadConfig(path)
		if err != nil {
			t.Fatal(err)
		}
		if got := cfg.PHYProfile != nil && cfg.PHYProfile.CADEnabled; got != tc.cad {
			t.Fatalf("CAD default or saved override changed for %s", tc.input)
		}
	}
}

func TestHostAcceptsMainlineMastNarrowBandwidthReadback(t *testing.T) {
	cfg := DefaultConfig()
	for _, bw := range []uint32{7810, 10420, 15630, 20830} {
		cfg.Radio.BwHz = bw
		if err := cfg.Validate(); err != nil {
			t.Fatalf("native mast bandwidth %d Hz rejected: %v", bw, err)
		}
	}
}

func TestLoadConfigRejectsInvalidInputRatherThanUsingRadioDefaults(t *testing.T) {
	for _, input := range []string{
		`null`, `[]`, `""`, ``, `{} {}`,
		`{"radio_addres":"wrong-key:8001"}`,
		`{"radio":{"FreqHz":0}}`,
		`{"radio":{"BwHz":12345}}`,
		`{"bot_max_clients":5}`,
		`{"repeater_policy":{"path_hash_mode":3}}`,
		`{"room_policy":{"rxdelay":-1}}`,
		`{"companion_policy":{"local_advert_seconds":30601}}`,
		`{"advert_interval_seconds":918001}`,
		`{"companion_retention":"lossless"}`,
		`{"phy_profile":{"airtime_factor":-1}}`,
		`{"phy_profile":{"airtime_factor":1e100}}`,
		`{"enabled_roles":["repeater","repeater"]}`,
		`{"enabled_roles":["base"]}`,
		`{"enabled_roles":["bot_companion"]}`,
		`{"enabled_roles":null}`,
		`{"room_name":"not\u0000safe"}`,
		`{"companion_name":"not\u0000safe"}`,
		`{"bot_companion_listen":"127.0.0.1:5001","bot_companion_name":"not\u0000safe"}`,
		`{"phy_authority":"unknown"}`,
		`{"phy_authority":null}`,
		`{"radio_session":null}`,
		`{"radio_session":"single"}`,
		`{"phy_authority":"modem"}`,
		`{"phy_authority":"modem","radio_client_capacity":5}`,
		`{"phy_authority":"modem","radio_client_capacity":4}`,
		`{"phy_authority":"modem","radio_client_capacity":4,"require_parity":false}`,
		`{"phy_tracking":"follow"}`,
		`{"phy_tracking":"fixed","require_parity":false,"phy_authority":"modem","radio_client_capacity":4}`,
		`{"phy_authority":"modem","radio_client_capacity":4,"phy_tracking":"pinned"}`,
		`{"radio_client_capacity":0}`,
		`{"radio_client_capacity":9}`,
	} {
		t.Run(input, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "config.json")
			if err := os.WriteFile(path, []byte(input), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadConfig(path); err == nil {
				t.Fatalf("invalid configuration %q accepted", input)
			}
		})
	}
}

func TestHostRejectsInvalidRuntimeAdvertNamesBeforeRadioStartup(t *testing.T) {
	for _, role := range []string{"repeater", "room", "companion", "bot_companion"} {
		cfg := DefaultConfig()
		if role == "bot_companion" {
			cfg.BotCompanionListen = "127.0.0.1:5001"
		}
		invalid := string([]byte{0xff})
		switch role {
		case "repeater":
			cfg.RepeaterName = invalid
		case "room":
			cfg.RoomName = invalid
		case "companion":
			cfg.CompanionName = invalid
		case "bot_companion":
			cfg.BotCompanionName = invalid
		}
		if err := cfg.Validate(); err == nil {
			t.Fatalf("%s accepted an invalid UTF-8 advert name", role)
		}
	}
}

func TestModemAuthorityEnforcesFourExternalConnections(t *testing.T) {
	cfg := DefaultConfig()
	if cfg.PHYAuthority != "host" || cfg.RadioClientCapacity != nil ||
		cfg.RadioCapacity() != 8 || cfg.RadioSourceCount() != 4 {
		t.Fatalf("omitted authority changed the eight-client host default: %+v", cfg)
	}

	capacity := 4
	cfg.PHYAuthority = "modem"
	cfg.RadioClientCapacity = &capacity
	cfg.EnabledRoles = []string{"repeater", "room"}
	if cfg.RadioSourceCount() != 3 {
		t.Fatalf("controller was not reserved: %d", cfg.RadioSourceCount())
	}
	if err := cfg.Validate(); err != nil {
		t.Fatalf("three source connections should fit: %v", err)
	}
	cfg.RadioClientCapacity = nil
	if err := cfg.Validate(); err != nil || cfg.RadioCapacity() != 4 {
		t.Fatalf("query-only modem authority did not reserve a four-slot maximum: %v", err)
	}
	cfg.RadioClientCapacity = &capacity
	cfg.EnabledRoles = []string{"repeater", "room", "bot"}
	cfg.BotMaxClients = 1
	if err := cfg.Validate(); err != nil {
		t.Fatalf("three sources plus one KISS client should fit: %v", err)
	}
	cfg.BotMaxClients = 2
	if err := cfg.Validate(); err == nil {
		t.Fatal("fifth external KISS connection was accepted")
	}
	cfg.BotMaxClients = 1
	cfg.BotCompanionListen = "127.0.0.1:5001"
	if err := cfg.Validate(); err == nil {
		t.Fatal("bot companion's radio source did not count against four")
	}
	cfg.EnabledRoles = []string{"repeater", "bot"}
	if err := cfg.Validate(); err != nil {
		t.Fatalf("controller, repeater, bot companion and one KISS client should fit: %v", err)
	}
	cfg.EnabledRoles = []string{"repeater", "room", "companion"}
	if err := cfg.Validate(); err == nil {
		t.Fatal("five persistent host sources were accepted without bot KISS")
	}
	cfg.BotCompanionListen = ""
	if err := cfg.Validate(); err != nil {
		t.Fatalf("four persistent host sources should fit: %v", err)
	}
	capacity = 3
	if err := cfg.Validate(); err == nil {
		t.Fatal("explicit three-client budget was ignored")
	}
}

func TestSessionModeReservesDirectBotAndOverflowSockets(t *testing.T) {
	cfg := DefaultConfig()
	if cfg.RadioSession != "per_role" {
		t.Fatal("default changed existing per-role connections")
	}
	cfg.RadioSession = "required"
	cfg.BotCompanionListen = "127.0.0.1:5001"
	if cfg.RadioSourceCount() != 5 || cfg.sessionPhysicalSources() != 2 || cfg.reservedClients(true) != 4 {
		t.Fatalf("multiplexed budget omitted bot companion or direct KISS clients: %d %d",
			cfg.sessionPhysicalSources(), cfg.reservedClients(true))
	}
	ports := cfg.sessionRolePorts(4)
	if len(ports) != 3 || ports["repeater"] != 1 || ports["room"] != 2 || ports["companion"] != 3 || ports["bot_companion"] != 0 {
		t.Fatalf("overflow role was omitted or stole a subport: %v", ports)
	}
	cfg.EnabledRoles = []string{"room", "bot"}
	ports = cfg.sessionRolePorts(4)
	if len(ports) != 2 || ports["room"] != 1 || ports["bot_companion"] != 2 {
		t.Fatalf("disabled roles reserved subports: %v", ports)
	}
	ports = cfg.sessionRolePorts(2)
	if len(ports) != 1 || ports["room"] != 1 {
		t.Fatalf("smaller negotiated session used a nonexistent port: %v", ports)
	}
	cfg.EnabledRoles = nil
	capacity := 4
	cfg.RadioClientCapacity = &capacity
	if err := cfg.Validate(); err != nil {
		t.Fatalf("four physical sockets rejected: %v", err)
	}
	cfg.RadioSession = "per_role"
	if err := cfg.Validate(); err == nil {
		t.Fatal("per-role plan silently omitted sources")
	}
	cfg.RadioSession = "auto"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("auto incorrectly rejected negotiable session: %v", err)
	}
	cfg.BotMaxClients = 3
	if err := cfg.Validate(); err == nil {
		t.Fatal("third direct KISS client exceeded negotiated socket capacity")
	}
	cfg.BotMaxClients = 2
	cfg.RequireParity = false
	if err := cfg.Validate(); err == nil {
		t.Fatal("non-queued session mode was accepted")
	}
	cfg.RequireParity = true
	for _, invalid := range []string{"single", "off", "Per_Role"} {
		cfg.RadioSession = RadioSessionMode(invalid)
		if err := cfg.Validate(); err == nil {
			t.Fatalf("unknown radio_session %q accepted", invalid)
		}
	}
}

func TestEnabledRolesPreserveDefaultsAndSelectOnlyRequestedSources(t *testing.T) {
	for _, tc := range []struct {
		name, input string
		want        []string
	}{
		{"omitted", `{}`, []string{"repeater", "room", "companion", "observer", "bot"}},
		{"repeater-companion", `{"enabled_roles":["repeater","companion"]}`, []string{"repeater", "companion"}},
		{"repeater-room", `{"enabled_roles":["repeater","room"]}`, []string{"repeater", "room"}},
		{"repeater-observer", `{"enabled_roles":["repeater","observer"]}`, []string{"repeater", "observer"}},
		{"controller-only", `{"enabled_roles":[]}`, nil},
		{"bot-companion-only", `{"enabled_roles":[],"bot_companion_listen":"127.0.0.1:5001"}`, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "config.json")
			if err := os.WriteFile(path, []byte(tc.input), 0600); err != nil {
				t.Fatal(err)
			}
			cfg, err := LoadConfig(path)
			if err != nil {
				t.Fatal(err)
			}
			for _, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
				want := false
				for _, selected := range tc.want {
					want = want || role == selected
				}
				if cfg.RoleEnabled(role) != want {
					t.Fatalf("%s enabled=%v, want %v", role, cfg.RoleEnabled(role), want)
				}
			}
			if tc.name == "bot-companion-only" && cfg.BotCompanionListen == "" {
				t.Fatal("separate bot companion was disabled with the base")
			}
			if cfg.RoleEnabled("bot_companion") || cfg.RoleEnabled("unknown") {
				t.Fatal("non-selectable roles were implicitly enabled")
			}
		})
	}
}

func TestSelectiveListenerValidationAndPhysicalCapacity(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"repeater", "room"}
	cfg.CompanionListen = "invalid"
	cfg.BotListen = "invalid"
	cfg.MQTT.BrokerListen = "invalid"
	cfg.MQTT.URL = ""
	cfg.MQTT.TopicPrefix = ""
	cfg.BotMaxClients = 0
	if err := cfg.Validate(); err != nil {
		t.Fatalf("disabled endpoints and broker were required: %v", err)
	}

	cfg.BotCompanionListen = "127.0.0.1:5001"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("bot companion unexpectedly depended on base: %v", err)
	}
	cfg.EnabledRoles = []string{"repeater", "room", "bot"}
	cfg.BotListen = "127.0.0.1:8105"
	cfg.BotMaxClients = 4 // owner + room + repeater + bot companion + four clients
	if err := cfg.Validate(); err != nil {
		t.Fatalf("eight modem connections rejected: %v", err)
	}
	cfg.BotMaxClients = 5
	if err := cfg.Validate(); err == nil {
		t.Fatal("nine modem connections accepted")
	}
	cfg.BotMaxClients = 4
	cfg.EnabledRoles = []string{"repeater", "room", "companion", "bot"}
	cfg.CompanionListen = "127.0.0.1:5000"
	if err := cfg.Validate(); err == nil {
		t.Fatal("enabling another source did not reduce bot capacity")
	}
	cfg.BotMaxClients = 3
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	cfg.EnabledRoles = []string{"repeater", "room", "companion", "observer", "bot"}
	cfg.MQTT.BrokerListen = "127.0.0.1:5000"
	cfg.MQTT.URL = "tcp://127.0.0.1:1883"
	cfg.MQTT.TopicPrefix = "meshcore"
	if err := cfg.Validate(); err == nil {
		t.Fatal("enabled broker listener collided with companion")
	}
	cfg.EnabledRoles = []string{"repeater", "room", "companion", "bot"}
	if err := cfg.Validate(); err != nil {
		t.Fatalf("disabled broker reserved a port: %v", err)
	}
}

func TestPolicyOverridePrecedencePreservesOmittedAndExplicitZero(t *testing.T) {
	for _, tc := range []struct {
		name, input string
		wantFlood   uint32
	}{
		{"omitted", `{}`, 1234},
		{"legacy", `{"advert_interval_seconds":169200}`, 169200},
		{"legacy-disabled", `{"advert_interval_seconds":0}`, 0},
		{"role-wins", `{"advert_interval_seconds":3600,"repeater_policy":{"flood_advert_seconds":169200}}`, 169200},
		{"role-disabled", `{"advert_interval_seconds":3600,"repeater_policy":{"flood_advert_seconds":0}}`, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "config.json")
			if err := os.WriteFile(path, []byte(tc.input), 0600); err != nil {
				t.Fatal(err)
			}
			cfg, err := LoadConfig(path)
			if err != nil {
				t.Fatal(err)
			}
			saved := policy.Defaults(policy.Repeater)
			saved.PathHashMode = 2
			saved.FloodAdvertSeconds = 1234
			saved.LocalAdvertSeconds = 0
			got, err := policy.ApplyOverrides(policy.Repeater, saved, cfg.withLegacyAdvert(cfg.RepeaterPolicy))
			if err != nil {
				t.Fatal(err)
			}
			if got.FloodAdvertSeconds != tc.wantFlood || got.PathHashMode != 2 || got.LocalAdvertSeconds != 0 {
				t.Fatalf("startup changed omitted preferences or lost explicit override: %+v", got)
			}
			if !cfg.RequireParity {
				t.Fatal("default silently permits an incapable modem")
			}
		})
	}
}

func TestPolicyOverridesAreRoleSpecificAndLegacyTransportIsExplicit(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(path, []byte(`{
		"require_parity":false,
		"repeater_policy":{"path_hash_mode":2,"airtime_factor":99},
		"room_policy":{"path_hash_mode":1}
	}`), 0600); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.RequireParity || cfg.RepeaterPolicy.PathHashMode == nil || *cfg.RepeaterPolicy.PathHashMode != 2 ||
		cfg.RoomPolicy.PathHashMode == nil || *cfg.RoomPolicy.PathHashMode != 1 ||
		cfg.CompanionPolicy.PathHashMode != nil || cfg.AdvertIntervalSecs != nil ||
		cfg.RepeaterPolicy.AirtimeFactor == nil || *cfg.RepeaterPolicy.AirtimeFactor != 99 {
		t.Fatal("configuration merged independent roles, lost AF99, or fabricated an override")
	}
}

func TestLoadConfigResolvesStateRelativeToConfigAndKeepsExplicitTuning(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.json")
	if err := os.WriteFile(path, []byte(`{
		"state_dir":"data",
		"companion_factory_reset":true,
		"radio":{"FreqHz":912525000,"BwHz":250000,"SF":7,"CR":5},
		"tx_power":2
	}`), 0600); err != nil {
		t.Fatal(err)
	}

	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.StateDir != filepath.Join(dir, "data") ||
		!cfg.CompanionFactoryReset ||
		cfg.Radio.FreqHz != 912525000 || cfg.Radio.BwHz != 250000 || cfg.TxPower != 2 {
		t.Fatalf("configuration changed intended state/tuning: %+v", cfg)
	}
}

func TestOptionalBotCompanionConfigAndListenerCapacity(t *testing.T) {
	cfg := DefaultConfig()
	if cfg.BotCompanionListen != "" || cfg.BotCompanionKeyImport || cfg.BotMaxClients != 2 {
		t.Fatal("optional companion listener changed the default deployment")
	}
	disabled := cfg
	disabled.BotCompanionKeyImport = true
	if err := disabled.Validate(); err == nil {
		t.Fatal("key import enabled without a dedicated bot companion listener")
	}
	path := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(path, []byte(`{
		"bot_companion_listen":"127.0.0.1:5001",
		"bot_companion_name":"Independent Bot",
		"bot_companion_policy":{"airtime_factor":4},
		"bot_companion_retention":"native_queue",
		"bot_companion_key_import":true
	}`), 0600); err != nil {
		t.Fatal(err)
	}
	loaded, err := LoadConfig(path)
	if err != nil || loaded.BotCompanionListen != "127.0.0.1:5001" ||
		loaded.BotCompanionName != "Independent Bot" ||
		loaded.BotCompanionPolicy.AirtimeFactor == nil ||
		*loaded.BotCompanionPolicy.AirtimeFactor != 4 ||
		!loaded.BotCompanionKeyImport ||
		loaded.BotCompanionRetention != "native_queue" ||
		loaded.CompanionListen != cfg.CompanionListen ||
		loaded.CompanionPolicy.AirtimeFactor != nil {
		t.Fatalf("JSON configuration did not keep companion roles independent: %+v, %v", loaded, err)
	}
	cfg.BotCompanionListen = "127.0.0.1:5001"
	cfg.BotCompanionKeyImport = true
	if err := cfg.Validate(); err != nil {
		t.Fatalf("five sources and two KISS clients should fit: %v", err)
	}
	for _, address := range []string{"localhost:5001", "LOCALHOST:5001", "[::1]:5001", "127.0.0.1:5001"} {
		cfg.BotCompanionListen = address
		if err := cfg.Validate(); err != nil {
			t.Fatalf("explicit loopback listener %q rejected: %v", address, err)
		}
	}
	for _, address := range []string{":5001", "0.0.0.0:5001", "[::]:5001", "192.0.2.1:5001",
		"bot.local:5001", "[::ffff:127.0.0.1]:5001"} {
		cfg.BotCompanionListen = address
		if err := cfg.Validate(); err == nil {
			t.Fatalf("key import permitted on non-loopback listener %q", address)
		}
	}
	cfg.BotCompanionKeyImport = false
	cfg.BotCompanionListen = "127.0.0.1:5001"
	cfg.BotMaxClients = 3
	if err := cfg.Validate(); err != nil {
		t.Fatalf("eight reserved physical connections should fit: %v", err)
	}
	cfg.BotMaxClients = 4
	if err := cfg.Validate(); err == nil {
		t.Fatal("nine reserved physical connections accepted")
	}
	cfg.BotMaxClients = 2
	for _, tc := range []struct {
		name string
		edit func(*Config)
	}{
		{"base", func(c *Config) { c.BotCompanionListen = "127.0.0.1:5000" }},
		{"KISS", func(c *Config) { c.BotCompanionListen = "127.0.0.1:8105" }},
		{"health", func(c *Config) { c.BotCompanionListen = "0.0.0.0:9080" }},
		{"MQTT", func(c *Config) { c.BotCompanionListen = "127.0.0.1:1883" }},
		{"invalid-bind", func(c *Config) { c.BotCompanionListen = "not-an-address" }},
		{"empty-port", func(c *Config) { c.BotCompanionListen = "127.0.0.1:" }},
		{"base-and-MQTT", func(c *Config) { c.MQTT.BrokerListen = "0.0.0.0:5000" }},
		{"bad-policy", func(c *Config) {
			mode := policy.PathHashMode(3)
			c.BotCompanionPolicy.PathHashMode = &mode
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			bad := cfg
			tc.edit(&bad)
			if err := bad.Validate(); err == nil {
				t.Fatal("conflicting or invalid optional service configuration accepted")
			}
		})
	}
	cfg.BotCompanionListen = "127.0.0.1:0"
	cfg.CompanionListen = "127.0.0.1:0"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("independent ephemeral test listeners were rejected: %v", err)
	}
}

func TestNativeBotSelectsOneSourceInsteadOfKISSProxyClients(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"bot"}
	cfg.BotRuntime = "native_lua"
	cfg.BotNativeWorker = "/opt/meshcore/bin/bot-native-worker"
	cfg.BotListen = "not-a-listener"
	cfg.BotMaxClients = 99
	if cfg.RadioSourceCount() != 2 || cfg.reservedClients(false) != 2 {
		t.Fatalf("native bot did not reserve exactly one distinct source: sources=%d reserved=%d",
			cfg.RadioSourceCount(), cfg.reservedClients(false))
	}

	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	cfg.BotNativeWorker = "relative/worker"
	if err := cfg.Validate(); err == nil {
		t.Fatal("relative worker executable accepted")
	}
	cfg.BotNativeWorker = "/opt/meshcore/bin/bot-native-worker"
	cfg.RequireParity = false
	if err := cfg.Validate(); err == nil {
		t.Fatal("native bot silently fell back to non-queued RF")
	}
	cfg.RequireParity = true
	cfg.EnabledRoles = []string{}
	if err := cfg.Validate(); err == nil {
		t.Fatal("native runtime enabled without a bot role")
	}
}

func TestNativeBotAndOptionalCompanionOverflowFourLogicalPortsExplicitly(t *testing.T) {
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	cfg.RadioSession = "required"
	cfg.BotRuntime = "native_lua"
	cfg.BotNativeWorker = "/opt/meshcore/bin/bot-native-worker"
	cfg.BotCompanionListen = "127.0.0.1:5001"
	if cfg.RadioSourceCount() != 6 || cfg.sessionPhysicalSourcesFor(4) != 3 ||
		cfg.sessionReservedClientsFor(4) != 3 {
		t.Fatalf("native bot or optional companion did not reserve explicit direct sockets: %d sources, %d physical",
			cfg.RadioSourceCount(), cfg.sessionPhysicalSourcesFor(4))
	}
	ports := cfg.sessionRolePorts(4)
	if len(ports) != 3 || ports["repeater"] != 1 || ports["room"] != 2 ||
		ports["companion"] != 3 || ports["bot"] != 0 || ports["bot_companion"] != 0 {
		t.Fatalf("four logical slots including controller cannot hold five roles: %v", ports)
	}
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
}
