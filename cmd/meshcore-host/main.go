package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"meshcore.local/meshcore/internal/app"
)

func main() {
	path := flag.String("config", "meshcore-host.json", "host configuration JSON")
	stateDir := flag.String("state-dir", "", "override persistent state directory")
	check := flag.Bool("check", false, "validate configuration without connecting or transmitting")
	brokerOnly := flag.Bool("broker-only", false, "serve the configured MQTT broker without opening a radio or starting host roles")
	flag.Parse()
	logger := slog.New(slog.NewJSONHandler(os.Stderr, nil))
	cfg, err := app.LoadConfig(*path)
	if err != nil {
		logger.Error("configuration", "error", err)
		os.Exit(1)
	}
	if *stateDir != "" {
		cfg.StateDir = *stateDir
	}
	if *check {
		logger.Info("configuration valid")
		return
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	run := app.Run
	if *brokerOnly {
		run = app.RunBroker
	}
	if err := run(ctx, cfg, logger); err != nil {
		logger.Error("host stopped", "error", err)
		os.Exit(1)
	}
}
