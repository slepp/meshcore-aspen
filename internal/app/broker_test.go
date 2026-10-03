package app

import (
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestBrokerOnlyLeavesRadioAndRoleStateAlone(t *testing.T) {
	cfg := DefaultConfig()
	cfg.StateDir = filepath.Join(t.TempDir(), "roles")
	cfg.RadioAddress = "127.0.0.1:1"
	cfg.MQTT.BrokerListen = "127.0.0.1:0"
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Millisecond)
	defer cancel()
	if err := RunBroker(ctx, cfg, slog.New(slog.NewTextHandler(io.Discard, nil))); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(cfg.StateDir); !os.IsNotExist(err) {
		t.Fatalf("broker-only mode touched role state: %v", err)
	}
}

func TestBrokerOnlyRequiresEnabledBroker(t *testing.T) {
	cfg := DefaultConfig()
	cfg.MQTT.BrokerListen = ""
	if err := RunBroker(context.Background(), cfg, slog.New(slog.NewTextHandler(io.Discard, nil))); err == nil {
		t.Fatal("disabled broker reported success")
	}
}
