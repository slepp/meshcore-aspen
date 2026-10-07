package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"meshcore.local/meshcore/internal/observer"
)

func main() {
	path := flag.String("config", "observer-reflector.json", "private reflector configuration JSON")
	check := flag.Bool("check", false, "validate configuration without connecting or publishing")
	flag.Parse()
	logger := slog.New(slog.NewJSONHandler(os.Stderr, nil))
	cfg, err := observer.LoadReflectorConfig(*path)
	if err == nil && *check {
		logger.Info("observer reflector configuration valid")
		return
	}
	if err == nil {
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		err = observer.RunReflector(ctx, cfg, logger)
	}
	if err != nil {
		logger.Error("observer reflector stopped", "error", err)
		os.Exit(1)
	}
}
