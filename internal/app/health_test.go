package app

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/observer"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/roles"
)

func onlineHealthSnapshot() map[string]roleStatus {
	started := time.Now().UTC()
	online := true
	status := make(map[string]roleStatus)
	for _, role := range []string{"room", "repeater", "companion", "observer"} {
		status[role] = roleStatus{
			State: "running", ApplicationStartedAt: &started, RadioConnected: &online,
			Telemetry: &radio.Telemetry{},
		}
	}
	observer := status["observer"]
	observer.MQTTConnected = &online
	status["observer"] = observer
	status["bot"] = roleStatus{State: "running", ApplicationStartedAt: &started, ListenerActive: &online}
	return status
}

func requestReadiness(t *testing.T, handler http.Handler, wantCode int, role, reason string) readiness {
	t.Helper()
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/readyz", nil))
	var body readiness
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if response.Code != wantCode || body.Ready != (wantCode == http.StatusOK) ||
		response.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("readiness HTTP %d: %s", response.Code, response.Body.String())
	}
	if role != "" && body.NotReady[role] != reason {
		t.Fatalf("readiness omitted %s/%s: %+v", role, reason, body)
	}
	return body
}

func requestStatus(t *testing.T, handler http.Handler) map[string]roleStatus {
	t.Helper()
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/status", nil))
	var body map[string]roleStatus
	if response.Code != http.StatusOK {
		t.Fatalf("detailed status unavailable: %d", response.Code)
	}
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	return body
}

// configuredPHY is the readback of a mast that matches cfg.
func configuredPHY(cfg Config, online bool) radio.PHYStatus {
	profile := radio.PHYProfile{AirtimeFactor: 1}
	if cfg.PHYProfile != nil {
		profile = radio.PHYProfile(*cfg.PHYProfile)
	}
	profile.AirtimeFactor = float64(float32(profile.AirtimeFactor))
	status := radio.PHYStatus{Tracking: cfg.radioPHYTracking(), Online: online}
	if online && cfg.RequireParity {
		status.Effective = &radio.PHYState{
			ConfigurationGeneration: 1,
			Settings:                radio.PHYSettings{Radio: cfg.Radio, TxPower: cfg.TxPower, Profile: profile},
		}
	}
	return status
}

type fakeOwner struct {
	phy       radio.PHYStatus
	airtime   []uint32
	telemetry error
}

func (o fakeOwner) PHYStatus() radio.PHYStatus { return o.phy }
func (o fakeOwner) AirtimeTable() []uint32     { return o.airtime }
func (o fakeOwner) Snapshot() (radio.Telemetry, error) {
	return radio.Telemetry{}, o.telemetry
}

func TestStatusReportsVerifiedSharedPHYOnlyWhileObserverIsConnected(t *testing.T) {
	cfg := DefaultConfig()
	cfg.RoomPassword = "not-for-status"
	cfg.PHYProfile = &PHYProfile{AirtimeFactor: 2.0004, CADEnabled: true, InterferenceThreshold: -8}
	connected := true
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus {
		phy := cfg.sharedPHYStatus(configuredPHY(cfg, connected))
		return map[string]roleStatus{"observer": {SharedPHY: &phy}}
	}, slog.Default(), cfg)
	read := func() (sharedPHYStatus, string) {
		t.Helper()
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/status", nil))
		if response.Code != http.StatusOK || response.Header().Get("Cache-Control") != "no-store" {
			t.Fatalf("status HTTP %d: %s", response.Code, response.Body.String())
		}
		var body map[string]struct {
			SharedPHY sharedPHYStatus `json:"shared_phy"`
		}
		if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
			t.Fatal(err)
		}
		return body["observer"].SharedPHY, response.Body.String()
	}
	phy, raw := read()
	if !phy.Valid || !phy.OwnerConnected || phy.ConfigurationOwner != "observer" ||
		phy.Error != "" || phy.Effective == nil || phy.Requested.AirtimeFactor != 2.0004 ||
		phy.Effective.AirtimeFactor != float64(float32(2.0004)) ||
		phy.Effective.FrequencyHz != cfg.Radio.FreqHz ||
		phy.Effective.BandwidthHz != cfg.Radio.BwHz ||
		phy.Effective.SpreadingFactor != cfg.Radio.SF ||
		phy.Effective.CodingRate != cfg.Radio.CR ||
		phy.Effective.TxPowerDBm != cfg.TxPower ||
		!phy.Effective.CADEnabled || phy.Effective.InterferenceThreshold != -8 ||
		!strings.Contains(raw, `"frequency_hz":910525000`) ||
		!strings.Contains(raw, `"effective":{`) || !strings.Contains(raw, `"error":""`) ||
		strings.Contains(raw, cfg.RoomPassword) {
		t.Fatalf("verified profile shape, float32 readback or secret exposure: %s", raw)
	}
	// A readback mismatch on reconnect leaves radio.Link offline. The last
	// verified profile must not be offered as the current effective value.
	connected = false
	phy, raw = read()
	if phy.Valid || phy.OwnerConnected || phy.Effective != nil ||
		phy.Requested.FrequencyHz != cfg.Radio.FreqHz ||
		phy.Error != "observer radio disconnected; current PHY is unverified" ||
		!strings.Contains(raw, `"effective":null`) {
		t.Fatalf("disconnected PHY was reported as verified: %s", raw)
	}
	cfg.RequireParity = false
	connected = true
	phy, raw = read()
	if phy.Valid || phy.OwnerConnected || phy.Effective != nil ||
		phy.Error != "queued PHY readback is unavailable in legacy mode" {
		t.Fatalf("legacy PHY was reported as queued-verified: %s", raw)
	}
}

func TestModemAuthorityStatusReportsVerifiedReadOnlyConfiguration(t *testing.T) {
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"repeater"}
	cfg.PHYAuthority = "modem"
	capacity := 4
	cfg.RadioClientCapacity = &capacity
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	online := cfg.sharedPHYStatus(configuredPHY(cfg, true))
	if online.ConfigurationOwner != "modem" || online.PHYAuthority != "modem" ||
		!online.OwnerConnected || !online.Valid || online.Effective == nil ||
		online.Effective.FrequencyHz != cfg.Radio.FreqHz {
		t.Fatalf("verified modem-owned PHY not reported: %+v", online)
	}
	cfg.radioCapacity = &capacityEvidence{external: 3, local: 3, source: "reported"}
	online = cfg.sharedPHYStatus(configuredPHY(cfg, true))
	if online.ExternalClientCapacity != 3 || online.LocalSourceCapacity != 3 || online.CapacitySource != "reported" {
		t.Fatalf("reported physical capacity omitted: %+v", online)
	}
	offline := cfg.sharedPHYStatus(configuredPHY(cfg, false))
	if offline.OwnerConnected || offline.Valid || offline.Effective != nil ||
		offline.ConfigurationOwner != "modem" || offline.Error != "controller radio disconnected; current PHY is unverified" {
		t.Fatalf("disconnected verify-only link claimed an effective PHY: %+v", offline)
	}
}

func TestSelectiveReadinessRequiresControllerButNotDisabledRoles(t *testing.T) {
	started := time.Now().UTC()
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"repeater"}
	online := true
	owner := cfg.ownerStatus(started, fakeOwner{phy: configuredPHY(cfg, online), airtime: []uint32{0, 7}})
	repeater := roleStatus{
		ApplicationStartedAt: &started, State: "running", RadioConnected: &online, Telemetry: &radio.Telemetry{},
	}
	status := map[string]roleStatus{
		"controller": owner, "repeater": repeater,
		"room": {State: "disabled"}, "companion": {State: "disabled"},
		"observer": {State: "disabled"}, "bot": {State: "disabled"},
	}
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus { return status }, slog.Default(), cfg)
	if got := requestReadiness(t, handler, http.StatusOK, "", ""); got.MQTTConnectionChecked || len(got.NotReady) != 0 {
		t.Fatalf("disabled publisher affected readiness: %+v", got)
	}
	snapshot := requestStatus(t, handler)
	if snapshot["observer"].State != "disabled" || snapshot["observer"].PublicKey != "" ||
		snapshot["controller"].PublicKey != "" || snapshot["controller"].MQTTConnected != nil ||
		snapshot["controller"].SharedPHY == nil || !snapshot["controller"].SharedPHY.Valid ||
		snapshot["controller"].SharedPHY.ConfigurationOwner != "controller" ||
		len(snapshot["controller"].AirtimeMS) != 2 {
		t.Fatalf("controller impersonated the MQTT observer or lost PHY/airtime: %+v", snapshot)
	}
	offline := cfg.ownerStatus(started, fakeOwner{
		phy: configuredPHY(cfg, false), airtime: []uint32{0, 7}, telemetry: errors.New("radio offline"),
	})
	status["controller"] = offline
	requestReadiness(t, handler, http.StatusServiceUnavailable, "controller", "radio_disconnected")
	if offline.SharedPHY.Valid || offline.SharedPHY.Effective != nil || offline.SharedPHY.OwnerConnected ||
		offline.SharedPHY.Error != "controller radio disconnected; current PHY is unverified" {
		t.Fatalf("offline controller claimed an effective physical profile: %+v", offline.SharedPHY)
	}
	delete(status, "controller")
	requestReadiness(t, handler, http.StatusServiceUnavailable, "controller", "not_started")
	cfg.EnabledRoles = []string{}
	cfg.BotCompanionListen = "127.0.0.1:5001"
	status["controller"] = owner
	requestReadiness(t, newStatusHandler(context.Background(), func() map[string]roleStatus { return status }, slog.Default(), cfg),
		http.StatusServiceUnavailable, "bot_companion", "not_started")
	status["bot_companion"] = repeater
	requestReadiness(t, newStatusHandler(context.Background(), func() map[string]roleStatus { return status }, slog.Default(), cfg),
		http.StatusOK, "", "")
}

func TestCompanionRetentionStatusTracksActiveApplication(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	cfg.CompanionRetention = "native_queue"
	worker := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
		return &lifecycleLink{}, nil
	})
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus {
		return map[string]roleStatus{"companion": worker.status()}
	}, slog.Default(), cfg)
	entry := requestStatus(t, handler)["companion"]
	if entry.CompanionRetention == nil || !entry.CompanionRetention.Valid ||
		entry.CompanionRetention.Requested != "native_queue" ||
		entry.CompanionRetention.Effective == nil ||
		*entry.CompanionRetention.Effective != "native_queue" ||
		entry.CompanionRetention.Error != "" {
		t.Fatalf("active retention not reported: %+v", entry.CompanionRetention)
	}
	if err := worker.Close(); err != nil {
		t.Fatal(err)
	}
	entry = requestStatus(t, handler)["companion"]
	if entry.CompanionRetention == nil || entry.CompanionRetention.Valid ||
		entry.CompanionRetention.Effective != nil ||
		entry.CompanionRetention.Requested != "native_queue" ||
		entry.CompanionRetention.Error != "companion application is not active" {
		t.Fatalf("stopped application retained a claimed effective mode: %+v", entry.CompanionRetention)
	}
}

func TestReadinessHTTPStartupListenerAndShutdown(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	status := make(map[string]roleStatus)
	handler := newStatusHandler(ctx, func() map[string]roleStatus { return status }, slog.Default(), DefaultConfig())
	requestReadiness(t, handler, http.StatusServiceUnavailable, "room", "not_started")
	status = onlineHealthSnapshot()
	if got := requestReadiness(t, handler, http.StatusOK, "", ""); !got.MQTTConnectionChecked {
		t.Fatal("readiness did not check MQTT connection evidence")
	}
	observer := status["observer"]
	observer.MQTTConnected = nil
	status["observer"] = observer
	requestReadiness(t, handler, http.StatusServiceUnavailable, "observer", "mqtt_connection_unavailable")
	status = onlineHealthSnapshot()
	socket, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	listener := trackListener(socket)
	t.Cleanup(func() { _ = listener.Close() })
	handler = newStatusHandler(ctx, func() map[string]roleStatus {
		bot := status["bot"]
		active := listener.active.Load()
		bot.ListenerActive = &active
		status["bot"] = bot
		return status
	}, slog.Default(), DefaultConfig())
	requestReadiness(t, handler, http.StatusOK, "", "")
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	requestReadiness(t, handler, http.StatusServiceUnavailable, "bot", "listener_inactive")
	status = onlineHealthSnapshot()
	cancel()
	requestReadiness(t, handler, http.StatusServiceUnavailable, "host", "shutting_down")
}

func TestObserverMQTTOutageAndReconnectChangeHTTPReadiness(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	broker, err := observer.StartBroker(observer.BrokerConfig{Address: "127.0.0.1:0", Logger: logger})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = broker.Close() })
	address := broker.Addr().String()
	id := meshcore.NewLocalIdentityFromSeed([32]byte{91})
	publisher, err := observer.New(id.String(), observer.Config{
		URL: "tcp://" + address, QueueSize: 2, Logger: logger,
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = publisher.Close() })
	status := onlineHealthSnapshot()
	before := status["observer"]
	before.PublicKey = id.String()
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus {
		entry := before
		connected, stats := publisher.Connected(), publisher.Stats()
		entry.MQTTConnected, entry.ObserverStats = &connected, &stats
		status["observer"] = entry
		return status
	}, logger, DefaultConfig())
	wait := func(condition func() bool) {
		t.Helper()
		deadline := time.NewTimer(8 * time.Second)
		defer deadline.Stop()
		tick := time.NewTicker(5 * time.Millisecond)
		defer tick.Stop()
		for !condition() {
			select {
			case <-deadline.C:
				t.Fatalf("MQTT transition timed out: connected=%v stats=%+v", publisher.Connected(), publisher.Stats())
			case <-tick.C:
			}
		}
	}
	requestReadiness(t, handler, http.StatusServiceUnavailable, "observer", "mqtt_disconnected")
	if err := publisher.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	wait(publisher.Connected)
	if got := requestReadiness(t, handler, http.StatusOK, "", ""); !got.MQTTConnectionChecked {
		t.Fatal("live MQTT connection was not checked")
	}
	if err := broker.Close(); err != nil {
		t.Fatal(err)
	}
	// Detect an idle connection loss before admitting another observation.
	wait(func() bool { return !publisher.Connected() })
	if got := requestReadiness(t, handler, http.StatusServiceUnavailable, "observer", "mqtt_disconnected"); !got.MQTTConnectionChecked {
		t.Fatal("disconnected MQTT publisher was reported unchecked")
	}
	entry := requestStatus(t, handler)["observer"]
	if entry.MQTTConnected == nil || *entry.MQTTConnected || entry.ApplicationError != "" ||
		entry.State != "running" || !entry.ApplicationStartedAt.Equal(*before.ApplicationStartedAt) {
		t.Fatalf("temporary broker outage changed the application lifetime or became terminal: %+v", entry)
	}
	publisher.Observe(make([]byte, observer.MaxFrameSize+1), 0, 0, false)
	publisher.Observe([]byte{0x0d, 0, 1, 2, 3, 4}, 1, -95, true)
	replacement, err := observer.StartBroker(observer.BrokerConfig{Address: address, Logger: logger})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = replacement.Close() })
	wait(func() bool { return publisher.Connected() && publisher.Stats().Published == 1 })
	requestReadiness(t, handler, http.StatusOK, "", "")
	entry = requestStatus(t, handler)["observer"]
	if entry.ApplicationError != "" || entry.PublicKey != before.PublicKey ||
		!entry.ApplicationStartedAt.Equal(*before.ApplicationStartedAt) {
		t.Fatalf("broker recovery restarted or faulted the observer: %+v", entry)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/status", nil))
	var wire map[string]struct {
		Stats map[string]uint64 `json:"observer_stats"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &wire); err != nil {
		t.Fatal(err)
	}
	stats := wire["observer"].Stats
	if len(stats) != 4 || stats["observed"] != 2 || stats["published"] != 1 ||
		stats["dropped"] != 1 || stats["queued"] != 0 {
		t.Fatalf("HTTP observer counters did not reflect admission and broker ACKs: %s", response.Body.String())
	}
	if err := publisher.Close(); err != nil {
		t.Fatal(err)
	}
	requestReadiness(t, handler, http.StatusServiceUnavailable, "observer", "mqtt_disconnected")
}

func TestRolePermanentFaultIsVisibleAtHTTPBoundaryButRecoverableErrorsAreNotTerminal(t *testing.T) {
	for _, role := range []string{"room", "repeater"} {
		t.Run(role, func(t *testing.T) {
			cfg, ids := lifecycleConfig(t)
			link := newRoutingLink()
			worker := routingStart(t, context.Background(), cfg, role, func(context.Context) (sourceLink, error) {
				return link, nil
			})
			routingNext(t, link)
			peer := meshcore.NewLocalIdentityFromSeed([32]byte{77})
			routingLogin(t, link, role, peer, ids[role])
			worker.mu.Lock()
			service := worker.current.service
			worker.mu.Unlock()
			siblingRole := "room"
			if role == "room" {
				siblingRole = "repeater"
			}
			siblingLink := newRoutingLink()
			sibling := routingStart(t, context.Background(), cfg, siblingRole, func(context.Context) (sourceLink, error) {
				return siblingLink, nil
			})
			routingNext(t, siblingLink)
			base := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
				return &lifecycleLink{}, nil
			})
			siblingBefore, companionBefore := sibling.status(), base.status()
			otherRoles := onlineHealthSnapshot()
			snapshot := func() map[string]roleStatus {
				out := make(map[string]roleStatus)
				for name, status := range otherRoles {
					out[name] = status
				}
				out[role] = worker.status()
				out[siblingRole] = sibling.status()
				out["companion"] = base.status()
				return out
			}
			handler := newStatusHandler(context.Background(), snapshot, slog.New(slog.NewTextHandler(io.Discard, nil)), cfg)
			requestReadiness(t, handler, http.StatusOK, "", "")
			started := worker.status().ApplicationStartedAt
			// A real authenticated but unsupported RF request reports an ordinary
			// packet error. A following reply proves the application kept working.
			plain := binary.LittleEndian.AppendUint32(nil, 2)
			plain = append(plain, 255)
			payload := append([]byte{ids[role].PublicKey()[0], peer.PublicKey()[0]}, routingEncrypt(t, peer, ids[role], plain)...)
			link.nodeRadio.inject(&meshcore.Packet{Header: meshcore.PayloadTypeReq<<2 | 2, Payload: payload})
			routingCommand(t, link, peer, ids[role], 3, "get name")
			if reply := routingReply(t, link, peer, ids[role]); !strings.HasPrefix(reply, "> Initial ") {
				t.Fatalf("role stopped after recoverable RF error: %q", reply)
			}
			if service.ApplicationError() != nil {
				t.Fatal("recoverable packet error became a terminal application fault")
			}
			requestReadiness(t, handler, http.StatusOK, "", "")
			temporary := errors.New("temporary telemetry sample unavailable")
			link.telemetryError.Store(&temporary)
			requestReadiness(t, handler, http.StatusServiceUnavailable, role, "telemetry_unavailable")
			if status := requestStatus(t, handler)[role]; status.State != "running" || status.ApplicationError != "" {
				t.Fatal("temporary telemetry became a permanent application fault")
			}
			link.telemetryError.Store(nil)
			requestReadiness(t, handler, http.StatusOK, "", "")
			link.offline.Store(true)
			requestReadiness(t, handler, http.StatusServiceUnavailable, role, "radio_disconnected")
			link.offline.Store(false)
			requestReadiness(t, handler, http.StatusOK, "", "")
			if !worker.status().ApplicationStartedAt.Equal(*started) {
				t.Fatal("radio reconnect pretended to restart the application")
			}
			permanent := errors.New("source policy confirmation failed")
			link.nodeRadio.policyError.Store(&permanent)
			routingCommand(t, link, peer, ids[role], 4, "set af 3")
			lifecycleWait(t, func() bool { return service.ApplicationError() != nil })
			if !errors.Is(service.ApplicationError(), roles.ErrCommitUncertain) {
				t.Fatal("terminal API lost fail-closed error classification")
			}
			requestReadiness(t, handler, http.StatusServiceUnavailable, role, "application_fault")
			status := requestStatus(t, handler)
			faulted := status[role]
			if faulted.State != "faulted" || !strings.Contains(faulted.ApplicationError, permanent.Error()) ||
				!*faulted.RadioConnected || !faulted.ApplicationStartedAt.Equal(*started) ||
				faulted.PublicKey != ids[role].String() {
				t.Fatalf("terminal application fault was hidden behind a live radio: %+v", faulted)
			}
			for name, before := range map[string]roleStatus{siblingRole: siblingBefore, "companion": companionBefore} {
				entry := status[name]
				if entry.State != "running" || entry.ApplicationError != "" ||
					entry.PublicKey != before.PublicKey || !entry.ApplicationStartedAt.Equal(*before.ApplicationStartedAt) {
					t.Fatalf("target fault changed sibling %s: %+v", name, entry)
				}
			}
			routingLogin(t, siblingLink, siblingRole, peer, ids[siblingRole])
			routingCommand(t, siblingLink, peer, ids[siblingRole], 2, "get name")
			if reply := routingReply(t, siblingLink, peer, ids[siblingRole]); !strings.HasPrefix(reply, "> Initial ") {
				t.Fatalf("sibling stopped processing after target fault: %q", reply)
			}
			link.nodeRadio.policyError.Store(nil)
			if err := worker.request(context.Background()); !errors.Is(err, roles.ErrCommitUncertain) {
				t.Fatalf("terminal service admitted a hidden retry: %v", err)
			}
			requestReadiness(t, handler, http.StatusServiceUnavailable, role, "application_fault")
		})
	}
}

func TestCompanionPermanentFaultIsVisibleAtHTTPBoundary(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	link := &lifecycleLink{}
	worker := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
		return link, nil
	})
	worker.mu.Lock()
	server := worker.current.server
	worker.mu.Unlock()
	handler := newStatusHandler(context.Background(), func() map[string]roleStatus {
		status := onlineHealthSnapshot()
		status["companion"] = worker.status()
		return status
	}, slog.New(slog.NewTextHandler(io.Discard, nil)), cfg)
	conn := lifecycleDial(t, worker.status().Endpoint)
	response := lifecycleCommand(t, conn, []byte{protocol.CmdSetAdvertName})
	if len(response) != 2 || response[0] != protocol.RespErr || server.ApplicationError() != nil {
		t.Fatal("malformed command became a terminal companion fault")
	}
	requestReadiness(t, handler, http.StatusOK, "", "")
	permanent := errors.New("source policy confirmation failed")
	link.radio.policyError.Store(&permanent)
	command := make([]byte, 9)
	command[0] = protocol.CmdSetTuningParams
	binary.LittleEndian.PutUint32(command[1:], 1200)
	binary.LittleEndian.PutUint32(command[5:], 3000)
	if reply := lifecycleCommand(t, conn, command); len(reply) != 2 || reply[0] != protocol.RespErr {
		t.Fatal("uncertain companion policy application claimed success")
	}
	if server.ApplicationError() == nil {
		t.Fatal("companion did not expose fail-closed cause")
	}
	requestReadiness(t, handler, http.StatusServiceUnavailable, "companion", "application_fault")
	entry := requestStatus(t, handler)["companion"]
	if entry.State != "faulted" || entry.ApplicationError == "" || !*entry.RadioConnected {
		t.Fatalf("companion failure not visible in detailed status: %+v", entry)
	}
	link.radio.policyError.Store(nil)
	if err := worker.request(context.Background(), companionReset); err == nil {
		t.Fatal("terminal companion admitted a reset as hidden recovery")
	}
	requestReadiness(t, handler, http.StatusServiceUnavailable, "companion", "application_fault")
}
