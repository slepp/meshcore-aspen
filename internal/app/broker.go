package app

import (
	"context"
	"log/slog"

	"meshcore.local/meshcore/internal/observer"
)

// RunBroker serves MQTT without opening a radio or starting host-side roles.
func RunBroker(ctx context.Context, cfg Config, logger *slog.Logger) error {
	if err := cfg.Preflight(true); err != nil {
		return err
	}
	username, err := envSecret(cfg.MQTT.UsernameEnv)
	if err != nil {
		return err
	}
	password, err := envSecret(cfg.MQTT.PasswordEnv)
	if err != nil {
		return err
	}
	broker, err := observer.StartBroker(observer.BrokerConfig{
		Address: cfg.MQTT.BrokerListen, Logger: logger,
		Username: username, Password: password,
	})
	if err != nil {
		return err
	}
	<-ctx.Done()
	return broker.Close()
}
