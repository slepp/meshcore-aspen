package app

import (
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCompanionRemoteAccessRequiresExplicitConsent(t *testing.T) {
	if DefaultConfig().CompanionListen != "127.0.0.1:5000" {
		t.Fatal("default companion listener is not loopback")
	}
	cfg, err := LoadConfig("../../meshcore-host.json.example")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.CompanionAllowRemote || cfg.CompanionListen != "127.0.0.1:5000" {
		t.Fatal("example exposes unauthenticated companion access")
	}
	for _, role := range []string{"companion", "bot_companion"} {
		for _, address := range []string{"127.0.0.1:5000", "[::1]:5000", "localhost:5000",
			"0.0.0.0:5000", "[::]:5000", ":5000", "192.0.2.1:5000", "radio.example:5000"} {
			t.Run(role+"/"+address, func(t *testing.T) {
				cfg := DefaultConfig()
				cfg.EnabledRoles = []string{}
				if role == "companion" {
					cfg.EnabledRoles = []string{"companion"}
					cfg.CompanionListen = address
				} else {
					cfg.BotCompanionListen = address
				}
				local := address == "127.0.0.1:5000" || address == "[::1]:5000" || address == "localhost:5000"
				if err := cfg.Validate(); (err == nil) != local {
					t.Fatalf("unacknowledged listener: local=%t, error=%v", local, err)
				}
				cfg.CompanionAllowRemote = true
				if err := cfg.Validate(); err != nil {
					t.Fatalf("explicit remote access rejected: %v", err)
				}
			})
		}
	}
	cfg.EnabledRoles = []string{}
	cfg.CompanionListen = "0.0.0.0:5000"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("disabled companion requires remote consent: %v", err)
	}
}

func TestRoomAccessRequiresProvisioningOrExplicitPublicSelection(t *testing.T) {
	cfg := DefaultConfig()
	if cfg.RoomPassword != "" || cfg.RoomPublic {
		t.Fatal("default room has reusable or public access")
	}
	if _, err := cfg.roomAccessPassword(); err == nil {
		t.Fatal("unprovisioned room permitted")
	}
	cfg.StateDir = filepath.Join(t.TempDir(), "uncreated-state")
	err := Run(context.Background(), cfg, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err == nil || !strings.Contains(err.Error(), "room access requires") {
		t.Fatalf("unprovisioned startup error: %v", err)
	}
	if _, err := os.Stat(cfg.StateDir); !os.IsNotExist(err) {
		t.Fatalf("invalid room startup touched state before failing: %v", err)
	}
	cfg.RoomPasswordEnv = "MESHCORE_TEST_ROOM_ACCESS"
	t.Setenv(cfg.RoomPasswordEnv, "")
	if _, err := cfg.roomAccessPassword(); err == nil {
		t.Fatal("empty environment password enabled public room")
	}
	t.Setenv(cfg.RoomPasswordEnv, "operator-selected-test-credential")
	if password, err := cfg.roomAccessPassword(); err != nil || password != "operator-selected-test-credential" {
		t.Fatalf("provisioned room password: %q, %v", password, err)
	}
	cfg.RoomPublic = true
	if _, err := cfg.roomAccessPassword(); err == nil {
		t.Fatal("conflicting public and password configuration accepted")
	}
	cfg.RoomPasswordEnv = ""
	if password, err := cfg.roomAccessPassword(); err != nil || password != "" {
		t.Fatalf("explicit public room: %q, %v", password, err)
	}
	cfg.RoomPublic = false
	cfg.RoomPassword = "deliberately-configured-test-password"
	if _, err := cfg.roomAccessPassword(); err != nil {
		t.Fatal(err)
	}
	cfg.EnabledRoles = []string{}
	cfg.RoomPassword = ""
	if _, err := cfg.roomAccessPassword(); err != nil {
		t.Fatalf("disabled room requires provisioning: %v", err)
	}
}
