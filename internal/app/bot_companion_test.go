package app

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/policy"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/state"
)

func TestBotCompanionStartsWithoutBaseOrOtherSelectedRoles(t *testing.T) {
	cfg := DefaultConfig()
	cfg.StateDir = t.TempDir()
	cfg.EnabledRoles = []string{}
	cfg.CompanionListen = "disabled-address"
	cfg.BotListen = "disabled-address"
	cfg.MQTT.URL = ""
	cfg.MQTT.BrokerListen = "disabled-address"
	cfg.MQTT.TopicPrefix = ""
	cfg.BotCompanionListen = "127.0.0.1:0"
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	identity, err := state.Identity(cfg.StateDir, "bot_companion")
	if err != nil {
		t.Fatal(err)
	}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	diagnostics := newTXDiagnostics(logger)
	t.Cleanup(func() { _ = diagnostics.Close() })
	worker, err := startCompanionRole(context.Background(), cfg, "bot_companion",
		map[string]meshcore.LocalIdentity{"bot_companion": identity},
		logger, diagnostics, func(context.Context, meshcore.LocalIdentity) (sourceLink, error) {
			return &lifecycleLink{}, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = worker.Close() })
	status := worker.status()
	if status.State != "running" || status.PublicKey != identity.String() ||
		status.ListenerActive == nil || !*status.ListenerActive || status.Endpoint == "" ||
		status.ApplicationKind != "companion_endpoint" {
		t.Fatalf("bot companion did not start independently: %+v", status)
	}
	for _, role := range []string{"companion", "repeater", "room", "observer", "bot"} {
		if _, err := os.Stat(filepath.Join(cfg.StateDir, role)); !os.IsNotExist(err) {
			t.Fatalf("disabled %s state touched: %v", role, err)
		}
	}
}

func TestBotCompanionAndBaseHaveIndependentIdentityStateRadioAndLifecycle(t *testing.T) {
	cfg, identities := lifecycleConfig(t)
	cfg.BotCompanionListen = "127.0.0.1:0"
	cfg.BotCompanionRetention = "native_queue"
	botFactor := float32(4)
	cfg.BotCompanionPolicy = policy.Overrides{AirtimeFactor: &botFactor}
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	botIdentity, err := state.Identity(cfg.StateDir, "bot_companion")
	if err != nil {
		t.Fatal(err)
	}
	identities["bot_companion"] = botIdentity
	for _, role := range []string{"companion", "bot", "room", "repeater", "observer"} {
		if botIdentity.PublicKey() == identities[role].PublicKey() {
			t.Fatalf("bot companion shares %s identity", role)
		}
	}
	baseLink := &lifecycleLink{}
	base := lifecycleStart(t, context.Background(), cfg, identities, func(context.Context) (sourceLink, error) {
		return baseLink, nil
	})
	botLinks := [2]*lifecycleLink{{}, {}}
	var opens atomic.Int32
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	diagnostics := newTXDiagnostics(logger)
	t.Cleanup(func() { _ = diagnostics.Close() })
	bot, err := startCompanionRole(context.Background(), cfg, "bot_companion", identities,
		logger, diagnostics, func(context.Context, meshcore.LocalIdentity) (sourceLink, error) {
			index := int(opens.Add(1)) - 1
			if index >= len(botLinks) {
				return nil, errors.New("unexpected bot companion reconnect")
			}
			return botLinks[index], nil
		})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = bot.Close() })
	room := routingStart(t, context.Background(), cfg, "room", func(context.Context) (sourceLink, error) {
		return newRoutingLink(), nil
	})
	repeater := routingStart(t, context.Background(), cfg, "repeater", func(context.Context) (sourceLink, error) {
		return newRoutingLink(), nil
	})
	status := func() map[string]roleStatus {
		snapshot := onlineHealthSnapshot()
		snapshot["companion"] = base.status()
		snapshot["bot_companion"] = bot.status()
		snapshot["room"] = room.status()
		snapshot["repeater"] = repeater.status()
		return snapshot
	}
	handler := newStatusHandler(context.Background(), status, logger, cfg)
	missing := onlineHealthSnapshot()
	requestReadiness(t, newStatusHandler(context.Background(), func() map[string]roleStatus {
		return missing
	}, logger, cfg), 503, "bot_companion", "not_started")
	requestReadiness(t, handler, 200, "", "")
	baseBefore, botBefore, roomBefore, repeaterBefore := base.status(), bot.status(), room.status(), repeater.status()
	if baseBefore.Endpoint == botBefore.Endpoint || baseBefore.PublicKey == botBefore.PublicKey ||
		botBefore.PublicKey == identities["bot"].String() ||
		botBefore.CompanionRetention == nil || !botBefore.CompanionRetention.Valid ||
		*botBefore.CompanionRetention.Effective != "native_queue" ||
		baseBefore.CompanionRetention == nil || *baseBefore.CompanionRetention.Effective != "durable_replay" ||
		baseLink.radio.factor != 7 || botLinks[0].radio.factor != 4 {
		t.Fatalf("companion services shared configuration or identity: base=%+v bot=%+v", baseBefore, botBefore)
	}
	baseConn := lifecycleDial(t, baseBefore.Endpoint)
	botConn := lifecycleDial(t, botBefore.Endpoint)
	for _, tc := range []struct {
		connRole string
		key      [32]byte
	}{
		{"base", identities["companion"].PublicKey()},
		{"bot", botIdentity.PublicKey()},
	} {
		conn := baseConn
		if tc.connRole == "bot" {
			conn = botConn
		}
		device := lifecycleCommand(t, conn, []byte{protocol.CmdDeviceQuery, 13})
		info, err := protocol.ParseResponse(device)
		if err != nil || info.Code != protocol.RespDeviceInfo {
			t.Fatalf("%s listener returned invalid device info: %x %v", tc.connRole, device, err)
		}
		if parsed := info.Data.(protocol.DeviceInfoResponse); parsed.FirmwareVersion != 13 {
			t.Fatalf("%s listener advertised protocol %d, want 13", tc.connRole, parsed.FirmwareVersion)
		}
		reply := lifecycleCommand(t, conn, append([]byte{protocol.CmdAppStart}, make([]byte, 7)...))
		if len(reply) < 36 || reply[0] != protocol.RespSelfInfo || !bytes.Equal(reply[4:36], tc.key[:]) {
			t.Fatalf("%s listener returned the wrong identity: %x", tc.connRole, reply)
		}
	}
	botLinks[0].txHandler(radio.TXResult{State: radio.TXFailed})
	if got := requestStatus(t, handler); got["bot_companion"].RoleTX.Failed != 1 ||
		got["companion"].RoleTX.Failed != 0 || got["room"].RoleTX.Failed != 0 ||
		got["bot"].RoleTX != nil {
		t.Fatalf("bot companion TX outcome leaked into another role: %+v", got)
	}
	if reply := lifecycleCommand(t, botConn, (protocol.FactoryResetCommand{}).ToBytes()); !bytes.Equal(reply, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}) {
		t.Fatalf("bot companion admitted factory reset: %x", reply)
	}
	if reply := lifecycleCommand(t, botConn, []byte{protocol.CmdExportPrivateKey}); !bytes.Equal(reply, []byte{protocol.RespDisabled}) {
		t.Fatalf("bot companion exported a key: %x", reply)
	}
	for _, kind := range []byte{protocol.StatsTypeCore, protocol.StatsTypeRadio, protocol.StatsTypePackets} {
		reply := lifecycleCommand(t, botConn, []byte{protocol.CmdGetStats, kind})
		if !bytes.Equal(reply, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}) {
			t.Fatalf("unavailable physical stats %d were not reported as unsupported: %x", kind, reply)
		}
	}
	expanded, err := state.ExportIdentity(cfg.StateDir, "bot_companion")
	if err != nil {
		t.Fatal(err)
	}
	if reply := lifecycleCommand(t, botConn, append([]byte{protocol.CmdImportPrivateKey}, expanded...)); !bytes.Equal(reply, []byte{protocol.RespDisabled}) {
		t.Fatalf("bot companion imported a key: %x", reply)
	}
	if _, err := base.serverConfig(baseLink).ImportIdentity(context.Background(), expanded, json.RawMessage(`{}`)); err == nil {
		t.Fatal("base companion import stole the bot companion identity")
	}
	if err := bot.request(context.Background(), companionReset); err == nil {
		t.Fatal("bot companion reset bypassed disabled factory reset")
	}
	botLinks[0].offline.Store(true)
	requestReadiness(t, handler, 503, "bot_companion", "radio_disconnected")
	botLinks[0].offline.Store(false)
	requestReadiness(t, handler, 200, "", "")
	lifecycleSend(t, botConn, (protocol.RebootCommand{}).ToBytes())
	lifecycleDisconnected(t, botConn)
	lifecycleWait(t, func() bool {
		next := bot.status()
		return next.State == "running" && next.ApplicationStartedAt != nil &&
			!next.ApplicationStartedAt.Equal(*botBefore.ApplicationStartedAt)
	})
	if opens.Load() != 2 || !botLinks[0].retired.Load() || botLinks[1].retired.Load() ||
		baseLink.retired.Load() || base.status().PublicKey != baseBefore.PublicKey ||
		!base.status().ApplicationStartedAt.Equal(*baseBefore.ApplicationStartedAt) ||
		!room.status().ApplicationStartedAt.Equal(*roomBefore.ApplicationStartedAt) ||
		!repeater.status().ApplicationStartedAt.Equal(*repeaterBefore.ApplicationStartedAt) {
		t.Fatal("bot companion reboot replaced a sibling or reused its retired radio source")
	}
	if reply := lifecycleCommand(t, baseConn, []byte{protocol.CmdGetDeviceTime}); len(reply) != 5 || reply[0] != protocol.RespCurrTime {
		t.Fatalf("base companion stopped serving after bot restart: %x", reply)
	}
	requestReadiness(t, handler, 200, "", "")
	document, err := state.ReadRoleJSON(filepath.Join(cfg.StateDir, "bot_companion", "companion.json"))
	if err != nil {
		t.Fatal(err)
	}
	var saved struct {
		PublicKey [32]byte
		Retention string
	}
	if err := json.Unmarshal(document, &saved); err != nil {
		t.Fatal(err)
	}
	if saved.PublicKey != botIdentity.PublicKey() || saved.Retention != "native-queue" {
		t.Fatalf("bot companion state was not retained independently: %+v", saved)
	}
}

func TestBotCompanionOptInImportCommitsOnlyItsIdentityAndState(t *testing.T) {
	cfg, identities := lifecycleConfig(t)
	cfg.BotCompanionListen = "127.0.0.1:0"
	cfg.BotCompanionKeyImport = true
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	original, err := state.Identity(cfg.StateDir, "bot_companion")
	if err != nil {
		t.Fatal(err)
	}
	identities["bot_companion"] = original
	baseLink := &lifecycleLink{}
	base := lifecycleStart(t, context.Background(), cfg, identities, func(context.Context) (sourceLink, error) {
		return baseLink, nil
	})
	links := [2]*lifecycleLink{{}, {}}
	var opens atomic.Int32
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	diagnostics := newTXDiagnostics(logger)
	t.Cleanup(func() { _ = diagnostics.Close() })
	bot, err := startCompanionRole(context.Background(), cfg, "bot_companion", identities,
		logger, diagnostics, func(context.Context, meshcore.LocalIdentity) (sourceLink, error) {
			index := int(opens.Add(1)) - 1
			if index >= len(links) {
				return nil, errors.New("unexpected bot companion reconnect")
			}
			return links[index], nil
		})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = bot.Close() })
	baseBefore, botBefore := base.status(), bot.status()
	botConn := lifecycleDial(t, botBefore.Endpoint)
	baseConn := lifecycleDial(t, baseBefore.Endpoint)
	rejected := func(key []byte) {
		t.Helper()
		reply := lifecycleCommand(t, botConn, append([]byte{protocol.CmdImportPrivateKey}, key...))
		if !bytes.Equal(reply, []byte{protocol.RespErr, protocol.ErrCodeFileIoError}) ||
			bot.status().PublicKey != original.String() {
			t.Fatalf("sibling key replaced the bot companion identity: %x", reply)
		}
	}
	for _, role := range []string{"companion", "bot"} {
		key, err := state.ExportIdentity(cfg.StateDir, role)
		if err != nil {
			t.Fatal(err)
		}
		rejected(key)
	}
	if err := state.WriteRoleJSON(filepath.Join(cfg.StateDir, "room", "state.json"), map[string]string{"name": "room"}); err != nil {
		t.Fatal(err)
	}
	pending, err := state.Identity(cfg.StateDir, "pending_candidate")
	if err != nil {
		t.Fatal(err)
	}
	pendingKey, err := state.ExportIdentity(cfg.StateDir, "pending_candidate")
	if err != nil {
		t.Fatal(err)
	}
	if err := state.StageRoleIdentity(context.Background(), cfg.StateDir, "room", pendingKey); err != nil {
		t.Fatal(err)
	}
	rejected(pendingKey)
	if pending.PublicKey() == original.PublicKey() {
		t.Fatal("pending role was not independent")
	}
	location := make([]byte, 9)
	location[0] = protocol.CmdSetAdvertLatLon
	binary.LittleEndian.PutUint32(location[1:], 12345)
	if reply := lifecycleCommand(t, botConn, location); !bytes.Equal(reply, []byte{protocol.RespOk}) {
		t.Fatalf("bot companion state setup failed: %x", reply)
	}
	imported, err := state.Identity(cfg.StateDir, "import_candidate")
	if err != nil {
		t.Fatal(err)
	}
	key, err := state.ExportIdentity(cfg.StateDir, "import_candidate")
	if err != nil {
		t.Fatal(err)
	}
	if reply := lifecycleCommand(t, botConn, append([]byte{protocol.CmdImportPrivateKey}, key...)); !bytes.Equal(reply, []byte{protocol.RespOk}) {
		t.Fatalf("bot companion key import failed: %x", reply)
	}
	if bot.status().PublicKey != imported.String() || base.status().PublicKey != baseBefore.PublicKey {
		t.Fatal("in-place bot import changed another role or left stale status")
	}
	saved := func() (public [32]byte, latitude int32) {
		t.Helper()
		document, err := state.ReadRoleJSON(filepath.Join(cfg.StateDir, "bot_companion", "companion.json"))
		if err != nil {
			t.Fatal(err)
		}
		var active struct {
			PublicKey [32]byte
			Latitude  int32
		}
		if err := json.Unmarshal(document, &active); err != nil {
			t.Fatal(err)
		}
		return active.PublicKey, active.Latitude
	}
	if public, latitude := saved(); public != imported.PublicKey() || latitude != 12345 {
		t.Fatal("bot identity and application document were not committed together")
	}
	if active, err := state.Identity(cfg.StateDir, "bot_companion"); err != nil || active.PublicKey() != imported.PublicKey() {
		t.Fatalf("durable bot authority differs from application: %v", err)
	}
	if reply := lifecycleCommand(t, botConn, []byte{protocol.CmdExportPrivateKey}); !bytes.Equal(reply, []byte{protocol.RespDisabled}) {
		t.Fatalf("opt-in import enabled private-key export: %x", reply)
	}
	if reply := lifecycleCommand(t, botConn, (protocol.FactoryResetCommand{}).ToBytes()); !bytes.Equal(reply, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}) {
		t.Fatalf("opt-in import enabled factory reset: %x", reply)
	}
	reply := lifecycleCommand(t, botConn, append([]byte{protocol.CmdAppStart}, make([]byte, 7)...))
	public := imported.PublicKey()
	if len(reply) < 36 || reply[0] != protocol.RespSelfInfo || !bytes.Equal(reply[4:36], public[:]) {
		t.Fatalf("same bot session did not adopt imported identity: %x", reply)
	}
	lifecycleSend(t, botConn, (protocol.RebootCommand{}).ToBytes())
	lifecycleDisconnected(t, botConn)
	lifecycleWait(t, func() bool {
		next := bot.status()
		return next.State == "running" && next.ApplicationStartedAt != nil &&
			!next.ApplicationStartedAt.Equal(*botBefore.ApplicationStartedAt)
	})
	if opens.Load() != 2 || !links[0].retired.Load() || links[1].retired.Load() || baseLink.retired.Load() ||
		bot.status().PublicKey != imported.String() ||
		base.status().PublicKey != baseBefore.PublicKey ||
		!base.status().ApplicationStartedAt.Equal(*baseBefore.ApplicationStartedAt) {
		t.Fatal("bot import/restart replaced the base, or lost its persisted identity")
	}
	if public, latitude := saved(); public != imported.PublicKey() || latitude != 12345 {
		t.Fatal("bot restart lost its imported identity or saved application state")
	}
	if id, err := state.Identity(cfg.StateDir, "bot"); err != nil || id.PublicKey() != identities["bot"].PublicKey() {
		t.Fatalf("KISS bot identity changed: %v", err)
	}
	if reply := lifecycleCommand(t, baseConn, []byte{protocol.CmdGetDeviceTime}); len(reply) != 5 || reply[0] != protocol.RespCurrTime {
		t.Fatalf("base stopped serving after bot import: %x", reply)
	}
}

func TestBotCompanionDisabledDoesNotBecomeAReadinessDependency(t *testing.T) {
	if DefaultConfig().BotCompanionListen != "" {
		t.Fatal("bot companion unexpectedly enabled by default")
	}
	result := applicationReadiness(context.Background(), onlineHealthSnapshot(), DefaultConfig())
	if !result.Ready || result.NotReady["bot_companion"] != "" {
		t.Fatalf("disabled role changed base readiness: %+v", result)
	}
}
