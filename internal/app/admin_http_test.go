package app

import (
	"bufio"
	"context"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
)

func TestHostAdminStalledBodiesNeverReachOwner(t *testing.T) {
	for _, route := range []string{"/admin/command", "/admin/role/repeater"} {
		t.Run(route, func(t *testing.T) {
			cfg := adminTestConfig(t)
			cfg.EnabledRoles = RoleSelection{"bot", "repeater"}
			var admitted atomic.Int32
			owner := hostRoleAdmin{"repeater": func(context.Context, string) (string, error) {
				admitted.Add(1)
				return "> fixture", nil
			}}
			handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg, owner)
			server := httptest.NewUnstartedServer(handler)
			server.Config = newStatusServer(handler)
			server.Start()
			defer server.Close()
			conn, err := net.Dial("tcp", server.Listener.Addr().String())
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			if err := conn.SetDeadline(time.Now().Add(7 * time.Second)); err != nil {
				t.Fatal(err)
			}
			start := time.Now()
			_, err = io.WriteString(conn, "POST "+route+" HTTP/1.1\r\nHost: localhost\r\n"+
				"X-Host-Admin: "+os.Getenv(cfg.AdminHTTPTokenEnv)+"\r\n"+
				"X-Host-Intent: admin-v1\r\nContent-Length: 8\r\nConnection: close\r\n\r\nget ")
			if err != nil {
				t.Fatal(err)
			}
			response, err := http.ReadResponse(bufio.NewReader(conn), nil)
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			body, err := io.ReadAll(response.Body)
			if err != nil {
				t.Fatal(err)
			}
			if response.StatusCode != http.StatusRequestTimeout ||
				!strings.Contains(string(body), "owner command was not submitted") {
				t.Fatalf("stalled body response: %d %q", response.StatusCode, body)
			}
			if elapsed := time.Since(start); elapsed < 4*time.Second || elapsed > 6*time.Second {
				t.Fatalf("body deadline elapsed %s", elapsed)
			}
			if admitted.Load() != 0 {
				t.Fatal("partial command was admitted to role owner")
			}
		})
	}
}

func TestHostStatusServerBoundsNetworkIO(t *testing.T) {
	server := newStatusServer(http.NewServeMux())
	if server.ReadHeaderTimeout != 5*time.Second || server.ReadTimeout != 5*time.Second ||
		server.WriteTimeout != 15*time.Second {
		t.Fatalf("status server does not bound request/response IO: %+v", server)
	}
}

func adminTestConfig(t *testing.T) Config {
	t.Helper()
	t.Setenv("MESHCORE_HOST_ADMIN_HTTP", "")
	t.Setenv("MESHCORE_TEST_HOST_ADMIN", strings.Repeat("a", 64))
	cfg := DefaultConfig()
	cfg.AdminHTTPTokenEnv = "MESHCORE_TEST_HOST_ADMIN"
	cfg.BotRuntime = "native_lua"
	cfg.EnabledRoles = RoleSelection{"bot"}
	// Unix socket paths must fit even when the checkout or TMPDIR is long.
	root, err := os.MkdirTemp("/tmp", "mc-admin-http-")
	if err != nil {
		t.Fatal(err)
	}
	cfg.StateDir = root
	cfg.BotNativeWorker = filepath.Join(cfg.StateDir, "fixture-worker")
	if err := os.MkdirAll(filepath.Join(cfg.StateDir, "bot", "native"), 0700); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(root) })
	return cfg
}

func adminRequest(handler http.Handler, target, body, token, intent, origin string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(http.MethodPost, target, strings.NewReader(body))
	r.Header.Set("X-Host-Admin", token)
	r.Header.Set("X-Host-Intent", intent)
	r.Header.Set("Origin", origin)
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	return w
}

func TestHostAdminAuthenticationAndScope(t *testing.T) {
	cfg := adminTestConfig(t)
	handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg)
	token := os.Getenv(cfg.AdminHTTPTokenEnv)
	for _, tc := range []struct {
		token, intent, origin string
		code                  int
	}{
		{"", "admin-v1", "", 401},
		{"wrong", "admin-v1", "", 401},
		{token, "", "", 403},
		{token, "admin-v1", "https://evil.example", 403},
		{token, "admin-v1", "http://example.com", 200},
	} {
		w := adminRequest(handler, "/admin/status", "", tc.token, tc.intent, tc.origin)
		if w.Code != tc.code || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatalf("status code %d, want %d", w.Code, tc.code)
		}
	}
	for _, command := range []string{"set wifi.pwd secret", "key bot " + strings.Repeat("a", 128),
		"bot https token home secret", "role password bot secret", "radio 912525000 250000 7 5 2",
		"role config repeater", "role config room", "role config companion", "role name repeater", "role name room", "role name companion",
		"data export kv caller bad-principal", "source status\nreboot", strings.Repeat("x", 161)} {
		w := adminRequest(handler, "/admin/command", command, token, "admin-v1", "")
		if w.Code != 400 {
			t.Fatalf("unsupported command admitted: %d", w.Code)
		}
	}
	cfg.AdminHTTPTokenEnv = ""
	disabled := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg)
	w := httptest.NewRecorder()
	disabled.ServeHTTP(w, httptest.NewRequest("GET", "/admin", nil))
	if w.Code != 404 {
		t.Fatal("admin must be disabled by default")
	}
}

func TestHostAdminOwnerPersistenceAndErrors(t *testing.T) {
	cfg := adminTestConfig(t)
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	if err := os.Chmod(socket, 0600); err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for n := 0; n < 3; n++ {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			body := make([]byte, 161)
			length, _ := conn.Read(body)
			switch strings.TrimSpace(string(body[:length])) {
			case "bot name Birch":
				_ = os.WriteFile(filepath.Join(cfg.StateDir, "saved-name"), []byte("Birch"), 0600)
				_, _ = io.WriteString(conn, "Name: Birch\n")
			case "bot name":
				value, _ := os.ReadFile(filepath.Join(cfg.StateDir, "saved-name"))
				_, _ = io.WriteString(conn, "Name: "+string(value)+"\n")
			default:
				_, _ = io.WriteString(conn, "Error: scope unavailable\n")
			}
			_ = conn.Close()
		}
	}()
	handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg)
	token := os.Getenv(cfg.AdminHTTPTokenEnv)
	for _, tc := range []struct {
		command string
		code    int
		reply   string
	}{
		{"bot name Birch", 200, "Name: Birch"},
		{"bot name", 200, "Name: Birch"},
		{"data export kv caller " + strings.Repeat("1", 64), 409, "Error: scope unavailable"},
	} {
		w := adminRequest(handler, "/admin/command", tc.command, token, "admin-v1", "")
		if w.Code != tc.code || strings.TrimSpace(w.Body.String()) != tc.reply {
			t.Fatalf("owner reply %d %q", w.Code, w.Body.String())
		}
	}
	<-done
	saved, err := os.ReadFile(filepath.Join(cfg.StateDir, "saved-name"))
	if err != nil || string(saved) != "Birch" {
		t.Fatal("adapter did not use persistent owner helper")
	}
}

func TestHostAdminNativeChannelPolicyCommands(t *testing.T) {
	for _, command := range []string{
		"bot contacts", "bot membership", "bot membership 7", "bot membership 0 #lab",
		"bot membership 1 public", "bot membership 3 off",
		"bot membership 2 private 4f7073 0102030405060708090a0b0c0d0e0f10",
		"bot access", "bot access dm", "bot access 1 list 0",
		"bot access 2 ping", "bot access 2 ping 12", "bot access dm action_send 0",
		"bot access dm ping inherit", "bot access 7 default 63",
		"bot access native default 16", "bot thread dm notes 16", "bot thread native monitor 48",
		"bot thread 2 notes inherit", "bot thread dm list 0",
		"data export kv conversation-thread " + strings.Repeat("a", 64),
		"source api threads",
	} {
		if !hostAdminCommandAllowed(command) {
			t.Errorf("native channel policy command denied: %q", command)
		}
	}
	for _, command := range []string{
		"bot membership 8 public", "bot membership 0 #Bad", "bot membership 0\nreboot",
		"bot membership 1 private Ops 12", "bot access 8 ping 12",
		"bot access dm ping 64", "bot access dm ping -1", "bot access dm list 65",
		"bot access dm ping 12\nsource remove",
		"bot thread dm notes 63", "bot thread dm notes 17", "bot thread dm a/b 16",
		"bot thread 8 notes 16", "bot thread dm notes 48\nreboot",
	} {
		if hostAdminCommandAllowed(command) {
			t.Errorf("invalid native policy command accepted: %q", command)
		}
	}
}
func TestHostAdminTimeoutIsUnknown(t *testing.T) {
	cfg := adminTestConfig(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	handler := newStatusHandler(ctx, onlineHealthSnapshot, slog.Default(), cfg)
	w := adminRequest(handler, "/admin/command", "bot name Birch", os.Getenv(cfg.AdminHTTPTokenEnv), "admin-v1", "")
	if w.Code != 504 || !strings.Contains(w.Body.String(), "outcome unknown") || strings.Contains(w.Body.String(), cfg.StateDir) {
		t.Fatalf("timeout response %d %q", w.Code, w.Body.String())
	}

	// Missing/incomplete owner responses are uncertain, not a successful write.
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	if err := os.Chmod(socket, 0600); err != nil {
		t.Fatal(err)
	}
	go func() {
		conn, err := listener.Accept()
		if err == nil {
			defer conn.Close()
			_, _ = io.WriteString(conn, "partial")
		}
	}()
	timeout, stop := context.WithTimeout(context.Background(), time.Second)
	defer stop()
	if _, err := hostOwnerCommand(timeout, socket, "bot name Birch"); err == nil {
		t.Fatal("partial reply treated as success")
	}
}

func TestHostAdminTokenConfiguration(t *testing.T) {
	t.Setenv("MESHCORE_HOST_ADMIN_HTTP", "")
	cfg := DefaultConfig()
	cfg.AdminHTTPTokenEnv = "MESHCORE_TEST_ADMIN_TOKEN"
	for _, value := range []string{"", "short", strings.Repeat("a", 32) + "\n", strings.Repeat("a", 16) + "\n" + strings.Repeat("b", 16)} {
		t.Setenv(cfg.AdminHTTPTokenEnv, value)
		if err := cfg.Validate(); err == nil {
			t.Fatal("unusable or missing admin token accepted")
		}

	}
	t.Setenv(cfg.AdminHTTPTokenEnv, strings.Repeat("a", 64))
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	cfg.AdminHTTPTokenEnv = "unsafe environment variable"
	if err := cfg.Validate(); err == nil {
		t.Fatal("invalid environment name accepted")
	}
}

func TestHostAdminOwnerDeadline(t *testing.T) {
	cfg := adminTestConfig(t)
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	if err := os.Chmod(socket, 0600); err != nil {
		t.Fatal(err)
	}
	received := make(chan string, 1)
	done := make(chan struct{})
	go func() {
		defer close(done)
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		text, _ := bufio.NewReader(conn).ReadString('\n')
		received <- text
		// The owner may still execute; leave the reply unresolved until disconnect.
		_, _ = io.Copy(io.Discard, conn)
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	handler := newStatusHandler(ctx, onlineHealthSnapshot, slog.Default(), cfg)
	w := adminRequest(handler, "/admin/command", "bot shared off", os.Getenv(cfg.AdminHTTPTokenEnv), "admin-v1", "")
	if w.Code != 504 || !strings.Contains(w.Body.String(), "outcome unknown") {
		t.Fatalf("deadline reported as known outcome: %d %q", w.Code, w.Body.String())
	}
	if command := <-received; command != "bot shared off\n" {
		t.Fatal("owner received an unexpected operation")
	}
	<-done
}

type hostAdminSocketInfo struct {
	os.FileInfo
	stat syscall.Stat_t
}

func (info hostAdminSocketInfo) Sys() any { return &info.stat }

func TestHostAdminOwnerSocketRestrictions(t *testing.T) {
	cfg := adminTestConfig(t)
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	for _, mode := range []os.FileMode{0666, 0620, 0604} {
		if err := os.Chmod(socket, mode); err != nil {
			t.Fatal(err)
		}
		if _, err := hostOwnerCommand(ctx, socket, "bot name Birch"); err == nil {
			t.Fatalf("non-private socket %o accepted", mode)
		}
	}
	if err := os.Chmod(socket, 0600); err != nil {
		t.Fatal(err)
	}
	info, err := os.Lstat(socket)
	if err != nil || !hostOwnerSocketAllowed(info) {
		t.Fatal("private same-user socket rejected")
	}
	foreign := hostAdminSocketInfo{FileInfo: info, stat: syscall.Stat_t{Uid: uint32(os.Geteuid()) ^ 1}}
	if hostOwnerSocketAllowed(foreign) || hostOwnerSocketAllowed(nil) {
		t.Fatal("foreign-owned or missing socket accepted")
	}
	link := socket + ".link"
	if err := os.Symlink(socket, link); err != nil {
		t.Fatal(err)
	}
	if _, err := hostOwnerCommand(ctx, link, "bot name Birch"); err == nil {
		t.Fatal("symlink socket accepted")
	}
	file := socket + ".file"
	if err := os.WriteFile(file, nil, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := hostOwnerCommand(ctx, file, "bot name Birch"); err == nil {
		t.Fatal("regular file accepted as owner socket")
	}
	if err := listener.(*net.UnixListener).SetDeadline(time.Now().Add(30 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	if conn, err := listener.Accept(); err == nil {
		conn.Close()
		t.Fatal("rejected socket received a connection")
	}
}

func TestHostAdminExistingPasswordSessions(t *testing.T) {
	cfg := adminTestConfig(t)
	cfg.AdminHTTPTokenEnv = ""
	cfg.AdminPasswordEnv = "MESHCORE_TEST_EXISTING_OWNER_PASSWORD"
	password := "fixture-owner-password"
	t.Setenv("MESHCORE_HOST_ADMIN_HTTP", "1")
	t.Setenv(cfg.AdminPasswordEnv, password)
	handler := newStatusHandler(context.Background(), onlineHealthSnapshot, slog.Default(), cfg)
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	if w := adminRequest(handler, "/admin/status", "", password, "admin-v1", ""); w.Code != 401 {
		t.Fatal("role password must not be accepted as a direct bearer credential")
	}
	if w := adminRequest(handler, "/admin/login", password, "", "", ""); w.Code != 403 {
		t.Fatal("login accepted without CSRF intent")
	}
	if w := adminRequest(handler, "/admin/login", password, "", "admin-v1", "https://evil.example"); w.Code != 403 {
		t.Fatal("cross-origin login accepted")
	}
	if w := adminRequest(handler, "/admin/login", "incorrect", "", "admin-v1", ""); w.Code != 401 {
		t.Fatal("incorrect login accepted")
	}
	login := adminRequest(handler, "/admin/login", password, "", "admin-v1", "")
	session := login.Body.String()
	if login.Code != 200 || !regexp.MustCompile(`^[0-9a-f]{64}$`).MatchString(session) || session == password {
		t.Fatal("login did not create an opaque session")
	}
	if w := adminRequest(handler, "/admin/status", "", session, "admin-v1", ""); w.Code != 200 {
		t.Fatal("session monitoring rejected")
	}
	if w := adminRequest(handler, "/admin/logout", "", session, "admin-v1", ""); w.Code != 200 {
		t.Fatal("logout failed")
	}
	if w := adminRequest(handler, "/admin/status", "", session, "admin-v1", ""); w.Code != 401 {
		t.Fatal("revoked session accepted")
	}
	for n := 0; n < 3; n++ {
		if w := adminRequest(handler, "/admin/login", "incorrect", "", "admin-v1", ""); w.Code != 401 {
			t.Fatal("login limit reached before five attempts")
		}
	}
	if w := adminRequest(handler, "/admin/login", password, "", "admin-v1", ""); w.Code != 429 || w.Header().Get("Retry-After") != "60" {
		t.Fatal("login attempts are not bounded")
	}
	t.Setenv(cfg.AdminPasswordEnv, "too-short")
	if err := cfg.Validate(); err == nil {
		t.Fatal("weak role password accepted for HTTP owner login")
	}
}
