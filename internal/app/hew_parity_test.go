package app

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha512"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
	"meshcore.local/meshcore/internal/state"
)

// Same serialized RF inputs and initial native state as the Hew TCP demo.
// The Go application and its real native worker are the reference, not a mock bot.
func TestHewNativeBotSerializedDifferential(t *testing.T) {
	hewNativeBotDifferential(t, os.Getenv("MESHCORE_HEW_TRANSCRIPT"))
}

func TestHewNativeBotScopedDifferential(t *testing.T) {
	hewNativeBotDifferential(t, os.Getenv("MESHCORE_HEW_SCOPE_TRANSCRIPT"))
}

func hewNativeAdminSocket(root string) (string, error) {
	socket := filepath.Join(root, "admin.sock")
	if runtime.GOOS == "linux" && len(socket) >= 108 {
		return "", fmt.Errorf("native reference admin socket path is %d bytes; Linux permits 107 pathname bytes: use a shorter pinned worktree path or a shorter TMPDIR for this Go test (%s)", len(socket), socket)
	}
	return socket, nil
}

func TestHewNativeAdminSocketPathBound(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("Linux sockaddr_un pathname bound")
	}
	for _, test := range []struct {
		root string
		ok   bool
	}{
		{"/" + strings.Repeat("x", 95), true},
		{"/" + strings.Repeat("x", 96), false},
		{"/" + strings.Repeat("é", 48), false},
	} {
		socket, err := hewNativeAdminSocket(test.root)
		if (err == nil) != test.ok {
			t.Fatalf("root %d bytes: socket=%q err=%v", len(test.root), socket, err)
		}
		if err != nil && !strings.Contains(err.Error(), "shorter TMPDIR") {
			t.Fatalf("missing actionable path diagnostic: %v", err)
		}
	}
}

func hewNativeBotDifferential(t *testing.T, path string) {
	executable := os.Getenv("BOT_NATIVE_WORKER")
	if path == "" || executable == "" {
		t.Skip("set MESHCORE_HEW_TRANSCRIPT and BOT_NATIVE_WORKER after make demo")
	}
	var fixture struct {
		Advert        string
		StartupAdvert string `json:"startup_advert"`
		State         map[string]string
		Commands      []struct{ Request, Reply string }
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &fixture); err != nil {
		t.Fatal(err)
	}
	checker := filepath.Join(filepath.Dir(path), "..", "build_worker.py")
	if output, err := exec.Command("python3", "-B", checker, "--verify", "--worker", executable).CombinedOutput(); err != nil {
		t.Fatalf("native worker must match this worktree build: %s (%v)", output, err)
	}
	decode := func(text string) []byte {
		t.Helper()
		value, err := hex.DecodeString(text)
		if err != nil {
			t.Fatal(err)
		}
		return value
	}
	cfg := DefaultConfig()
	cfg.EnabledRoles, cfg.BotRuntime, cfg.BotNativeWorker = []string{"bot"}, "native_lua", executable
	cfg.BotListen = "not-a-listener"
	cfg.StateDir, cfg.StatusListen = nativeBotStateDir(t), freeBotStatusAddress(t)
	nativeRoot := filepath.Join(cfg.StateDir, "bot", "native")
	socket, err := hewNativeAdminSocket(nativeRoot)
	if err != nil {
		t.Fatal(err)
	}
	expanded := sha512.Sum512(bytes.Repeat([]byte{5}, 32))
	expanded[0] &= 248
	expanded[31] = expanded[31]&63 | 64
	bot, err := state.ImportIdentity(cfg.StateDir, "bot", expanded[:])
	if err != nil {
		t.Fatal(err)
	}
	for name, text := range fixture.State {
		clean := filepath.Clean(name)
		if clean != name || filepath.IsAbs(name) || strings.HasPrefix(name, "..") {
			t.Fatal("invalid fixture state path")
		}
		target := filepath.Join(nativeRoot, name)
		if err := os.MkdirAll(filepath.Dir(target), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(target, decode(text), 0600); err != nil {
			t.Fatal(err)
		}
	}
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.completeTX, mast.fastAirtime = true, true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	ctx, cancel := context.WithCancel(context.Background())
	stopped := make(chan error, 1)
	go func() { stopped <- Run(ctx, cfg, slog.New(slog.NewTextHandler(io.Discard, nil))) }()
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-stopped:
			if err != nil {
				t.Error(err)
			}
		case <-time.After(5 * time.Second):
			t.Error("Go native reference did not stop")
		}
	})
	waitAuthority(t, func() bool { _, err := os.Stat(socket); return err == nil })
	waitAuthority(t, func() bool {
		connection, err := net.DialTimeout("unix", socket, time.Second)
		if err != nil {
			return false
		}
		defer connection.Close()
		connection.SetDeadline(time.Now().Add(3 * time.Second))
		fmt.Fprintln(connection, "status")
		status, err := bufio.NewReader(connection).ReadString('\n')
		return err == nil && strings.Contains(status, "ready=1")
	})
	var seed [32]byte
	for i := range seed {
		seed[i] = 2
	}
	peer := meshcore.NewLocalIdentityFromSeed(seed)
	secret, err := peer.SharedSecret(bot.Identity)
	if err != nil {
		t.Fatal(err)
	}
	plaintext := func(raw []byte) []byte {
		t.Helper()
		packet, err := meshcore.PacketFromBytes(raw)
		if err != nil || len(packet.Payload) < 3 {
			t.Fatal("invalid reference response", err)
		}
		text, err := meshcore.MACThenDecrypt(secret, packet.Payload[2:])
		if err != nil || len(text) < 5 {
			t.Fatal("invalid authenticated response", err)
		}
		return bytes.TrimRight(text[4:], "\x00") // only wall-clock timestamp differs
	}
	var home, fallback policy.Scope
	if raw, ok := fixture.State["scopes"]; ok {
		scopes := decode(raw)
		if len(scopes) != 36 || string(scopes[:4]) != "SCP1" {
			t.Fatal("invalid native scope fixture")
		}
		copy(home.Key[:], scopes[4:20])
		copy(fallback.Key[:], scopes[20:36])
	}
	if fixture.StartupAdvert != "" {
		mast.mu.Lock()
		var raw []byte
		for _, packet := range mast.txPackets {
			if packet[0]>>2&15 == meshcore.PayloadTypeAdvert {
				raw = bytes.Clone(packet)
				break
			}
		}
		mast.mu.Unlock()
		got, err := meshcore.PacketFromBytes(raw)
		if err != nil || got.RouteType() != meshcore.RouteTypeTransportFlood ||
			got.TransportCode1 != policy.TransportCode(fallback, got.PayloadType(), got.Payload) ||
			got.TransportCode2 != got.TransportCode1 {
			t.Fatal("default-scoped startup advert mismatch", err)
		}
	}
	injectBotRF(t, mast, decode(fixture.Advert))
	time.Sleep(100 * time.Millisecond)
	for index, test := range fixture.Commands {
		t.Run(fmt.Sprint(index), func(t *testing.T) {
			mast.mu.Lock()
			start := len(mast.txPackets)
			mast.mu.Unlock()
			injectBotRF(t, mast, decode(test.Request))
			var got []byte
			waitAuthority(t, func() bool {
				mast.mu.Lock()
				defer mast.mu.Unlock()
				for _, raw := range mast.txPackets[start:] {
					p, err := meshcore.PacketFromBytes(raw)
					if err == nil && p.PayloadType() == meshcore.PayloadTypeTxtMsg {
						got = bytes.Clone(raw)
						return true
					}
				}
				return false
			})
			want := decode(test.Reply)
			a, _ := meshcore.PacketFromBytes(got)
			b, _ := meshcore.PacketFromBytes(want)
			request, err := meshcore.PacketFromBytes(decode(test.Request))
			if err != nil {
				t.Fatal(err)
			}
			context := policy.ReceiveContext{Unscoped: request.RouteType() == meshcore.RouteTypeFlood}
			if request.RouteType() == meshcore.RouteTypeTransportFlood {
				for _, key := range []policy.Scope{fallback, home} {
					if !key.IsNull() && request.TransportCode1 == policy.TransportCode(key, request.PayloadType(), request.Payload) {
						context.ScopeKnown, context.Scope = true, key
						break
					}
				}
			}
			expected := policy.ChooseReplyScope(context, fallback)
			if !expected.IsNull() {
				for _, p := range []*meshcore.Packet{a, b} {
					code := policy.TransportCode(expected, p.PayloadType(), p.Payload)
					if p.RouteType() != meshcore.RouteTypeTransportFlood || p.TransportCode1 != code || p.TransportCode2 != code {
						t.Fatal("native reply scope differs from actual Go ChooseReplyScope/TransportCode")
					}
				}
			} else if a.RouteType() != meshcore.RouteTypeFlood {
				t.Fatal("unscoped request unexpectedly scoped")
			}
			if a.Header != b.Header || a.PathLength != b.PathLength || !bytes.Equal(a.Path, b.Path) ||
				!bytes.Equal(a.Payload[:2], b.Payload[:2]) || !bytes.Equal(plaintext(got), plaintext(want)) {
				t.Fatalf("native bot mismatch Go=%x Hew=%x (Go text %q; Hew text %q)", got, want, plaintext(got), plaintext(want))
			}
		})
	}
}
