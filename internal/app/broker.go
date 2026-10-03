package app

import (
	"context"
	"errors"
	"log/slog"

	"meshcore.local/meshcore/internal/observer"
)

// RunBroker serves MQTT without opening a radio or starting host-side roles.
func RunBroker(ctx context.Context, cfg Config, logger *slog.Logger) error {
	if cfg.MQTT.BrokerListen == "" {
		return errors.New("mqtt.broker_listen is required for broker-only mode")
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
