// meshcore-check exercises the deployed host using normal companion, KISS and MQTT clients.
package main

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
	ctransport "github.com/meshcore-go/meshcore-go/companion/transport"
	"github.com/meshcore-go/meshcore-go/hardware"
	htransport "github.com/meshcore-go/meshcore-go/hardware/transport"
	"meshcore.local/meshcore/internal/app"
	"meshcore.local/meshcore/internal/radio"
)

func selfInfoMatches(info companion.SelfInfoResponse, reference hardware.RadioConfig) bool {
	return info.RadioFrequency == reference.FreqHz/1000 && info.RadioBandwidth == reference.BwHz &&
		info.RadioSpreadFactor == reference.SF && info.RadioCodingRate == reference.CR
}

func localAddress(address string) string {
	host, port, err := net.SplitHostPort(address)
	if err == nil && (host == "" || host == "0.0.0.0" || host == "::") {
		return net.JoinHostPort("127.0.0.1", port)
	}
	return address
}

type roleStatus struct {
	PublicKey            string           `json:"public_key"`
	Connected            *bool            `json:"radio_connected"`
	Endpoint             string           `json:"endpoint"`
	ListenerActive       *bool            `json:"listener_active"`
	AirtimeMS            []uint32         `json:"airtime_ms"`
	ApplicationStartedAt time.Time        `json:"application_started_at"`
	ApplicationError     string           `json:"application_error"`
	State                string           `json:"state"`
	SourceGeneration     uint32           `json:"source_generation"`
	DeviceUptime         uint64           `json:"device_uptime"`
	MQTTConnected        *bool            `json:"mqtt_connected"`
	SharedPHY            *sharedPHYStatus `json:"shared_phy"`
}

type sharedPHYStatus struct {
	ConfigurationOwner     string        `json:"configuration_owner"`
	PHYAuthority           string        `json:"phy_authority"`
	PHYTracking            string        `json:"phy_tracking"`
	ExternalClientCapacity int           `json:"external_client_capacity"`
	CapacitySource         string        `json:"capacity_source"`
	OwnerConnected         bool          `json:"owner_connected"`
	Valid                  bool          `json:"valid"`
	Effective              *effectivePHY `json:"effective"`
}

type effectivePHY struct {
	FrequencyHz           uint32  `json:"frequency_hz"`
	BandwidthHz           uint32  `json:"bandwidth_hz"`
	SpreadingFactor       uint8   `json:"spreading_factor"`
	CodingRate            uint8   `json:"coding_rate"`
	TxPowerDBm            uint8   `json:"tx_power_dbm"`
	AirtimeFactor         float64 `json:"airtime_factor"`
	CADEnabled            bool    `json:"cad_enabled"`
	InterferenceThreshold int16   `json:"interference_threshold"`
}

// sharedPHYReference returns the radio tuning every host surface must match.
// With a fixed profile (host authority or phy_tracking fixed) that is the
// configured tuning; when following the mast it is the host's verified live
// readback, which must still be a valid profile.
func sharedPHYReference(cfg app.Config, phy *sharedPHYStatus) (hardware.RadioConfig, error) {
	if phy == nil || !phy.OwnerConnected || !phy.Valid || phy.Effective == nil {
		return hardware.RadioConfig{}, errors.New("effective PHY is unverified")
	}
	tracking := "fixed"
	if cfg.FollowsModemPHY() {
		tracking = "follow"
	}
	if phy.PHYTracking != tracking && (phy.PHYTracking != "" || tracking == "follow") {
		return hardware.RadioConfig{}, fmt.Errorf("host reports phy_tracking %q, expected %q", phy.PHYTracking, tracking)
	}
	effective := radio.PHYSettings{
		Radio: hardware.RadioConfig{
			FreqHz: phy.Effective.FrequencyHz, BwHz: phy.Effective.BandwidthHz,
			SF: phy.Effective.SpreadingFactor, CR: phy.Effective.CodingRate,
		},
		TxPower: phy.Effective.TxPowerDBm,
		Profile: radio.PHYProfile{
			AirtimeFactor: phy.Effective.AirtimeFactor, CADEnabled: phy.Effective.CADEnabled,
			InterferenceThreshold: phy.Effective.InterferenceThreshold,
		},
	}
	if err := effective.Validate(); err != nil {
		return hardware.RadioConfig{}, err
	}
	if tracking == "fixed" && (effective.Radio != cfg.Radio || effective.TxPower != cfg.TxPower) {
		return hardware.RadioConfig{}, fmt.Errorf("effective PHY %s differs from the fixed configured profile", effective)
	}
	return effective.Radio, nil
}

func readStatus(ctx context.Context, cfg app.Config, onchip bool) (map[string]roleStatus, error) {
	path := "/status"
	if onchip {
		path = "/api/status"
	}
	request, err := http.NewRequestWithContext(ctx, "GET", "http://"+localAddress(cfg.StatusListen)+path, nil)
	if err != nil {
		return nil, err
	}
	client := &http.Client{Timeout: 5 * time.Second}
	var response *http.Response
	for attempt := 0; ; attempt++ {
		response, err = client.Do(request)
		if err != nil {
			return nil, err
		}
		if response.StatusCode != http.StatusTooManyRequests || attempt == 2 {
			break
		}
		response.Body.Close()
		select {
		case <-time.After(time.Second):
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("host status: %s", response.Status)
	}
	var status map[string]roleStatus
	if onchip {
		var snapshot struct {
			Uptime uint64 `json:"uptime_ms"`
			Roles  []struct {
				Role       string `json:"role"`
				Name       string `json:"name"`
				PublicKey  string `json:"public_key"`
				State      string `json:"state"`
				Ready      bool   `json:"ready"`
				Fault      string `json:"fault"`
				Generation uint32 `json:"source_generation"`
			} `json:"roles"`
		}
		if err := json.NewDecoder(response.Body).Decode(&snapshot); err != nil {
			return nil, err
		}
		status = make(map[string]roleStatus)
		for _, role := range snapshot.Roles {
			if _, exists := status[role.Role]; exists {
				return nil, fmt.Errorf("duplicate on-chip role %q", role.Role)
			}
			status[role.Role] = roleStatus{
				PublicKey: role.PublicKey, Connected: &role.Ready,
				State: role.State, ApplicationError: role.Fault,
				SourceGeneration: role.Generation, DeviceUptime: snapshot.Uptime,
			}
		}
		return status, nil
	}
	if err := json.NewDecoder(response.Body).Decode(&status); err != nil {
		return nil, err
	}
	return status, nil
}

type checks struct {
	transmit, airtime, reboot, factoryReset, rebootRoles, rekeyRoles bool
	onchip                                                           bool
	onchipRoles                                                      map[string]bool
}

func enabled(cfg app.Config, selected checks, role string) bool {
	if selected.onchip {
		active, found := selected.onchipRoles[role]
		return !found || active
	}
	return cfg.RoleEnabled(role)
}

func (selected *checks) setOnchipRoles(status map[string]roleStatus) {
	if !selected.onchip {
		return
	}
	selected.onchipRoles = make(map[string]bool)
	for _, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
		if entry, found := status[role]; found {
			selected.onchipRoles[role] = entry.State != "disabled"
		}
	}
}

func checkSelection(cfg app.Config, selected checks) error {
	if selected.airtime && !enabled(cfg, selected, "bot") &&
		cfg.RadioSourceCount() >= cfg.RadioCapacity() {
		return errors.New("-airtime requires an unreserved physical radio client slot or enabled bot KISS")
	}
	require := func(flagName string, roles ...string) error {
		for _, role := range roles {
			if !enabled(cfg, selected, role) {
				return fmt.Errorf("-%s requires enabled role %s", flagName, role)
			}
		}
		return nil
	}
	if selected.reboot || selected.factoryReset {
		if err := require("reboot/-factory-reset", "companion", "bot"); err != nil {
			return err
		}
	}
	if selected.rebootRoles || selected.rekeyRoles {
		if err := require("reboot-roles/-rekey-roles", "companion", "bot"); err != nil {
			return err
		}
		if !enabled(cfg, selected, "room") && !enabled(cfg, selected, "repeater") {
			return errors.New("-reboot-roles/-rekey-roles requires an enabled room or repeater")
		}
	}
	if selected.transmit {
		if err := require("transmit", "companion", "room", "observer", "bot"); err != nil {
			return err
		}
	}
	return nil
}

func checkRoleStatus(cfg app.Config, selected checks, status map[string]roleStatus) (int, error) {
	seen := map[string]bool{}
	count := 0
	for _, role := range []string{"repeater", "room", "companion", "observer", "bot", "bot_companion"} {
		active := role == "bot_companion" && !selected.onchip && cfg.BotCompanionListen != "" ||
			role != "bot_companion" && enabled(cfg, selected, role)
		entry, exists := status[role]
		if !active {
			if role != "bot_companion" && (!exists || entry.State != "disabled" || entry.PublicKey != "") {
				return 0, fmt.Errorf("disabled role %s is not reported disabled", role)
			}
			continue
		}
		if !exists || len(entry.PublicKey) != 64 || seen[entry.PublicKey] ||
			(role != "bot" && (entry.Connected == nil || !*entry.Connected)) ||
			(!selected.onchip && entry.State != "running") {
			return 0, fmt.Errorf("role %s missing, offline or sharing an identity", role)
		}
		seen[entry.PublicKey] = true
		count++
	}
	if selected.onchip {
		entry, exists := status["management"]
		if !exists || len(entry.PublicKey) != 64 || seen[entry.PublicKey] ||
			entry.State != "running" || entry.Connected == nil || !*entry.Connected ||
			entry.ApplicationError != "" {
			return 0, errors.New("standalone RF management identity is missing, unprovisioned or offline")
		}
		count++
	}
	owner := "observer"
	if !selected.onchip && !enabled(cfg, selected, "observer") {
		owner = "controller"
		entry, exists := status[owner]
		if !exists || entry.State != "running" || entry.Connected == nil || !*entry.Connected ||
			entry.PublicKey != "" {
			return 0, errors.New("configuration controller is missing or offline")
		}
	}
	if !selected.onchip {
		ownerEntry := status[owner]
		if len(ownerEntry.AirtimeMS) != hardware.KISS_MAX_PACKET_SIZE+1 {
			return 0, fmt.Errorf("%s has no physical airtime model", owner)
		}
		phy := ownerEntry.SharedPHY
		expectedOwner := owner
		if cfg.PHYAuthority == "modem" {
			expectedOwner = "modem"
		}
		reserved := cfg.RadioSourceCount()
		if cfg.RoleEnabled("bot") && cfg.BotRuntime != "native_lua" {
			reserved += cfg.BotMaxClients
		}
		if phy == nil || phy.ConfigurationOwner != expectedOwner ||
			(phy.PHYAuthority != "" && phy.PHYAuthority != string(cfg.PHYAuthority)) ||
			(cfg.PHYAuthority == "modem" && phy.PHYAuthority == "") ||
			(cfg.PHYAuthority == "modem" && (phy.CapacitySource == "" ||
				phy.CapacitySource == "unverified" || phy.ExternalClientCapacity < reserved ||
				cfg.RadioClientCapacity == nil && phy.CapacitySource != "reported" ||
				cfg.RadioClientCapacity != nil && phy.ExternalClientCapacity < *cfg.RadioClientCapacity)) {
			return 0, fmt.Errorf("%s has no verified shared PHY configuration", owner)
		}
		if cfg.RequireParity {
			if _, err := sharedPHYReference(cfg, phy); err != nil {
				return 0, fmt.Errorf("%s has no verified shared PHY configuration: %w", owner, err)
			}
		}
		if owner == "observer" {
			connected := status["observer"].MQTTConnected
			if connected == nil || !*connected {
				return 0, errors.New("observer MQTT publisher is disconnected")
			}
		}
	}
	return count, nil
}

func run(cfg app.Config, selected checks) (result error) {
	if selected.factoryReset && (!cfg.CompanionFactoryReset || selected.reboot || selected.transmit || selected.rebootRoles || selected.rekeyRoles) {
		return errors.New("factory-reset check requires companion_factory_reset=true and cannot run with other lifecycle or transmit checks")
	}
	if err := checkSelection(cfg, selected); err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	status, err := readStatus(ctx, cfg, selected.onchip)
	if err != nil {
		return err
	}
	selected.setOnchipRoles(status)
	if err := checkSelection(cfg, selected); err != nil {
		return err
	}
	roleCount, err := checkRoleStatus(cfg, selected, status)
	if err != nil {
		return err
	}
	// Tuning observed through companion SELF_INFO and KISS must match the
	// host's shared PHY reference, which follows the mast in modem-follow mode.
	reference := cfg.Radio
	if cfg.RequireParity && !selected.onchip {
		owner := "observer"
		if !cfg.RoleEnabled("observer") {
			owner = "controller"
		}
		if reference, err = sharedPHYReference(cfg, status[owner].SharedPHY); err != nil {
			return err
		}
	}
	if selected.airtime && !enabled(cfg, selected, "bot") && !selected.onchip {
		owner := "observer"
		if !cfg.RoleEnabled("observer") {
			owner = "controller"
		}
		if phy := status[owner].SharedPHY; phy != nil && phy.ExternalClientCapacity > 0 &&
			cfg.RadioSourceCount() >= phy.ExternalClientCapacity {
			return errors.New("-airtime would exceed the reported external radio client capacity")
		}
	}
	var clients []*client.Client
	var transports []*ctransport.TCPTransport
	var channelIndex byte
	if enabled(cfg, selected, "companion") {
		for i := range 2 {
			transport := ctransport.NewTCPTransport(ctransport.TCPConfig{
				Address: localAddress(cfg.CompanionListen),
			})
			c := client.New(transport)
			if err := c.Connect(ctx); err != nil {
				c.Close()
				return err
			}
			defer c.Close()
			device, err := c.DeviceQuery(ctx)
			if err != nil {
				return fmt.Errorf("client %d device query: %w", i, err)
			}
			if device.FirmwareVersion != 13 {
				return fmt.Errorf("client %d companion protocol %d; expected 13", i, device.FirmwareVersion)
			}
			if device.MaxChannels == 0 {
				return errors.New("companion advertises no channel slots")
			}
			channelIndex = device.MaxChannels - 1
			info, err := c.AppStart(ctx, 3, "meshcore-host-check")
			if err != nil {
				return err
			}
			if hex.EncodeToString(info.PublicKey[:]) != status["companion"].PublicKey {
				return errors.New("companion client did not see the shared base identity")
			}
			if !selected.onchip && !selfInfoMatches(info, reference) {
				return fmt.Errorf("companion SELF_INFO tuning %d kHz/%d Hz/SF%d/CR%d differs from the shared PHY %s",
					info.RadioFrequency, info.RadioBandwidth, info.RadioSpreadFactor, info.RadioCodingRate,
					radio.PHYSettings{Radio: reference})
			}
			contacts, err := c.GetContacts(ctx)
			if err != nil {
				return err
			}
			for _, role := range []string{"room", "repeater"} {
				if !enabled(cfg, selected, role) {
					continue
				}
				found := false
				for _, contact := range contacts {
					if hex.EncodeToString(contact.PublicKey[:]) == status[role].PublicKey {
						found = true
					}
				}
				if !found {
					return fmt.Errorf("companion has not discovered the %s advert", role)
				}
			}
			clients = append(clients, c)
			transports = append(transports, transport)
		}
	}
	var modem *hardware.KissModem
	if enabled(cfg, selected, "bot") {
		modem = hardware.NewKissModem(htransport.NewTCPTransport(htransport.TCPConfig{
			Address: localAddress(cfg.BotListen), ReadIdleTimeout: -1,
		}), hardware.WithSignalReport(true))
		defer modem.Close()
		if err := modem.Connect(ctx); err != nil {
			return err
		}
		identity, err := modem.Request(ctx, hardware.HW_CMD_GET_IDENTITY, nil)
		if err != nil {
			return err
		}
		if hex.EncodeToString(identity) != status["bot"].PublicKey {
			return errors.New("bot KISS identity differs from its persistent role identity")
		}
	}
	if cfg.BotCompanionListen != "" && !selected.onchip {
		entry := status["bot_companion"]
		if entry.Endpoint != cfg.BotCompanionListen || entry.ListenerActive == nil || !*entry.ListenerActive {
			return errors.New("dedicated bot companion listener is unavailable")
		}
		transport := ctransport.NewTCPTransport(ctransport.TCPConfig{Address: localAddress(cfg.BotCompanionListen)})
		bot := client.New(transport)
		if err := bot.Connect(ctx); err != nil {
			bot.Close()
			return fmt.Errorf("bot companion connect: %w", err)
		}
		defer bot.Close()
		device, err := bot.DeviceQuery(ctx)
		if err != nil {
			return fmt.Errorf("bot companion device query: %w", err)
		}
		if device.FirmwareVersion != 13 {
			return fmt.Errorf("bot companion protocol %d; expected 13", device.FirmwareVersion)
		}
		info, err := bot.AppStart(ctx, 3, "meshcore-bot-check")
		if err != nil {
			return fmt.Errorf("bot companion app start: %w", err)
		}
		if hex.EncodeToString(info.PublicKey[:]) != entry.PublicKey {
			return errors.New("bot companion client did not see its distinct role identity")
		}
	}
	if modem != nil {
		tuning, err := modem.RadioConfiguration(ctx)
		if err != nil || tuning == nil || *tuning != reference {
			return fmt.Errorf("bot PHY configuration mismatch: %v", err)
		}
	}
	if selected.airtime {
		measurementModem := modem
		if measurementModem == nil {
			measurementModem = hardware.NewKissModem(htransport.NewTCPTransport(htransport.TCPConfig{
				Address: cfg.RadioAddress, ReadIdleTimeout: -1,
			}), hardware.WithSignalReport(true))
			defer measurementModem.Close()
			if err := measurementModem.Connect(ctx); err != nil {
				return fmt.Errorf("physical airtime check connection: %w", err)
			}
			tuning, err := measurementModem.RadioConfiguration(ctx)
			if err != nil || tuning == nil || *tuning != reference {
				return fmt.Errorf("physical airtime check PHY mismatch: %v", err)
			}
		}
		owner := "observer"
		if !enabled(cfg, selected, "observer") {
			owner = "controller"
		}
		estimates := status[owner].AirtimeMS
		if len(estimates) != hardware.KISS_MAX_PACKET_SIZE+1 {
			return errors.New("host did not report its physical airtime model")
		}
		var mismatches error
		for _, length := range []byte{20, 149, 255} {
			data, err := measurementModem.Request(ctx, hardware.HW_CMD_GET_AIRTIME, []byte{length})
			if err != nil {
				return fmt.Errorf("%d-byte modem airtime query: %w", length, err)
			}
			if len(data) != 4 {
				return errors.New("invalid physical airtime response")
			}
			physical := binary.LittleEndian.Uint32(data)
			host := estimates[length]
			fmt.Printf("airtime %d bytes: host=%dms modem=%dms\n", length, host, physical)
			if host != physical {
				mismatches = errors.Join(mismatches, fmt.Errorf("%d-byte airtime differs: host=%dms modem=%dms", length, host, physical))
			}
		}
		if mismatches != nil {
			return mismatches
		}
	}
	deployment := "host"
	if selected.onchip {
		deployment = "standalone"
	}
	fmt.Printf("%s readiness passed: %d distinct identities", deployment, roleCount)
	if enabled(cfg, selected, "companion") {
		fmt.Print(", two base companion clients")
	}
	if enabled(cfg, selected, "room") && enabled(cfg, selected, "companion") {
		fmt.Print(", room discovery")
	}
	if enabled(cfg, selected, "repeater") && enabled(cfg, selected, "companion") {
		fmt.Print(", repeater discovery")
	}
	if modem != nil {
		fmt.Print(", bot KISS")
	}
	if !selected.onchip && !enabled(cfg, selected, "observer") {
		fmt.Print(", configuration controller")
	}
	if !selected.onchip && cfg.BotCompanionListen != "" {
		fmt.Print(", dedicated bot companion")
	}
	fmt.Println()
	if selected.rebootRoles || selected.rekeyRoles {
		if err := checkRoleRestarts(ctx, cfg, clients, modem, selected.rekeyRoles, selected); err != nil {
			return err
		}
	}
	if selected.reboot || selected.factoryReset {
		seen := make(map[string]bool)
		for _, entry := range status {
			if entry.PublicKey != "" {
				seen[entry.PublicKey] = true
			}
		}
		operation := "reboot"
		if selected.factoryReset {
			operation = "factory reset"
		}
		disconnected, reconnected := make(chan int, 4), make(chan int, 4)
		for index, transport := range transports {
			transport.SetDisconnectHandler(func() {
				select {
				case disconnected <- index:
				default:
				}
			})
			transport.SetReconnectHandler(func() {
				select {
				case reconnected <- index:
				default:
				}
			})
		}
		savedChannel, err := clients[0].GetChannel(ctx, channelIndex)
		if err != nil {
			return err
		}
		var rebootKey [16]byte
		if _, err := rand.Read(rebootKey[:]); err != nil {
			return err
		}
		if err := clients[0].SetChannel(ctx, channelIndex, "MeshCore Lifecycle Check", rebootKey); err != nil {
			return err
		}
		if !selected.factoryReset {
			defer func() {
				restoreCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
				defer cancel()
				result = errors.Join(result, clients[0].SetChannel(restoreCtx, channelIndex, savedChannel.Name, savedChannel.Secret))
			}()
		}
		if selected.factoryReset {
			err = clients[0].FactoryReset()
		} else {
			err = clients[0].Reboot()
		}
		if err != nil {
			return err
		}
		for _, phase := range []struct {
			name   string
			events <-chan int
		}{{"disconnect", disconnected}, {"reconnect", reconnected}} {
			deadline := time.NewTimer(15 * time.Second)
			observed := map[int]bool{}
			for len(observed) < len(clients) {
				select {
				case index := <-phase.events:
					observed[index] = true
				case <-deadline.C:
					return fmt.Errorf("companion %s did not %s both clients", operation, phase.name)
				case <-ctx.Done():
					deadline.Stop()
					return ctx.Err()
				}
			}
			deadline.Stop()
		}
		if _, err := modem.Request(ctx, hardware.HW_CMD_PING, nil); err != nil {
			return fmt.Errorf("companion %s disrupted the existing bot/radio connection: %w", operation, err)
		}
		current, err := readStatus(ctx, cfg, selected.onchip)
		if err != nil {
			return err
		}
		for role, before := range status {
			if before.State == "disabled" {
				continue
			}
			after, exists := current[role]
			if after.DeviceUptime < before.DeviceUptime {
				return errors.New("companion lifecycle restarted the physical modem")
			}
			if !exists || (role != "bot" && (after.Connected == nil || !*after.Connected)) {
				return fmt.Errorf("companion %s left %s offline or missing", operation, role)
			}
			if role == "companion" && selected.factoryReset {
				if len(after.PublicKey) != 64 || seen[after.PublicKey] {
					return errors.New("factory reset did not create an independent companion identity")
				}
			} else if before.PublicKey != after.PublicKey {
				return fmt.Errorf("companion %s changed the %s identity", operation, role)
			}
		}
		for _, c := range clients {
			info, err := c.AppStart(ctx, 3, "meshcore-host-check")
			if err != nil {
				return err
			}
			if hex.EncodeToString(info.PublicKey[:]) != current["companion"].PublicKey {
				return errors.New("reconnected client and host disagree on the companion identity")
			}
			channel, err := c.GetChannel(ctx, channelIndex)
			if err != nil {
				return err
			}
			if selected.factoryReset {
				if channel.Name != "" || channel.Secret != ([16]byte{}) {
					return errors.New("factory reset retained the changed companion channel")
				}
				continue
			}
			if channel.Name != "MeshCore Lifecycle Check" || channel.Secret != rebootKey {
				return errors.New("logical reboot lost companion channel state")
			}
			contacts, err := c.GetContacts(ctx)
			if err != nil {
				return err
			}
			for _, role := range []string{"room", "repeater"} {
				if !enabled(cfg, selected, role) {
					continue
				}
				found := false
				for _, contact := range contacts {
					found = found || hex.EncodeToString(contact.PublicKey[:]) == status[role].PublicKey
				}
				if !found {
					return fmt.Errorf("logical reboot lost the %s contact", role)
				}
			}
		}
		if selected.factoryReset {
			fmt.Println("companion factory reset passed: both clients reconnected, fresh independent identity and channel defaults, sibling identities and existing bot/radio connection retained")
		} else {
			fmt.Println("companion reboot passed: both clients reconnected, identity/contacts/channel retained, existing bot/radio connection survived")
		}
	}
	if !selected.transmit {
		return nil
	}
	oldChannel, err := clients[0].GetChannel(ctx, channelIndex)
	if err != nil {
		return fmt.Errorf("reading temporary test channel: %w", err)
	}
	var psk [16]byte
	if _, err := rand.Read(psk[:]); err != nil {
		return err
	}
	if err := clients[0].SetChannel(ctx, channelIndex, "MeshCore Host Check", psk); err != nil {
		return err
	}
	defer func() {
		restoreCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		result = errors.Join(result, clients[0].SetChannel(restoreCtx, channelIndex, oldChannel.Name, oldChannel.Secret))
	}()
	shared, err := clients[1].GetChannel(ctx, channelIndex)
	if err != nil || shared.Secret != psk || shared.Name != "MeshCore Host Check" {
		return fmt.Errorf("channel change not visible to second client: %v", err)
	}
	text := "MeshCore Host Check " + hex.EncodeToString(psk[:6])
	group, err := (&meshcore.GroupTextPayload{
		Timestamp: uint32(time.Now().Unix()), Sender: "Host check", Text: text,
	}).Encrypt(meshcore.DeriveChannelHash(psk), psk[:])
	if err != nil {
		return err
	}
	payload, err := group.ToBytes()
	if err != nil {
		return err
	}
	packet, err := (&meshcore.Packet{
		Header:  meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeGrpTxt, 0),
		Payload: payload,
	}).ToBytes()
	if err != nil {
		return err
	}
	packetHex := hex.EncodeToString(packet)
	observed := make(chan error, 1)
	options := mqtt.NewClientOptions().AddBroker(cfg.MQTT.URL).
		SetClientID("meshcore-check-" + hex.EncodeToString(psk[:6])).
		SetConnectTimeout(5 * time.Second)
	if cfg.MQTT.UsernameEnv != "" {
		options.SetUsername(os.Getenv(cfg.MQTT.UsernameEnv))
	}
	if cfg.MQTT.PasswordEnv != "" {
		options.SetPassword(os.Getenv(cfg.MQTT.PasswordEnv))
	}
	sub := mqtt.NewClient(options)
	if err := mqttWait(sub.Connect()); err != nil {
		return err
	}
	defer sub.Disconnect(250)
	topic := cfg.MQTT.TopicPrefix + "/" + status["observer"].PublicKey + "/packets"
	if err := mqttWait(sub.Subscribe(topic, 1, func(_ mqtt.Client, message mqtt.Message) {
		var event struct {
			Raw   string   `json:"raw_packet_hex"`
			Local bool     `json:"local_loopback"`
			RSSI  *float64 `json:"rssi"`
			SNR   *float64 `json:"snr"`
		}
		if err := json.Unmarshal(message.Payload(), &event); err != nil || event.Raw != packetHex {
			return
		}
		var err error
		if !event.Local || event.RSSI != nil || event.SNR != nil {
			err = errors.New("MQTT local reflection reported fictional RF signal values")
		}
		select {
		case observed <- err:
		default:
		}
	})); err != nil {
		return err
	}
	if err := modem.SendData(packet); err != nil {
		return err
	}
	select {
	case err := <-observed:
		if err != nil {
			return err
		}
	case <-ctx.Done():
		return errors.New("observer did not publish the bot transmission to MQTT")
	}
	for index, c := range clients {
		found := false
		for !found && ctx.Err() == nil {
			messages, err := c.GetWaitingMessages(ctx)
			if err != nil {
				return err
			}
			for _, message := range messages {
				if message.Channel != nil && message.Channel.ChannelIdx == channelIndex &&
					strings.Contains(message.Channel.Text, text) {
					found = true
				}
			}
			if !found {
				select {
				case <-ctx.Done():
				case <-time.After(100 * time.Millisecond):
				}
			}
		}
		if !found {
			return fmt.Errorf("companion client %d lost or stole the shared incoming message", index)
		}
	}
	room, err := meshcore.NewIdentityFromHex(status["room"].PublicKey)
	if err != nil {
		return err
	}
	logins := []chan struct{}{make(chan struct{}, 1), make(chan struct{}, 1)}
	for index, c := range clients {
		stop := c.OnPush(companion.PushLoginSuccess, func(response companion.Response) {
			if login, ok := response.Data.(companion.PushLoginSuccessResponse); ok &&
				login.PubKeyPrefix == room.Prefix() {
				select {
				case logins[index] <- struct{}{}:
				default:
				}
			}
		})
		defer stop()
	}
	password := cfg.RoomPassword
	if cfg.RoomPasswordEnv != "" {
		password = os.Getenv(cfg.RoomPasswordEnv)
	}
	if err := clients[0].SendLogin(ctx, room, password); err != nil {
		return err
	}
	for index, login := range logins {
		select {
		case <-login:
		case <-ctx.Done():
			return fmt.Errorf("companion client %d did not receive room login confirmation", index)
		}
	}
	acks := make(chan uint32, 16)
	stop := clients[0].OnPush(companion.PushSendConfirmed, func(response companion.Response) {
		if ack, ok := response.Data.(companion.PushSendConfirmedResponse); ok {
			select {
			case acks <- ack.AckCode:
			default:
			}
		}
	})
	defer stop()
	sent, err := clients[0].SendTextMessage(ctx, room, text, 0)
	if err != nil {
		return err
	}
	expectedACK := sent.AckCode
	if sent.HasExtended {
		expectedACK = sent.Tag
	} else if !sent.HasAckCode {
		return errors.New("companion send response omitted the expected acknowledgement")
	}
	for {
		select {
		case ack := <-acks:
			if ack == expectedACK {
				fmt.Println("host local integration passed: bot TX, MQTT local reflection, independent companion inboxes, shared room login and acknowledged post")
				return nil
			}
		case <-ctx.Done():
			return errors.New("room did not acknowledge the posted message")
		}
	}
}

func mqttWait(token mqtt.Token) error {
	if !token.WaitTimeout(5 * time.Second) {
		return errors.New("MQTT operation timed out")
	}
	return token.Error()
}

func main() {
	var selected checks
	path := flag.String("config", "meshcore-host.json", "host configuration JSON")
	flag.BoolVar(&selected.transmit, "transmit", false, "send an encrypted test packet, log into the room and post a test message")
	flag.BoolVar(&selected.airtime, "airtime", false, "compare host timing estimates with the physical modem without transmitting")
	flag.BoolVar(&selected.reboot, "reboot", false, "restart only the companion and verify client reconnection and retained state")
	flag.BoolVar(&selected.factoryReset, "factory-reset", false, "DESTRUCTIVE: replace the staging companion identity and reset its saved data")
	flag.BoolVar(&selected.rebootRoles, "reboot-roles", false, "restart repeater and room individually through encrypted admin commands")
	flag.BoolVar(&selected.rekeyRoles, "rekey-roles", false, "DESTRUCTIVE: stage fresh repeater/room identities and activate them through encrypted reboot commands")
	flag.BoolVar(&selected.onchip, "onchip", false, "exercise native on-chip roles using the ESP32 status API")
	flag.Parse()
	cfg, err := app.LoadConfig(*path)
	if err == nil {
		err = run(cfg, selected)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "meshcore-check:", err)
		os.Exit(1)
	}
}
