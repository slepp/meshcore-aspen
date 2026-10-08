package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"path/filepath"
	"sync"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/kissproxy"
	nativebot "meshcore.local/meshcore/internal/nativebot/host"
	"meshcore.local/meshcore/internal/nodebackup"
	"meshcore.local/meshcore/internal/observer"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/roles"
	"meshcore.local/meshcore/internal/state"
)

type roleStatus struct {
	ApplicationStartedAt  *time.Time                `json:"application_started_at"`
	ApplicationKind       string                    `json:"application_kind,omitempty"`
	PublicKey             string                    `json:"public_key"`
	RadioConnected        *bool                     `json:"radio_connected,omitempty"`
	RolePresence          *radio.RolePresenceStatus `json:"role_presence,omitempty"`
	PHYGeneration         uint32                    `json:"phy_configuration_generation,omitempty"`
	Warning               string                    `json:"warning,omitempty"`
	SharedPHY             *sharedPHYStatus          `json:"shared_phy,omitempty"`
	CompanionRetention    *retentionStatus          `json:"companion_retention,omitempty"`
	Endpoint              string                    `json:"endpoint,omitempty"`
	Telemetry             *radio.Telemetry          `json:"telemetry,omitempty"`
	TelemetryError        string                    `json:"telemetry_error,omitempty"`
	AirtimeMS             []uint32                  `json:"airtime_ms,omitempty"`
	State                 string                    `json:"state,omitempty"`
	ApplicationError      string                    `json:"application_error,omitempty"`
	RoleTX                *txSnapshot               `json:"role_tx,omitempty"`
	TelemetryCounterScope string                    `json:"telemetry_counter_scope,omitempty"`
	ListenerActive        *bool                     `json:"listener_active,omitempty"`
	MQTTConnected         *bool                     `json:"mqtt_connected,omitempty"`
	ObserverStats         *observer.Stats           `json:"observer_stats,omitempty"`
}

func (s *roleStatus) setRolePresence(presence *radio.RolePresenceStatus) {
	s.RolePresence = presence
	s.Warning = ""
	if presence != nil && presence.Online {
		s.Warning = presence.Warning
	}
}

// setPHY reports which configuration generation this role's source follows.
func (s *roleStatus) setPHY(link interface {
	EffectivePHY() (radio.PHYState, bool)
}) {
	if state, ok := link.EffectivePHY(); ok {
		s.PHYGeneration = state.ConfigurationGeneration
	}
}

// radioScore uses the spreading factor currently verified on the source that
// received the packet, falling back to the configured value while unknown.
func radioScore(fallback hardware.RadioConfig, effective func() (radio.PHYState, bool)) func(*meshcore.Packet, int, uint32) (float32, error) {
	return func(packet *meshcore.Packet, length int, _ uint32) (float32, error) {
		if !packet.HasSignalInfo {
			return 0, errors.New("RX score requires an over-the-air signal measurement")
		}
		sf := fallback.SF
		if effective != nil {
			if state, ok := effective(); ok {
				sf = state.Settings.Radio.SF
			}
		}
		return float32(hardware.PacketScore(float64(packet.SNR), sf, length)), nil
	}
}

func (c Config) configurationLink(role string, logger *slog.Logger, profile *radio.PHYProfile, txHandler func(radio.TXResult)) radio.Config {
	return radio.Config{
		Address: c.RadioAddress, Radio: c.Radio, TxPower: c.TxPower,
		RequireParity: c.RequireParity, ConfigurationOwner: c.PHYAuthority == "host",
		PHYTracking: c.radioPHYTracking(), PHYGroup: c.phyGroup,
		PHYProfile: profile, Logger: logger.With("role", role), TXResultHandler: txHandler,
	}
}

func roleAnnouncement(role string, identity meshcore.LocalIdentity) *radio.RoleAnnouncement {
	roles := map[string]radio.MastRole{
		"repeater": radio.RepeaterRole, "room": radio.RoomRole,
		"companion": radio.CompanionRole, "observer": radio.ObserverRole,
	}
	id, ok := roles[role]
	if !ok {
		return nil
	}
	var key [32]byte
	copy(key[:], identity.PublicKeyBytes())
	return &radio.RoleAnnouncement{Role: id, PublicKey: key}
}

// botProxyConfig checks bot legacy setters against the configured profile, or
// when following the mast, against the configuration link's live readback.
func (c Config) botProxyConfig(owner interface {
	EffectivePHY() (radio.PHYState, bool)
}, logger *slog.Logger) kissproxy.Config {
	config := kissproxy.Config{
		Upstream: c.RadioAddress, Radio: c.Radio, TxPower: c.TxPower,
		MaxClients: c.BotMaxClients, RequireParity: c.RequireParity, Logger: logger,
	}
	if c.FollowsModemPHY() {
		config.EffectivePHY = func() (hardware.RadioConfig, uint8, bool) {
			state, ok := owner.EffectivePHY()
			return state.Settings.Radio, state.Settings.TxPower, ok
		}
	}
	return config
}

func (c Config) openRoleSource(ctx context.Context, role string, identity meshcore.LocalIdentity, logger *slog.Logger, profile *radio.PHYProfile) (*radio.Link, error) {
	return c.openRoleSourceOn(ctx, role, identity, logger, profile, nil, 0)
}

func (c Config) openRoleSourceOn(ctx context.Context, role string, identity meshcore.LocalIdentity, logger *slog.Logger, profile *radio.PHYProfile, session *radio.Session, port int) (*radio.Link, error) {
	var announcement *radio.RoleAnnouncement
	if c.RequireParity {
		announcement = roleAnnouncement(role, identity)
	}
	return radio.Open(ctx, radio.Config{
		Address: c.RadioAddress, Radio: c.Radio, TxPower: c.TxPower,
		RequireParity: c.RequireParity, ConfigurationOwner: false,
		PHYTracking: c.radioPHYTracking(), PHYGroup: c.phyGroup,
		PHYProfile: profile, Logger: logger.With("role", role),
		RoleAnnouncement: announcement,
		Session:          session, SessionPort: port,
	})
}

func Run(ctx context.Context, cfg Config, logger *slog.Logger) (result error) {
	if err := cfg.Preflight(false); err != nil {
		return err
	}
	roomPassword, err := cfg.roomAccessPassword()
	if err != nil {
		return err
	}
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	var cleanups []func() error
	defer func() {
		cancel()
		for i := len(cleanups) - 1; i >= 0; i-- {
			result = errors.Join(result, cleanups[i]())
		}
	}()
	txDiagnostics := newTXDiagnostics(logger)
	cleanups = append(cleanups, txDiagnostics.Close)
	lock, err := state.Lock(cfg.StateDir)
	if err != nil {
		return err
	}
	cleanups = append(cleanups, lock.Close)
	roleWorkers := make(map[string]*roleWorker)
	backupReady := make(chan struct{})
	cfg.nodeBackup, err = nodebackup.New(ctx, filepath.Join(cfg.StateDir, "node-backup"),
		func(snapshotContext context.Context) (map[string][]byte, error) {
			select {
			case <-backupReady:
			case <-snapshotContext.Done():
				return nil, snapshotContext.Err()
			}
			for role, owner := range newHostRoleAdmin(roleWorkers) {
				if _, err := owner(snapshotContext, "get name"); err != nil {
					return nil, fmt.Errorf("backup %s state barrier: %w", role, err)
				}
			}
			return hostBackupSnapshot(snapshotContext, cfg)
		})
	if err != nil {
		return fmt.Errorf("host backup storage: %w", err)
	}
	cleanups = append(cleanups, cfg.nodeBackup.Close)
	if err := activatePendingRoleIdentities(cfg); err != nil {
		return err
	}
	identities := make(map[string]meshcore.LocalIdentity)
	var roleNames []string
	for _, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
		if cfg.roleEnabled(role) {
			roleNames = append(roleNames, role)
		}
	}
	if cfg.BotCompanionListen != "" {
		roleNames = append(roleNames, "bot_companion")
	}
	publicKeys := make(map[string]string)
	for _, role := range roleNames {
		identity, err := state.Identity(cfg.StateDir, role)
		if err != nil {
			return fmt.Errorf("%s identity: %w", role, err)
		}
		if prior := publicKeys[identity.String()]; prior != "" {
			return fmt.Errorf("%s and %s have the same role identity", prior, role)
		}
		publicKeys[identity.String()] = role
		identities[role] = identity
		logger.Info("role identity", "role", role, "public_key", identity.String())
	}
	var adminPassword string
	if cfg.roleEnabled("room") || cfg.roleEnabled("repeater") {
		adminPassword, err = envSecret(cfg.AdminPasswordEnv)
		if err != nil {
			return err
		}
	}
	var obs *observer.Observer
	var mqttUsername, mqttPassword string
	if cfg.roleEnabled("observer") {
		mqttUsername, err = envSecret(cfg.MQTT.UsernameEnv)
		if err != nil {
			return err
		}
		mqttPassword, err = envSecret(cfg.MQTT.PasswordEnv)
		if err != nil {
			return err
		}
		if cfg.MQTT.BrokerListen != "" {
			broker, err := observer.StartBroker(observer.BrokerConfig{
				Address: cfg.MQTT.BrokerListen, Logger: logger,
				Username: mqttUsername, Password: mqttPassword,
			})
			if err != nil {
				return err
			}
			cleanups = append(cleanups, broker.Close)
		}
		obs, err = observer.New(identities["observer"].String(), observer.Config{
			URL: cfg.MQTT.URL, ClientID: "meshcore-observer-" + identities["observer"].String()[:12],
			TopicPrefix: cfg.MQTT.TopicPrefix, Username: mqttUsername, Password: mqttPassword,
			QueueSize: 256, Logger: logger.With("role", "observer"),
			Format: cfg.MQTT.Format, IATA: cfg.MQTT.IATA, Origin: cfg.MQTT.Origin,
			Audience: cfg.MQTT.Audience, Sign: identities["observer"].Sign,
			PacketFilter: cfg.MQTT.PacketFilter,
		})
		if err != nil {
			return err
		}
		cleanups = append(cleanups, obs.Close)
		if err := obs.Start(ctx); err != nil {
			return err
		}
	}
	var phyProfile *radio.PHYProfile
	if cfg.PHYProfile != nil {
		profile := radio.PHYProfile(*cfg.PHYProfile)
		phyProfile = &profile
	}
	owner := "controller"
	if obs != nil {
		owner = "observer"
	}
	// Every link to the mast shares one airtime model and PHY-change prompts.
	cfg.phyGroup = radio.NewPHYGroup()
	var observerTX *roleTX
	if obs != nil {
		observerTX = newRoleTX("observer", cfg.RequireParity, txDiagnostics)
	}
	ownerConfig := cfg.configurationLink(owner, logger, phyProfile, nil)
	if obs != nil && cfg.RequireParity {
		ownerConfig.RoleAnnouncement = roleAnnouncement("observer", identities["observer"])
	}
	if observerTX != nil {
		ownerConfig.TXResultHandler = observerTX.record
	}
	ownerLink, session, err := cfg.openOwnerRadio(ctx, ownerConfig, logger)
	if err != nil {
		return fmt.Errorf("%s radio: %w", owner, err)
	}
	cleanups = append(cleanups, ownerLink.Close)
	ownerStartedAt := time.Now().UTC()
	if obs != nil {
		ownerLink.SetDataHandler(obs.Observe)
	}
	cfg.sessionActive = session != nil
	var reader capacityReader = ownerLink
	if session != nil {
		reader = sessionCapacityReader{session}
	}
	capacity, err := cfg.checkRadioCapacity(ctx, reader, logger)
	if err != nil {
		return err
	}
	cfg.radioCapacity = &capacity
	if session == nil && (cfg.RadioSession == "auto") && cfg.reservedClients(false) > capacity.external {
		return fmt.Errorf("per-role fallback needs %d connections, but only %d are available", cfg.reservedClients(false), capacity.external)
	}
	rolePorts := cfg.sessionRolePorts(capacity.ports)
	if session != nil {
		if err := session.ReserveCapacity(1+len(rolePorts), cfg.sessionReservedClientsFor(capacity.ports)); err != nil {
			return fmt.Errorf("reserving selected role capacity: %w", err)
		}
	}
	openRoleSource := func(ctx context.Context, role string, identity meshcore.LocalIdentity) (sourceLink, error) {
		if virtualPort := rolePorts[role]; session != nil && virtualPort != 0 {
			return cfg.openRoleSourceOn(ctx, role, identity, logger, phyProfile, session, virtualPort)
		}
		return cfg.openRoleSource(ctx, role, identity, logger, phyProfile)
	}
	var base *companionWorker
	if cfg.roleEnabled("companion") {
		base, err = startCompanion(ctx, cfg, identities, logger, txDiagnostics, func(ctx context.Context, identity meshcore.LocalIdentity) (sourceLink, error) {
			return openRoleSource(ctx, "companion", identity)
		})
		if err != nil {
			return err
		}
		cleanups = append(cleanups, base.Close)
	}
	var botCompanion *companionWorker
	if cfg.BotCompanionListen != "" {
		botCompanion, err = startCompanionRole(ctx, cfg, "bot_companion", identities, logger, txDiagnostics,
			func(ctx context.Context, identity meshcore.LocalIdentity) (sourceLink, error) {
				return openRoleSource(ctx, "bot_companion", identity)
			})
		if err != nil {
			return fmt.Errorf("bot companion startup: %w", err)
		}
		cleanups = append(cleanups, botCompanion.Close)
	}
	for _, role := range []string{"room", "repeater"} {
		if !cfg.roleEnabled(role) {
			continue
		}
		name := cfg.RepeaterName
		rolePolicy := cfg.RepeaterPolicy
		if role == "room" {
			name = cfg.RoomName
			rolePolicy = cfg.RoomPolicy
		}
		worker, err := startRole(ctx, cfg, role, roles.Config{
			StateDir: filepath.Join(cfg.StateDir, role), Name: name,
			Password: roomPassword, AdminPassword: adminPassword, MaxHistory: 32,
			Policy: cfg.withLegacyAdvert(rolePolicy),
		}, logger, txDiagnostics, func(ctx context.Context, identity meshcore.LocalIdentity) (sourceLink, error) {
			return openRoleSource(ctx, role, identity)
		})
		if err != nil {
			return fmt.Errorf("%s startup: %w", role, err)
		}
		roleWorkers[role] = worker
		cleanups = append(cleanups, worker.Close)
	}
	failures := make(chan error, 2)
	var workers sync.WaitGroup
	cleanups = append(cleanups, func() error { workers.Wait(); return nil })
	var botListener *trackedListener
	var nativeWorker *nativebot.Service
	var nativeLink *radio.Link
	var botStartedAt time.Time
	if cfg.roleEnabled("bot") {
		if cfg.BotRuntime == "native_lua" {
			source, sourceErr := openRoleSource(ctx, "bot", identities["bot"])
			err = sourceErr
			if err != nil {
				return fmt.Errorf("native bot source: %w", err)
			}
			var ok bool
			nativeLink, ok = source.(*radio.Link)
			if !ok {
				return errors.New("native bot source must expose queued PHY receipts")
			}
			cleanups = append(cleanups, nativeLink.Close)
			expanded, exportErr := state.ExportIdentity(cfg.StateDir, "bot")
			if exportErr != nil {
				return fmt.Errorf("native bot identity: %w", exportErr)
			}
			nativeWorker, err = nativebot.Start(ctx, nativebot.Config{
				Executable: cfg.BotNativeWorker, StateDir: filepath.Join(cfg.StateDir, "bot", "native"),
				Identity: identities["bot"], Expanded: expanded, Link: nativeLink,
				Logger: logger.With("role", "bot"),
			})
			clear(expanded)
			if err != nil {
				return fmt.Errorf("native bot startup: %w", err)
			}
			cleanups = append(cleanups, nativeWorker.Close)
			botStartedAt = time.Now().UTC()
		} else {
			proxy := kissproxy.New(identities["bot"], cfg.botProxyConfig(ownerLink, logger.With("role", "bot")))
			botSocket, listenErr := net.Listen("tcp", cfg.BotListen)
			if listenErr != nil {
				return fmt.Errorf("bot listener: %w", listenErr)
			}
			botStartedAt = time.Now().UTC()
			botListener = trackListener(botSocket)
			workers.Add(1)
			go func() {
				defer workers.Done()
				err := proxy.Serve(ctx, botListener)
				botListener.active.Store(false)
				failures <- err
			}()
			logger.Info("endpoint listening", "role", "bot", "address", botListener.Addr())
		}
	}
	close(backupReady)
	mux := newStatusHandler(ctx, func() map[string]roleStatus {
		status := make(map[string]roleStatus)
		for _, role := range []string{"repeater", "room", "companion", "observer", "bot"} {
			if !cfg.roleEnabled(role) {
				status[role] = roleStatus{State: "disabled"}
				continue
			}
			entry := roleStatus{PublicKey: identities[role].String()}
			if role == "companion" {
				status[role] = base.status()
				continue
			}
			if worker := roleWorkers[role]; worker != nil {
				status[role] = worker.status()
				continue
			}
			if role == "observer" {
				entry = cfg.ownerStatus(ownerStartedAt, ownerLink)
				entry.PublicKey = identities[role].String()
				entry.setRolePresence(ownerLink.RolePresenceStatus())
				connected, stats := obs.Connected(), obs.Stats()
				entry.MQTTConnected, entry.ObserverStats = &connected, &stats
				tx := observerTX.snapshot()
				entry.RoleTX = &tx
			}
			if role == "bot" {
				if nativeWorker != nil {
					entry.PublicKey = nativeWorker.PublicKey()
					entry.ApplicationKind = "native_lua"
					entry.setPHY(nativeLink)
					connected := nativeLink.PHYStatus().Online
					entry.RadioConnected = &connected
					native := nativeWorker.Snapshot()
					entry.State = "starting"
					if native.Ready {
						entry.State = "running"
					}
					if native.Fault {
						entry.State = "faulted"
						entry.ApplicationError = "native bot reported a source or VM fault"
						if fault := nativeWorker.Error(); fault != "" {
							entry.ApplicationError = fault + "; inspect key bot and retry key bot apply"
						}
					}
					entry.ApplicationStartedAt = &botStartedAt
				} else {
					active := botListener.active.Load()
					entry.ListenerActive = &active
					entry.ApplicationKind = "kiss_proxy"
					entry.State = "stopped"
					if active {
						entry.ApplicationStartedAt = &botStartedAt
						entry.State = "running"
					}
					entry.Endpoint = cfg.BotListen
				}
				status[role] = entry
				continue
			}
			status[role] = entry
		}
		if botCompanion != nil {
			status["bot_companion"] = botCompanion.status()
		}
		if obs == nil {
			status["controller"] = cfg.ownerStatus(ownerStartedAt, ownerLink)
		}
		return status
	}, logger, cfg, newHostRoleAdmin(roleWorkers))
	status := newStatusServer(mux)
	listener, err := net.Listen("tcp", cfg.StatusListen)
	if err != nil {
		return err
	}
	cleanups = append(cleanups, status.Close)
	workers.Add(1)
	go func() {
		defer workers.Done()
		err := status.Serve(listener)
		if errors.Is(err, http.ErrServerClosed) {
			err = nil
		}
		failures <- err
	}()
	logger.Info("host endpoints listening", "status", cfg.StatusListen, "radio", cfg.RadioAddress)
	var botFatal <-chan error
	if nativeWorker != nil {
		botFatal = nativeWorker.Fatal()
	}
	select {
	case <-ctx.Done():
		return nil
	case err := <-ownerLink.Fatal():
		return fmt.Errorf("KISS session lost subports; supervisor restart required (radio_session=%s): %w", cfg.RadioSession, err)
	case err := <-botFatal:
		return fmt.Errorf("native bot stopped: %w", err)
	case err := <-failures:
		if err == nil && ctx.Err() == nil {
			return errors.New("host listener stopped unexpectedly")
		}
		return err
	}
}
