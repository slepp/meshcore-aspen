package app

import (
	"context"
	"encoding/json"
	"log/slog"
	"net"
	"net/http"
	"sync/atomic"
	"time"
)

func newStatusServer(handler http.Handler) *http.Server {
	return &http.Server{
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       5 * time.Second,
		WriteTimeout:      15 * time.Second,
	}
}

// Accept failure ends both application servers, before their session cleanup
// necessarily finishes. Do not report the listener active during that drain.
type trackedListener struct {
	net.Listener
	active atomic.Bool
}

func trackListener(listener net.Listener) *trackedListener {
	l := &trackedListener{Listener: listener}
	l.active.Store(true)
	return l
}

func (l *trackedListener) Accept() (net.Conn, error) {
	conn, err := l.Listener.Accept()
	if err != nil {
		l.active.Store(false)
	}
	return conn, err
}

func (l *trackedListener) Close() error {
	l.active.Store(false)
	return l.Listener.Close()
}

type readiness struct {
	Ready                 bool              `json:"ready"`
	NotReady              map[string]string `json:"not_ready"`
	MQTTConnectionChecked bool              `json:"mqtt_connection_checked"`
}

func applicationReadiness(ctx context.Context, status map[string]roleStatus, cfg Config) readiness {
	result := readiness{NotReady: make(map[string]string)}
	if ctx.Err() != nil {
		result.NotReady["host"] = "shutting_down"
	}
	owner := "observer"
	if !cfg.roleEnabled("observer") {
		owner = "controller"
	}
	roles := []string{owner}
	for _, role := range []string{"companion", "room", "repeater"} {
		if cfg.roleEnabled(role) {
			roles = append(roles, role)
		}
	}
	if cfg.BotCompanionListen != "" {
		roles = append(roles, "bot_companion")
	}
	for _, role := range roles {
		entry, found := status[role]
		switch {
		case !found:
			result.NotReady[role] = "not_started"
		case entry.ApplicationError != "" || entry.State == "faulted":
			result.NotReady[role] = "application_fault"
		case entry.State != "running" || entry.ApplicationStartedAt == nil:
			result.NotReady[role] = "application_not_running"
		case entry.RadioConnected == nil || !*entry.RadioConnected:
			result.NotReady[role] = "radio_disconnected"
		case entry.TelemetryError != "" || entry.Telemetry == nil:
			result.NotReady[role] = "telemetry_unavailable"
		}
	}
	if cfg.roleEnabled("observer") {
		connected := status["observer"].MQTTConnected
		result.MQTTConnectionChecked = connected != nil
		if result.NotReady["observer"] == "" {
			switch {
			case connected == nil:
				result.NotReady["observer"] = "mqtt_connection_unavailable"
			case !*connected:
				result.NotReady["observer"] = "mqtt_disconnected"
			}
		}
	}
	if cfg.roleEnabled("bot") {
		bot := status["bot"]
		if cfg.BotRuntime == "native_lua" {
			switch {
			case bot.ApplicationKind != "native_lua" || bot.State != "running" || bot.ApplicationStartedAt == nil:
				result.NotReady["bot"] = "application_not_running"
			case bot.ApplicationError != "":
				result.NotReady["bot"] = "application_fault"
			case bot.RadioConnected == nil || !*bot.RadioConnected || bot.PHYGeneration == 0:
				result.NotReady["bot"] = "radio_disconnected"
			}
		} else if bot.ListenerActive == nil || !*bot.ListenerActive || bot.State != "running" || bot.ApplicationStartedAt == nil {
			result.NotReady["bot"] = "listener_inactive"
		}
	}
	result.Ready = len(result.NotReady) == 0
	return result
}

func newStatusHandler(ctx context.Context, snapshot func() map[string]roleStatus, logger *slog.Logger, cfg Config, roleProviders ...hostRoleAdmin) *http.ServeMux {
	mux := http.NewServeMux()
	write := func(w http.ResponseWriter, code int, value any) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(code)
		if err := json.NewEncoder(w).Encode(value); err != nil {
			logger.Error("writing host health response", "error", err)
		}
	}
	mux.HandleFunc("GET /status", func(w http.ResponseWriter, _ *http.Request) {
		write(w, http.StatusOK, snapshot())
	})
	mux.HandleFunc("GET /readyz", func(w http.ResponseWriter, _ *http.Request) {
		result := applicationReadiness(ctx, snapshot(), cfg)
		code := http.StatusServiceUnavailable
		if result.Ready {
			code = http.StatusOK
		}
		write(w, code, result)
	})
	var roleAdmin hostRoleAdmin
	if len(roleProviders) > 0 {
		roleAdmin = roleProviders[0]
	}
	registerHostAdmin(mux, ctx, snapshot, cfg, roleAdmin)
	return mux
}
