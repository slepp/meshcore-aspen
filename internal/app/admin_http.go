package app

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"meshcore.local/meshcore/internal/roles"
)

//go:embed admin_page.html
var hostAdminPage string

var hostAdminToken = regexp.MustCompile(`^[\x21-\x7e]{32,256}$`)
var hostAdminPassword = regexp.MustCompile(`^[\x21-\x7e]{12,256}$`)
var hostAdminRead = regexp.MustCompile(`^(?:help(?: (?:bot|source|data|role|stats))?|status|ver|board|job|stats(?: (?:sensors|memory|observer|companion))?|bot (?:help|status|name|key|stats|log|diagnostics|admission|policy|mesh|limits|contention|shared|home|reminders|events|channel-wait|membership(?: [0-7])?|(?:access|thread)(?: (?:dm|native|[0-7])(?: (?:default|[a-z][a-z0-9_-]{0,23}|list (?:[0-5]?[0-9]|6[0-4])))?)?|discovery(?: status)?|forward(?: from|to)?|destination(?: [1-4])?|https(?: status)?)|role (?:help|config (?:bot|repeater|room|companion)|name (?:bot|repeater|room|companion))|source (?:help|status|hash|metadata|helptext|api(?: (?:runtimes|package|modules|fetch|threads))?|read [0-9]+)|data (?:help|status|read [0-9a-f]{16} [0-9]+))$`)
var hostAdminWrite = regexp.MustCompile(`^(?:bot (?:name [\x20-\x7e]{1,31}|(?:shared|home|reminders|channel-wait|discovery) (?:on|off)|membership [0-7] (?:off|public|#[a-z0-9_-]{1,31}|private [0-9a-f]{2,64} [0-9a-f]{32})|access (?:dm|native|[0-7]) (?:default|[a-z][a-z0-9_-]{0,23}) (?:[0-5]?[0-9]|6[0-3]|inherit)|thread (?:dm|native|[0-7]) [a-z][a-z0-9_-]{0,23} (?:0|16|32|48|inherit)|cancel)|source (?:begin [0-9a-f]{16} [0-9]{1,4} [0-9a-f]{64}|chunk [0-9a-f]{16} [0-9]+ [0-9a-f]{2,96}|commit [0-9a-f]{16}|cancel|rollback|remove|retry|help [\x20-\x7e]{1,96})|data (?:export (?:kv|timers|reminders) (?:caller|conversation|bot|channel)(?:-thread)? [0-9a-f]{64}|begin [0-9a-f]{16} [0-9a-f]{64}|chunk [0-9a-f]{16} [0-9]+ [0-9a-f]{2,96}|stage [0-9a-f]{16}|restore [0-9a-f]{16}(?: no-rearm)?|clear))$`)

func hostAdminCommandAllowed(command string) bool {
	if strings.HasPrefix(command, "role config ") || strings.HasPrefix(command, "role name ") {
		return command == "role config bot" || command == "role name bot"
	}
	return len(command) <= 160 && (hostAdminRead.MatchString(command) || hostAdminWrite.MatchString(command))
}

func hostOwnerCommand(ctx context.Context, socket, command string) (string, error) {
	info, err := os.Lstat(socket)
	if err != nil || !hostOwnerSocketAllowed(info) {
		return "", errors.New("owner socket must be private and owned by the host service user")
	}
	conn, err := (&net.Dialer{}).DialContext(ctx, "unix", socket)
	if err != nil {
		return "", err
	}
	defer conn.Close()
	peer, ok := conn.(*net.UnixConn)
	if !ok {
		return "", errors.New("owner connection is not a Unix socket")
	}
	raw, err := peer.SyscallConn()
	if err != nil {
		return "", err
	}
	var credential *syscall.Ucred
	var credentialErr error
	err = raw.Control(func(fd uintptr) {
		credential, credentialErr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	})
	if err != nil || credentialErr != nil || credential == nil || credential.Uid != uint32(os.Geteuid()) {
		return "", errors.New("owner socket server does not belong to the host service user")
	}
	deadline, _ := ctx.Deadline()
	if err := conn.SetDeadline(deadline); err != nil {
		return "", err
	}
	if _, err := io.WriteString(conn, command+"\n"); err != nil {
		return "", err
	}
	reply, err := bufio.NewReader(io.LimitReader(conn, 258)).ReadString('\n')
	if err != nil || !strings.HasSuffix(reply, "\n") {
		return "", errors.New("owner reply incomplete; outcome unknown")
	}
	return strings.TrimSuffix(reply, "\n"), nil
}

func hostOwnerSocketAllowed(info os.FileInfo) bool {
	if info == nil || info.Mode()&os.ModeSocket == 0 || info.Mode().Perm()&0077 != 0 {
		return false
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	return ok && stat.Uid == uint32(os.Geteuid())
}

func hostAdminReadCommand(w http.ResponseWriter, r *http.Request) (string, bool) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 160))
	if err == nil {
		return string(body), true
	}
	code := http.StatusBadRequest
	var timeout net.Error
	if errors.As(err, &timeout) && timeout.Timeout() {
		code = http.StatusRequestTimeout
	}
	http.Error(w, "Host command body could not be read; owner command was not submitted", code)
	return "", false
}

func registerHostAdmin(mux *http.ServeMux, ctx context.Context, snapshot func() map[string]roleStatus, cfg Config, roleAdmin hostRoleAdmin) {
	useRoleCredential := cfg.AdminHTTPTokenEnv == "" && os.Getenv("MESHCORE_HOST_ADMIN_HTTP") == "1"
	if cfg.AdminHTTPTokenEnv == "" && !useRoleCredential {
		return
	}
	allowedRoles := make(hostRoleAdmin)
	for _, role := range []string{"room", "repeater"} {
		if cfg.roleEnabled(role) && roleAdmin[role] != nil {
			allowedRoles[role] = roleAdmin[role]
		}
	}
	roleAdmin = allowedRoles
	token := os.Getenv(cfg.AdminHTTPTokenEnv)
	validToken := hostAdminToken.MatchString(token)
	if useRoleCredential {
		token = os.Getenv(cfg.AdminPasswordEnv)
		validToken = hostAdminPassword.MatchString(token)
	}
	expected := sha256.Sum256([]byte(token))
	var sessionMu sync.Mutex
	sessions := make(map[string]time.Time)
	var loginWindow time.Time
	attempts := 0
	expireSessions := func(now time.Time) {
		for id, expiry := range sessions {
			if !now.Before(expiry) {
				delete(sessions, id)
			}
		}
	}
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	headers := func(w http.ResponseWriter) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
	}
	intent := func(w http.ResponseWriter, r *http.Request) bool {
		if r.Header.Get("X-Host-Intent") != "admin-v1" {
			http.Error(w, "Explicit host admin intent required", http.StatusForbidden)
			return false
		}
		if origin := r.Header.Get("Origin"); origin != "" {
			u, err := url.Parse(origin)
			scheme := "http"
			if r.TLS != nil {
				scheme = "https"
			}
			if err != nil || u.Scheme != scheme || u.Host != r.Host || u.Path != "" || u.User != nil {
				http.Error(w, "Same-origin host admin request required", http.StatusForbidden)
				return false
			}
		}
		return true
	}
	auth := func(w http.ResponseWriter, r *http.Request) bool {
		headers(w)
		id := r.Header.Get("X-Host-Admin")
		actual := sha256.Sum256([]byte(id))
		sessionMu.Lock()
		expireSessions(time.Now())
		_, session := sessions[id]
		sessionMu.Unlock()
		direct := !useRoleCredential && validToken && subtle.ConstantTimeCompare(expected[:], actual[:]) == 1
		if !session && !direct {
			http.Error(w, "Host admin authentication required", http.StatusUnauthorized)
			return false
		}
		return intent(w, r)
	}
	mux.HandleFunc("GET /admin", func(w http.ResponseWriter, _ *http.Request) {
		headers(w)
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = io.WriteString(w, hostAdminPage)
	})
	mux.HandleFunc("POST /admin/login", func(w http.ResponseWriter, r *http.Request) {
		headers(w)
		if !intent(w, r) {
			return
		}
		sessionMu.Lock()
		now := time.Now()
		if now.Sub(loginWindow) >= time.Minute {
			loginWindow, attempts = now, 0
		}
		if attempts >= 5 {
			sessionMu.Unlock()
			w.Header().Set("Retry-After", "60")
			http.Error(w, "Host login limit reached; wait one minute", http.StatusTooManyRequests)
			return
		}
		attempts++
		sessionMu.Unlock()
		controller := http.NewResponseController(w)
		if err := controller.SetReadDeadline(now.Add(5 * time.Second)); err != nil && !errors.Is(err, http.ErrNotSupported) {
			http.Error(w, "Host login connection deadline unavailable", http.StatusServiceUnavailable)
			return
		}
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 256))
		_ = controller.SetReadDeadline(time.Time{})
		actual := sha256.Sum256(body)
		clear(body)
		if err != nil || !validToken || subtle.ConstantTimeCompare(expected[:], actual[:]) != 1 {
			http.Error(w, "Host admin authentication required", http.StatusUnauthorized)
			return
		}
		sessionMu.Lock()
		expireSessions(now)
		if len(sessions) >= 4 {
			sessionMu.Unlock()
			http.Error(w, "Host admin sessions full; logout or wait ten minutes", http.StatusTooManyRequests)
			return
		}
		var random [32]byte
		if _, err := rand.Read(random[:]); err != nil {
			sessionMu.Unlock()
			http.Error(w, "Host session creation unavailable", http.StatusServiceUnavailable)
			return
		}
		id := hex.EncodeToString(random[:])
		sessions[id] = now.Add(10 * time.Minute)
		sessionMu.Unlock()
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = io.WriteString(w, id)
	})
	mux.HandleFunc("POST /admin/logout", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		sessionMu.Lock()
		delete(sessions, r.Header.Get("X-Host-Admin"))
		sessionMu.Unlock()
		_, _ = io.WriteString(w, "Logged out")
	})
	mux.HandleFunc("POST /admin/status", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		w.Header().Set("Content-Type", "application/json")
		// Role snapshots contain public operational state, never the host Config.
		_ = json.NewEncoder(w).Encode(map[string]any{"roles": snapshot(), "capabilities": map[string]any{
			"native_owner":        cfg.BotRuntime == "native_lua" && cfg.roleEnabled("bot"),
			"node_backup":         cfg.nodeBackup != nil,
			"role_owner_commands": roleAdmin.names(),
			"configuration":       "native bot settings/source/data persist through owner helpers; host role selection requires config and service restart",
			"unsupported":         []string{"ESP WiFi provisioning", "ESP firmware OTA", "physical ADC", "host config overwrite", "private key/password/token imports", "Wasm installation through this page"},
		}})
	})
	mux.HandleFunc("POST /admin/command", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		command, read := hostAdminReadCommand(w, r)
		if !read {
			return
		}
		if command == "backup" || strings.HasPrefix(command, "backup ") || command == "help backup" {
			if cfg.nodeBackup == nil {
				http.Error(w, "Host node backup service unavailable", http.StatusServiceUnavailable)
				return
			}
			if command == "help backup" {
				command = "backup help"
			}
			reply := cfg.nodeBackup.Command(command, false)
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			if strings.HasPrefix(reply, "Error:") {
				w.WriteHeader(http.StatusConflict)
			}
			_, _ = io.WriteString(w, reply)
			return
		}
		if cfg.BotRuntime != "native_lua" || !cfg.roleEnabled("bot") {
			http.Error(w, "Native bot owner is not enabled", http.StatusConflict)
			return
		}
		if !hostAdminCommandAllowed(command) {
			http.Error(w, "Unsupported host admin command; use the scoped owner CLI for other settings", http.StatusBadRequest)
			return
		}
		timeout, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		reply, err := hostOwnerCommand(timeout, socket, command)
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		if err != nil {
			// Do not return socket paths, environment values or command text.
			http.Error(w, "Host owner connection failed or timed out; outcome unknown. Inspect affected state; do not repeat a write.", http.StatusGatewayTimeout)
			return
		}
		if strings.HasPrefix(reply, "Error:") {
			w.WriteHeader(http.StatusConflict)
		}
		_, _ = io.WriteString(w, reply)
	})
	mux.HandleFunc("GET /admin/backup", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		if cfg.nodeBackup == nil {
			http.Error(w, "Host node backup service unavailable", http.StatusServiceUnavailable)
			return
		}
		id := r.URL.Query().Get("id")
		if len(id) != 16 {
			http.Error(w, "Node backup requires its saved ID16", http.StatusBadRequest)
			return
		}
		file, size, err := cfg.nodeBackup.Open(id)
		if err != nil {
			http.Error(w, err.Error(), http.StatusConflict)
			return
		}
		defer file.Close()
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Disposition", `attachment; filename="meshcore-node-`+id+`.mcb"`)
		w.Header().Set("Content-Length", strconv.Itoa(size))
		_, _ = io.CopyN(w, file, int64(size))
	})
	mux.HandleFunc("POST /admin/role/{role}", func(w http.ResponseWriter, r *http.Request) {
		if !auth(w, r) {
			return
		}
		role := r.PathValue("role")
		owner := roleAdmin[role]
		if (role != "room" && role != "repeater") || owner == nil {
			http.Error(w, "Host role owner commands are unavailable for this role", http.StatusNotFound)
			return
		}
		command, read := hostAdminReadCommand(w, r)
		if !read {
			return
		}
		if !hostRoleRead.MatchString(command) && !hostRoleWrite.MatchString(command) {
			http.Error(w, "Unsupported role command; credentials, identities, shared PHY and restart are unavailable here", http.StatusBadRequest)
			return
		}
		timeout, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		reply, err := owner(timeout, command)
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		if err != nil {
			if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, context.Canceled) || errors.Is(err, roles.ErrCommitUncertain) || errors.Is(err, roles.ErrOwnerCommandUnknown) {
				http.Error(w, "Host role owner outcome unknown; inspect the affected setting before another write", http.StatusGatewayTimeout)
				return
			}
			http.Error(w, "Host role owner command failed; role may be stopped or restarting. Inspect its monitoring state.", http.StatusConflict)
			return
		}
		if hostRoleError.MatchString(reply) {
			w.WriteHeader(http.StatusConflict)
		}
		_, _ = io.WriteString(w, reply)
	})
}
