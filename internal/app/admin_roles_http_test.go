package app

import (
	"context"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"meshcore.local/meshcore/internal/roles"
)

func TestHostRoleAdminHTTPAuthScopeAndPersistence(t *testing.T) {
	cfg := adminTestConfig(t)
	cfg.EnabledRoles = RoleSelection{"room", "repeater"}
	cfg.BotRuntime, cfg.BotNativeWorker = "kiss_proxy", ""
	path := filepath.Join(cfg.StateDir, "room-name")
	calls := 0
	owner := hostRoleAdmin{"room": func(_ context.Context, command string) (string, error) {
		calls++
		if strings.HasPrefix(command, "set name ") {
			value := strings.TrimPrefix(command, "set name ")
			return "ok", os.WriteFile(path, []byte(value), 0600)
		}
		if command == "get name" {
			value, err := os.ReadFile(path)
			return "> " + string(value), err
		}
		if command == "advert" || command == "advert.zerohop" {
			return "OK - Advert sent", nil
		}
		return "Error, bad value", nil
	}, "companion": func(context.Context, string) (string, error) {
		t.Fatal("companion owner must not be exposed by the role adapter")
		return "", nil
	}}
	handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg, owner)
	token := os.Getenv(cfg.AdminHTTPTokenEnv)
	if w := adminRequest(handler, "/admin/role/room", "set name Birch Room", "", "admin-v1", ""); w.Code != 401 {
		t.Fatal("unauthenticated role write admitted")
	}
	if calls != 0 {
		t.Fatal("unauthenticated request reached owner")
	}
	if w := adminRequest(handler, "/admin/role/room", "set name Birch Room", token, "admin-v1", "https://evil.example"); w.Code != 403 {
		t.Fatal("cross-origin role mutation admitted")
	}
	for _, command := range []string{"set prv.key " + strings.Repeat("a", 128), "password secret", "reboot",
		"set radio 912525000,250000,7,5", "set guest.password secret", "set name A\npassword secret"} {
		w := adminRequest(handler, "/admin/role/room", command, token, "admin-v1", "")
		if w.Code != 400 {
			t.Fatalf("unsupported role command admitted: %d", w.Code)
		}
	}
	for _, scope := range []string{"bot", "companion", "observer", "repeater"} {
		w := adminRequest(handler, "/admin/role/"+scope, "get name", token, "admin-v1", "")
		if w.Code != 404 {
			t.Fatalf("unavailable scope admitted: %s/%d", scope, w.Code)
		}
	}
	if calls != 0 {
		t.Fatal("invalid/scoped requests reached owner")
	}
	for _, command := range []string{"advert", "advert.zerohop"} {
		before := calls
		if w := adminRequest(handler, "/admin/role/room", command, token, "", ""); w.Code != 403 || calls != before {
			t.Fatal("advert needs write confirmation")
		}
		if w := adminRequest(handler, "/admin/role/room", command, token, "admin-v1", "https://evil.example"); w.Code != 403 || calls != before {
			t.Fatal("cross-origin advert admitted")
		}
		if w := adminRequest(handler, "/admin/role/room", command, token, "admin-v1", ""); w.Code != 200 || calls != before+1 {
			t.Fatalf("explicit advert not routed to role: %d %q", w.Code, w.Body.String())
		}
	}
	if w := adminRequest(handler, "/admin/role/room", "set name Birch Room", token, "admin-v1", ""); w.Code != 200 {
		t.Fatalf("role owner write failed: %d", w.Code)
	}
	if w := adminRequest(handler, "/admin/role/room", "get name", token, "admin-v1", ""); w.Code != 200 || w.Body.String() != "> Birch Room" {
		t.Fatal("role owner persistence/readback was bypassed")
	}
	if w := adminRequest(handler, "/admin/role/room", "set advert.interval 99", token, "admin-v1", ""); w.Code != 409 || !strings.Contains(w.Body.String(), "Error,") {
		t.Fatal("native validation errors must not become success")
	}
	status := adminRequest(handler, "/admin/status", "", token, "admin-v1", "")
	if !strings.Contains(status.Body.String(), `"role_owner_commands":["room"]`) || !strings.Contains(status.Body.String(), `"native_owner":false`) {
		t.Fatal("role controls must be capability-gated independently of native bot")
	}
}

func TestHostRoleAdminHTTPUnknownOutcome(t *testing.T) {
	for _, result := range []error{context.DeadlineExceeded, context.Canceled, roles.ErrCommitUncertain, roles.ErrOwnerCommandUnknown} {
		cfg := adminTestConfig(t)
		cfg.EnabledRoles = RoleSelection{"room"}
		handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg,
			hostRoleAdmin{"room": func(ctx context.Context, command string) (string, error) {
				deadline, ok := ctx.Deadline()
				if !ok || time.Until(deadline) > 8*time.Second || command != "set repeat on" {
					t.Fatal("role adapter lacks bounded/explicit operation")
				}
				return "", errors.Join(result, errors.New("private fixture path"))
			}})
		w := adminRequest(handler, "/admin/role/room", "set repeat on", os.Getenv(cfg.AdminHTTPTokenEnv), "admin-v1", "")
		if w.Code != 504 || !strings.Contains(w.Body.String(), "outcome unknown") || strings.Contains(w.Body.String(), "private fixture") {
			t.Fatalf("role outcome misreported: %d %q", w.Code, w.Body.String())
		}
	}
}

func TestHostRoleAdminUsesNativeEngineAndReload(t *testing.T) {
	for _, role := range []string{"room", "repeater"} {
		t.Run(role, func(t *testing.T) {
			cfg := adminTestConfig(t)
			cfg.EnabledRoles = RoleSelection{role}
			cfg.BotRuntime, cfg.BotNativeWorker = "kiss_proxy", ""
			ctx := context.Background()
			start := func() *roleWorker {
				return routingStart(t, ctx, cfg, role, func(context.Context) (sourceLink, error) {
					return newRoutingLink(), nil
				})
			}
			worker := start()
			originalKey := worker.identity.String()
			handler := newStatusHandler(ctx, onlineHealthSnapshot, slog.Default(), cfg, newHostRoleAdmin(map[string]*roleWorker{role: worker}))
			token := os.Getenv(cfg.AdminHTTPTokenEnv)
			for _, command := range []string{"set name Browser Role", "set repeat on", "set path.hash.mode 2", "set advert.interval 60"} {
				w := adminRequest(handler, "/admin/role/"+role, command, token, "admin-v1", "")
				if w.Code != 200 || !strings.HasPrefix(w.Body.String(), "OK") {
					t.Fatalf("native owner %s: %d %q", command, w.Code, w.Body.String())
				}
			}
			if w := adminRequest(handler, "/admin/role/"+role, "set name Invalid:name", token, "admin-v1", ""); w.Code != 409 {
				t.Fatalf("native name validation became success: %d %q", w.Code, w.Body.String())
			}
			if w := adminRequest(handler, "/admin/role/"+role, "get name", token, "admin-v1", ""); w.Code != 200 || w.Body.String() != "> Browser Role" {
				t.Fatalf("invalid name changed readback: %d %q", w.Code, w.Body.String())
			}
			if err := worker.Close(); err != nil {
				t.Fatal(err)
			}
			worker = start()
			if worker.identity.String() != originalKey {
				t.Fatal("owner configuration changed role identity")
			}
			handler = newStatusHandler(ctx, onlineHealthSnapshot, slog.Default(), cfg, newHostRoleAdmin(map[string]*roleWorker{role: worker}))
			for command, want := range map[string]string{"get name": "> Browser Role", "get repeat": "> on", "get path.hash.mode": "> 2", "get advert.interval": "> 60"} {
				w := adminRequest(handler, "/admin/role/"+role, command, token, "admin-v1", "")
				if w.Code != 200 || w.Body.String() != want {
					t.Fatalf("reloaded %s: %d %q", command, w.Code, w.Body.String())
				}
			}
		})
	}
}
