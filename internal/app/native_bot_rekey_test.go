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
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/state"
)

func TestNativeBotCoordinatedIdentityApply(t *testing.T) {
	executable := os.Getenv("BOT_NATIVE_WORKER")
	if executable == "" {
		t.Skip("set BOT_NATIVE_WORKER to the production worker")
	}
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"bot", "companion"}
	cfg.BotRuntime = "native_lua"
	cfg.RadioSession = "required"
	cfg.PHYAuthority = "modem"
	cfg.StateDir = nativeBotStateDir(t)
	cfg.StatusListen, cfg.CompanionListen = freeBotStatusAddress(t), freeBotStatusAddress(t)
	wrapper := filepath.Join(cfg.StateDir, "worker")
	failOnce := filepath.Join(cfg.StateDir, "fail-once")
	failActivate := filepath.Join(cfg.StateDir, "fail-activate")
	// Inject one launch failure without substituting the native implementation.
	script := fmt.Sprintf(`#!/usr/bin/env python3
import os, struct, subprocess, sys
from pathlib import Path
fail_once, fail_activate, worker = Path(%q), Path(%q), %q
if fail_once.exists():
    fail_once.unlink()
    sys.exit(3)
if fail_activate.exists() and "--dormant" in sys.argv:
    child = subprocess.Popen([worker] + sys.argv[1:], stdin=subprocess.PIPE)
    try:
        while True:
            header = sys.stdin.buffer.read(4)
            if not header:
                break
            body = sys.stdin.buffer.read(struct.unpack("<I", header)[0])
            if body[:1] == b"\x06":
                fail_activate.unlink()
                child.kill()
                child.wait()
                sys.exit(3)
            child.stdin.write(header + body)
            child.stdin.flush()
    finally:
        child.stdin.close()
        child.wait()
else:
    os.execv(worker, [worker] + sys.argv[1:])
`, failOnce, failActivate, executable)
	if err := os.WriteFile(wrapper, []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	cfg.BotNativeWorker = wrapper
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.completeTX, mast.fastAirtime, mast.extendedSubports = true, true, true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	status := func() map[string]roleStatus {
		t.Helper()
		response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/status")
		if err != nil {
			return nil
		}
		defer response.Body.Close()
		var result map[string]roleStatus
		_ = json.NewDecoder(response.Body).Decode(&result)
		return result
	}
	start := func() func() {
		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan error, 1)
		go func() { done <- Run(ctx, cfg, logger) }()
		waitAuthority(t, func() bool { return status()["bot"].State == "running" && status()["companion"].State == "running" })
		return func() {
			cancel()
			select {
			case err := <-done:
				if err != nil {
					t.Errorf("host shutdown: %v", err)
				}
			case <-time.After(8 * time.Second):
				t.Error("host did not stop")
			}
		}
	}
	admin := func(command string) string {
		t.Helper()
		conn, err := net.DialTimeout("unix", socket, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		defer conn.Close()
		_ = conn.SetDeadline(time.Now().Add(75 * time.Second))
		if _, err := fmt.Fprintln(conn, command); err != nil {
			t.Fatal(err)
		}
		reply, err := bufio.NewReader(conn).ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		return strings.TrimSpace(reply)
	}
	stop := start()
	defer func() {
		if stop != nil {
			stop()
		}
	}()
	old, err := state.Identity(cfg.StateDir, "bot")
	if err != nil {
		t.Fatal(err)
	}
	oldKey, err := state.ExportIdentity(cfg.StateDir, "bot")
	if err != nil {
		t.Fatal(err)
	}
	if reply := admin("key bot " + hex.EncodeToString(oldKey)); reply != "KEY "+old.String()+"; already active; no apply required" {
		t.Fatal("active identity import did not preserve the current key: " + reply)
	}
	peer, err := state.Identity(cfg.StateDir, "companion")
	if err != nil {
		t.Fatal(err)
	}
	checkOldData := nativeBotDataRoundTrip(t, admin, old.String(), true)
	if reply := admin("discovery on"); !strings.Contains(reply, "Saved and applied") {
		t.Fatal(reply)
	}
	if reply := admin("name Rekey Bot"); !strings.Contains(reply, "Saved and applied") {
		t.Fatal(reply)
	}
	channelBefore, sourceBefore := admin("channel status"), admin("source hash")
	before := status()
	mast.mu.Lock()
	sockets, sets, generation := len(mast.active), mast.sets, mast.nextGeneration
	mast.mu.Unlock()
	next := meshcore.NewLocalIdentityFromSeed([32]byte{93})
	seed := next.Seed()
	digest := sha512.Sum512(seed[:])
	digest[0] &= 248
	digest[31] &= 63
	digest[31] |= 64
	key := digest[:]
	pending := "KEY " + next.String() + " pending; apply required"
	for _, invalid := range []string{"key bot ab", "key bot " + strings.Repeat("00", 64), "key room apply", "identity rotate"} {
		if reply := admin(invalid); !strings.HasPrefix(reply, "Error:") {
			t.Fatalf("invalid request admitted: %s", reply)
		}
	}
	if reply := admin("key bot " + hex.EncodeToString(key)); reply != pending {
		t.Fatal(reply)
	}
	if reply := admin("role key bot pending"); reply != pending {
		t.Fatal(reply)
	}
	if reply := admin("key bot"); reply != "KEY "+old.String() {
		t.Fatal("staging changed live key: " + reply)
	}
	if status()["bot"].PublicKey != old.String() {
		t.Fatal("staging changed dashboard identity")
	}
	if reply := admin("key bot cancel"); !strings.Contains(reply, "pending cancelled") {
		t.Fatal(reply)
	}
	if reply := admin("key bot pending"); !strings.HasPrefix(reply, "Error:") {
		t.Fatal("cancel retained pending: " + reply)
	}
	companionKey, _ := state.ExportIdentity(cfg.StateDir, "companion")
	if reply := admin("key bot " + hex.EncodeToString(companionKey)); !strings.HasPrefix(reply, "Error:") {
		t.Fatal("another role identity admitted")
	}
	if reply := admin("key bot " + hex.EncodeToString(key)); reply != pending {
		t.Fatal(reply)
	}
	// Restart with a pending key must retain the active identity and stage.
	stop()
	stop = nil
	stop = start()
	if status()["bot"].PublicKey != old.String() || admin("key bot pending") != pending {
		t.Fatal("restart applied or lost pending key")
	}
	before = status()
	mast.mu.Lock()
	sockets, sets, generation = len(mast.active), mast.sets, mast.nextGeneration
	mast.mu.Unlock()
	if err := os.WriteFile(failOnce, []byte("fail"), 0600); err != nil {
		t.Fatal(err)
	}
	if reply := admin("key bot apply"); !strings.HasPrefix(reply, "Error:") || !strings.Contains(reply, "active key unchanged") {
		t.Fatal("candidate failure not atomic: " + reply)
	}
	waitAuthority(t, func() bool { return status()["bot"].State == "running" })
	if admin("key bot") != "KEY "+old.String() || admin("key bot pending") != pending {
		t.Fatal("failed candidate changed key authority")
	}
	checkOldData()
	if err := os.Chmod(filepath.Join(cfg.StateDir, "bot"), 0500); err != nil {
		t.Fatal(err)
	}
	reply := admin("key bot apply")
	if err := os.Chmod(filepath.Join(cfg.StateDir, "bot"), 0700); err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(reply, "Error:") || !strings.Contains(reply, "commit failed; active key unchanged") {
		t.Fatal("failed key persistence was not atomic: " + reply)
	}
	waitAuthority(t, func() bool { return status()["bot"].State == "running" })
	if admin("key bot") != "KEY "+old.String() || admin("key bot pending") != pending {
		t.Fatal("failed persistence changed active/pending key")
	}
	mast.mu.Lock()
	advertStart := len(mast.txPackets)
	// Old accepted work remains unresolved while the identity changes.
	mast.completeTX = false
	mast.mu.Unlock()
	if reply := admin("advert.zerohop"); !strings.Contains(reply, "queued") {
		t.Fatal(reply)
	}
	waitAuthority(t, func() bool { mast.mu.Lock(); defer mast.mu.Unlock(); return len(mast.txPackets) > advertStart })
	mast.mu.Lock()
	oldUncertain := append([]byte(nil), mast.txPackets[len(mast.txPackets)-1]...)
	advertStart = len(mast.txPackets)
	terminalStart := mast.txTerminals
	mast.completeTX = true
	mast.mu.Unlock()
	if err := os.WriteFile(failActivate, []byte("fail"), 0600); err != nil {
		t.Fatal(err)
	}
	reply = admin("key bot apply")
	if !strings.Contains(reply, "key saved but activation unavailable") {
		t.Fatal("activation failure did not disclose committed authority: " + reply)
	}
	if admin("key bot") != "KEY "+next.String() || status()["bot"].State != "faulted" ||
		status()["bot"].PublicKey != next.String() {
		t.Fatal("failed activation guessed a rollback or hid the saved key")
	}
	if reply := admin("key bot pending"); !strings.HasPrefix(reply, "Error:") {
		t.Fatal("failed activation did not consume committed stage")
	}
	mast.mu.Lock()
	if len(mast.txPackets) != advertStart {
		t.Fatal("failed dormant activation transmitted before acknowledgement")
	}
	mast.mu.Unlock()
	reply = admin("key bot apply")
	if reply != "KEY "+next.String()+" applied; zero-hop advert queued; verify RF" {
		t.Fatal("apply failed: " + reply)
	}
	if admin("key bot") != "KEY "+next.String() || status()["bot"].PublicKey != next.String() {
		t.Fatal("owner/HELLO/dashboard key diverged")
	}
	active, _ := state.Identity(cfg.StateDir, "bot")
	if active.PublicKey() != next.PublicKey() {
		t.Fatal("saved authority differs from native READY")
	}
	if status()["companion"].PublicKey != peer.String() ||
		!status()["companion"].ApplicationStartedAt.Equal(*before["companion"].ApplicationStartedAt) {
		t.Fatal("bot apply restarted or rekeyed companion")
	}
	mast.mu.Lock()
	if len(mast.active) != sockets || mast.sets != sets || mast.nextGeneration != generation {
		t.Fatal("bot apply reset/retuned shared radio source")
	}
	mast.mu.Unlock()
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets[advertStart:]...)
		completed := mast.txTerminals > terminalStart
		mast.mu.Unlock()
		for _, raw := range packets {
			if bytes.Equal(raw, oldUncertain) {
				t.Fatal("uncertain old transmission replayed")
			}
			p, err := meshcore.PacketFromBytes(raw)
			if err != nil || p.PayloadType() != meshcore.PayloadTypeAdvert {
				continue
			}
			a, err := meshcore.AdvertFromBytes(p.Payload)
			if err != nil || !a.Verify() || !a.PublicKey.Matches(next.Identity) ||
				a.AppData().Name != "Rekey Bot" || !p.IsRouteDirect() || p.PathLength != 0 {
				t.Fatal("rekey notification not a signed new-key zero-hop advert")
			}
			return completed
		}
		return false
	})
	if admin("channel status") != channelBefore || admin("source hash") != sourceBefore ||
		!strings.Contains(admin("discovery status"), "live=1") {
		t.Fatal("rekey changed device policy/source/channel")
	}
	// The same principal cannot see the old identity's notes or scheduler data.
	for _, kind := range []string{"kv", "timers", "reminders"} {
		data := nativeRekeyExport(t, admin, kind)
		if hex.EncodeToString(data[4:36]) != next.String() || data[69] != 0 {
			t.Fatalf("%s data reassigned to new identity", kind)
		}
	}
	stop()
	stop = nil
	stop = start()
	if admin("key bot") != "KEY "+next.String() || status()["bot"].PublicKey != next.String() {
		t.Fatal("applied identity lost on Go restart")
	}
	// Returning deliberately to the original full key recovers its own records.
	if reply := admin("key bot " + hex.EncodeToString(oldKey)); !strings.Contains(reply, "pending") {
		t.Fatal(reply)
	}
	if reply := admin("key bot apply"); !strings.Contains(reply, "applied;") {
		t.Fatal(reply)
	}
	checkOldData()
}

func nativeRekeyExport(t *testing.T, admin func(string) string, kind string) []byte {
	t.Helper()
	if reply := admin("data export " + kind + " caller " + strings.Repeat("ab", 32)); !strings.HasPrefix(reply, "PENDING") {
		t.Fatal(reply)
	}
	var manifest []string
	waitAuthority(t, func() bool {
		manifest = strings.Fields(admin("data status"))
		return len(manifest) == 3 && manifest[0] == "EXPORTED"
	})
	var data []byte
	for offset := 0; offset < 2422; offset += 48 {
		reply := admin(fmt.Sprintf("data read %s %d", manifest[2], offset/48))
		chunk, err := hex.DecodeString(strings.TrimPrefix(reply, "DATA "))
		if err != nil {
			t.Fatal(err)
		}
		data = append(data, chunk...)
	}
	if len(data) != 2422 {
		t.Fatalf("invalid data snapshot length %d", len(data))
	}
	return data
}
