package app

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/state"
)

func TestNativeBotRemindersUseTrustedHostClock(t *testing.T) {
	executable := os.Getenv("BOT_NATIVE_CLOCK_TEST_WORKER")
	if executable == "" {
		t.Skip("set BOT_NATIVE_CLOCK_TEST_WORKER to the test-only native clock worker")
	}
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"bot"}
	cfg.BotRuntime, cfg.BotNativeWorker = "native_lua", executable
	cfg.BotListen = "not-a-listener"
	cfg.StateDir, cfg.StatusListen = nativeBotStateDir(t), freeBotStatusAddress(t)
	mast := newAuthorityMast(t, cfg)
	draining := make(chan struct{})
	go func() {
		for {
			select {
			case <-mast.clients:
			case <-draining:
				return
			}
		}
	}()
	t.Cleanup(func() { close(draining) })
	mast.mu.Lock()
	mast.completeTX, mast.fastAirtime = true, true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")
	clockFile := filepath.Join(cfg.StateDir, "clock.fixture")
	setClock := func(status int, offsetSeconds int, uncertainty ...int) {
		t.Helper()
		errorUs := 5000
		if len(uncertainty) != 0 {
			errorUs = uncertainty[0]
		}
		data := []byte(fmt.Sprintf("%d 0 %d %d\n", status, int64(offsetSeconds)*int64(time.Second), errorUs))
		if err := os.WriteFile(clockFile+".new", data, 0600); err != nil {
			t.Fatal(err)
		}
		if err := os.Rename(clockFile+".new", clockFile); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("MESHCORE_NATIVE_CLOCK_FIXTURE", clockFile)
	setClock(0, 0)
	admin := func(command string) string {
		t.Helper()
		conn, err := net.DialTimeout("unix", socket, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		defer conn.Close()
		if err := conn.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
			t.Fatal(err)
		}
		if _, err := fmt.Fprintln(conn, command); err != nil {
			t.Fatal(err)
		}
		reply, err := bufio.NewReader(conn).ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		return strings.TrimSuffix(reply, "\n")
	}
	var stop func()
	start := func() {
		t.Helper()
		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan error, 1)
		go func() { done <- Run(ctx, cfg, logger) }()
		stop = func() {
			cancel()
			select {
			case err := <-done:
				if err != nil {
					t.Errorf("native reminder host shutdown: %v", err)
				}
			case <-time.After(5 * time.Second):
				t.Error("native reminder host did not stop")
			}
		}
		waitAuthority(t, func() bool {
			response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/status")
			if err != nil {
				return false
			}
			defer response.Body.Close()
			var roles map[string]roleStatus
			return json.NewDecoder(response.Body).Decode(&roles) == nil &&
				roles["bot"].State == "running" && roles["bot"].RadioConnected != nil &&
				*roles["bot"].RadioConnected
		})
	}
	defer func() {
		if stop != nil {
			stop()
		}
	}()
	restart := func() {
		t.Helper()
		stop()
		stop = nil
		start()
	}
	awaitClock := func(trusted bool) {
		t.Helper()
		value := "clock=0"
		if trusted {
			value = "clock=1"
		}
		waitAuthority(t, func() bool { return strings.Contains(admin("reminders status"), value) })
	}
	// The production executable must ignore the fixture even when it requests denial.
	production := os.Getenv("BOT_NATIVE_WORKER")
	productionAvailable := false
	if production != "" {
		var kernel syscall.Timex
		state, err := syscall.Adjtimex(&kernel)
		if err == nil && state != 5 && kernel.Status&(64|4096) == 0 &&
			kernel.Maxerror >= 0 && kernel.Maxerror < 2000000 &&
			kernel.Esterror >= 0 && kernel.Esterror < 2000000 {
			cfg.BotNativeWorker = production
			setClock(64, 0)
			start()
			awaitClock(true)
			if reply := admin("home"); !strings.Contains(reply, "clock=1") {
				t.Fatalf("HTTPS and scheduler disagree on kernel clock trust: %s", reply)
			}
			productionAvailable = true
			t.Log("Production worker ignored the denied fixture; HTTPS and scheduler trust the real kernel clock")
			stop()
			stop = nil
			cfg.BotNativeWorker = executable
			setClock(0, 0)
		}
	}
	start()
	awaitClock(true)
	if reply := admin("airtime 3600"); !strings.Contains(reply, "Saved") {
		t.Fatal(reply)
	}
	restart()
	awaitClock(true)
	bot, err := state.Identity(cfg.StateDir, "bot")
	if err != nil {
		t.Fatal(err)
	}
	first, other := meshcore.NewLocalIdentityFromSeed([32]byte{99}), meshcore.NewLocalIdentityFromSeed([32]byte{101})
	sequence := uint32(time.Now().Unix())
	transmitted := func() [][]byte {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return append([][]byte(nil), mast.txPackets...)
	}
	inject := func(packet *meshcore.Packet) {
		t.Helper()
		raw, err := packet.ToBytes()
		if err != nil {
			t.Fatal(err)
		}
		injectBotRF(t, mast, raw)
	}
	encrypted := func(primary bool, kind byte, plain []byte) *meshcore.Packet {
		t.Helper()
		peer := first
		if !primary {
			peer = other
		}
		secret, err := peer.SharedSecret(bot.Identity)
		if err != nil {
			t.Fatal(err)
		}
		sealed, err := meshcore.EncryptThenMAC(secret, plain)
		if err != nil {
			t.Fatal(err)
		}
		return &meshcore.Packet{
			Header:  meshcore.MakeHeader(meshcore.RouteTypeDirect, kind, 0),
			Payload: append([]byte{bot.PublicKeyBytes()[0], peer.PublicKeyBytes()[0]}, sealed...),
		}
	}
	learn := func(primary bool) {
		t.Helper()
		peer := first
		if !primary {
			peer = other
		}
		appData, err := (&meshcore.AdvertAppData{Type: "CHAT", Name: "Reminder caller"}).ToBytes()
		if err != nil {
			t.Fatal(err)
		}
		sequence++
		advert := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: sequence, RawAppData: appData}
		advert.SignWith(peer)
		payload, err := advert.ToBytes()
		if err != nil {
			t.Fatal(err)
		}
		inject(&meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0), Payload: payload})
		time.Sleep(50 * time.Millisecond)
		plain := []byte{0, 0xff, 0, 0, 0, 0}
		binary.LittleEndian.PutUint32(plain[2:], sequence)
		inject(encrypted(primary, meshcore.PayloadTypePath, plain))
		time.Sleep(50 * time.Millisecond)
	}
	messages := func(primary bool, offset int) []string {
		peer := first
		if !primary {
			peer = other
		}
		secret, err := peer.SharedSecret(bot.Identity)
		if err != nil {
			t.Fatal(err)
		}
		var result []string
		for _, raw := range transmitted()[offset:] {
			packet, err := meshcore.PacketFromBytes(raw)
			if err != nil || packet.PayloadType() != meshcore.PayloadTypeTxtMsg || len(packet.Payload) < 3 {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, packet.Payload[2:])
			if err == nil && len(plain) >= 5 {
				result = append(result, strings.TrimRight(string(plain[5:]), "\x00"))
			}
		}
		return result
	}
	send := func(primary bool, text string) int {
		t.Helper()
		offset := len(transmitted())
		sequence++
		plain := make([]byte, 5)
		binary.LittleEndian.PutUint32(plain, sequence)
		inject(encrypted(primary, meshcore.PayloadTypeTxtMsg, append(plain, text...)))
		return offset
	}
	command := func(primary bool, text string) string {
		t.Helper()
		offset := send(primary, text)
		var reply string
		deadline := time.Now().Add(9 * time.Second)
		for time.Now().Before(deadline) {
			for _, value := range messages(primary, offset) {
				if !strings.HasPrefix(value, "Reminder #") {
					reply = value
					return reply
				}
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("native command %q received no reply; %s; %s; messages=%v", text,
			admin("status"), admin("reminders"), messages(primary, offset))
		return ""
	}
	create := func(seconds int, text string) uint32 {
		t.Helper()
		reply := command(true, fmt.Sprintf("!remind %ds %s", seconds, text))
		var id, due uint32
		if _, err := fmt.Sscanf(reply, "#%d pending; due UTC %d", &id, &due); err != nil {
			t.Fatalf("reminder creation failed: %q, %v", reply, err)
		}
		if due < uint32(time.Now().Unix())+uint32(seconds) {
			t.Fatalf("reminder deadline predates conservative UTC: %q", reply)
		}
		return id
	}
	countReminder := func(id uint32) int {
		wanted, count := fmt.Sprintf("Reminder #%d:", id), 0
		for _, reply := range messages(true, 0) {
			if strings.HasPrefix(reply, wanted) {
				count++
			}
		}
		return count
	}
	awaitReminder := func(id uint32) {
		t.Helper()
		waitAuthority(t, func() bool { return countReminder(id) == 1 })
		waitAuthority(t, func() bool {
			return strings.Contains(command(true, "!reminders"), fmt.Sprintf("%d sent", id))
		})
	}
	learn(true)
	learn(false)
	if reply := command(true, "!remind 1s denied"); !strings.Contains(reply, "grant") {
		t.Fatalf("default-off reminder grant was bypassed: %q", reply)
	}
	if reply := admin("reminders on"); !strings.Contains(reply, "Saved/applied") {
		t.Fatal(reply)
	}
	for _, errorUs := range []int{990000, 1000000, 1024000} {
		setClock(0, 0, errorUs)
		awaitClock(true)
		if reply := admin("home"); !strings.Contains(reply, "clock=1") {
			t.Fatalf("normal timesyncd uncertainty disabled HTTPS: %s", reply)
		}
		if reply := admin("clock"); !strings.Contains(reply, "https=1 scheduler=1") ||
			!strings.Contains(reply, "reason=usable") {
			t.Fatalf("normal timesyncd uncertainty disabled scheduler: %s", reply)
		}
	}
	ramping := create(1, "timesyncd default polling")
	awaitReminder(ramping)
	setClock(0, 0, 5000)
	awaitClock(true)
	if reply := admin("clock"); !strings.Contains(reply, "https=1 scheduler=1") {
		t.Fatalf("normal NTP poll reset caused a clock flap: %s", reply)
	}
	uncertain := create(1, "held by excessive uncertainty")
	setClock(0, 0, 2100000)
	awaitClock(false)
	if reply := admin("clock"); !strings.Contains(reply, "https=1 scheduler=0") ||
		!strings.Contains(reply, "reason=scheduler-error-bound-exceeded") {
		t.Fatalf("excessive scheduler uncertainty did not fence only deadlines: %s", reply)
	}
	if reply := admin("home"); !strings.Contains(reply, "clock=1") {
		t.Fatalf("scheduler uncertainty policy changed synchronized HTTPS behavior: %s", reply)
	}
	time.Sleep(3 * time.Second)
	if countReminder(uncertain) != 0 {
		t.Fatal("excessive scheduler uncertainty dispatched a pending reminder")
	}
	if reply := command(true, "!remind 1s excessive uncertainty"); !strings.Contains(reply, "clock unsynchronized") {
		t.Fatalf("excessive uncertainty admitted a deadline: %q", reply)
	}
	setClock(0, 0, 5000)
	awaitClock(true)
	awaitReminder(uncertain)
	cancelled := create(1, "cancelled")
	if reply := command(false, "!reminders"); reply != "No personal reminders" {
		t.Fatalf("another full-key caller read reminders: %q", reply)
	}
	if reply := command(false, "!cancel "+strconv.FormatUint(uint64(cancelled), 10)); !strings.Contains(reply, "not found") {
		t.Fatalf("another full-key caller cancelled a reminder: %q", reply)
	}
	if reply := command(true, "!cancel "+strconv.FormatUint(uint64(cancelled), 10)); !strings.Contains(reply, "cancelled") {
		t.Fatal(reply)
	}
	if productionAvailable {
		cfg.BotNativeWorker = production
		setClock(64, 0)
		restart()
		awaitClock(true)
		learn(true)
		learn(false)
	}
	persistent := create(4, "after restart")
	restart()
	awaitClock(true)
	learn(true)
	learn(false)
	awaitReminder(persistent)
	if productionAvailable {
		t.Log("Production worker delivered the durable reminder once after restart using the real kernel clock")
	}
	restart()
	awaitClock(true)
	learn(true)
	learn(false)
	time.Sleep(200 * time.Millisecond)
	if countReminder(persistent) != 1 || countReminder(cancelled) != 0 {
		t.Fatal("restart replayed sent/cancelled reminder effects")
	}
	if productionAvailable {
		cfg.BotNativeWorker = executable
		setClock(0, 0)
		restart()
		awaitClock(true)
		learn(true)
		learn(false)
	}

	held := create(1, "held without UTC")
	setClock(64, 0)
	awaitClock(false)
	if reply := admin("home"); !strings.Contains(reply, "clock=0") {
		t.Fatalf("unsynchronized kernel clock admitted HTTPS: %s", reply)
	}
	time.Sleep(3 * time.Second)
	if countReminder(held) != 0 {
		t.Fatal("unsynchronized host clock delivered a reminder")
	}
	if reply := command(true, "!remind 1s untrusted"); !strings.Contains(reply, "clock unsynchronized") {
		t.Fatalf("untrusted clock admitted a new deadline: %q", reply)
	}
	if api := admin("source api storage"); !strings.Contains(api, "autonomous-reminders=0") {
		t.Fatal(api)
	}
	setClock(0, 0)
	awaitClock(true)
	awaitReminder(held)

	paused := create(1, "held without grant")
	if reply := admin("reminders off"); !strings.Contains(reply, "Saved/applied") {
		t.Fatal(reply)
	}
	time.Sleep(3 * time.Second)
	if countReminder(paused) != 0 {
		t.Fatal("withdrawn reminder grant delivered pending work")
	}
	if reply := command(true, "!remind 1s denied again"); !strings.Contains(reply, "grant") {
		t.Fatal(reply)
	}
	if reply := admin("reminders on"); !strings.Contains(reply, "Saved/applied") {
		t.Fatal(reply)
	}
	awaitReminder(paused)

	jumped := create(1, "time jump must not send")
	setClock(0, 120)
	awaitClock(false)
	if countReminder(jumped) != 0 {
		t.Fatal("discontinuous UTC delivered a reminder before recovery")
	}
	awaitClock(true)
	waitAuthority(t, func() bool { return strings.Contains(command(true, "!reminders"), fmt.Sprintf("%d overdue", jumped)) })
	setClock(0, -120)
	awaitClock(false)
	setClock(0, 0)
	awaitClock(true)
	if countReminder(jumped) != 0 {
		t.Fatal("clock correction replayed an overdue reminder")
	}

	// A source change fences a command yielding before reminder creation.
	script := []byte("function later() sleep(1500) reminder.after(1,'source-fenced') return 'scheduled' end function personal() return reminder.list() end")
	hash := sha256.Sum256(script)
	const uploadID = "f0f0f0f0f0f0f0f0"
	if reply := admin(fmt.Sprintf("source begin %s %d %x", uploadID, len(script), hash)); !strings.HasPrefix(reply, "ACK ") {
		t.Fatal(reply)
	}
	for offset := 0; offset < len(script); offset += 48 {
		if reply := admin(fmt.Sprintf("source chunk %s %d %x", uploadID, offset/48, script[offset:min(offset+48, len(script))])); !strings.HasPrefix(reply, "ACK ") {
			t.Fatal(reply)
		}
	}
	if reply := admin("source commit " + uploadID); !strings.Contains(reply, "Accepted") {
		t.Fatal(reply)
	}
	waitAuthority(t, func() bool { return strings.Contains(admin("source status"), "durably saved and active") })
	before := command(true, "!personal")
	send(true, "!later")
	waitAuthority(t, func() bool { return !strings.Contains(admin("status"), "jobs=0") })
	if reply := admin("source retry"); !strings.Contains(reply, "Accepted") {
		t.Fatal(reply)
	}
	waitAuthority(t, func() bool { return strings.Contains(admin("source status"), "durably saved and active") })
	time.Sleep(1700 * time.Millisecond)
	if after := command(true, "!personal"); after != before {
		t.Fatalf("source-withdrawn invocation created a reminder: before=%q after=%q", before, after)
	}

	// Native BRD1 restore retains terminal state and cancels pending work.
	if reply := admin("source remove"); !strings.Contains(reply, "Accepted") {
		t.Fatal(reply)
	}
	waitAuthority(t, func() bool {
		reply := admin("source status")
		return strings.Contains(reply, "active=3") && strings.Contains(reply, "durably saved and active")
	})
	restored := create(1, "restore must not rearm")
	admin("reminders off")
	if reply := admin("data export reminders caller " + first.String()); !strings.HasPrefix(reply, "PENDING") {
		t.Fatal(reply)
	}
	var manifest []string
	waitAuthority(t, func() bool {
		manifest = strings.Fields(admin("data status"))
		return len(manifest) == 3 && manifest[0] == "EXPORTED"
	})
	var snapshot []byte
	for index := 0; index < 51; index++ {
		reply := admin(fmt.Sprintf("data read %s %d", manifest[2], index))
		if !strings.HasPrefix(reply, "DATA ") {
			t.Fatal(reply)
		}
		bytes, err := hex.DecodeString(reply[5:])
		if err != nil {
			t.Fatal(err)
		}
		snapshot = append(snapshot, bytes...)
	}
	digest := sha256.Sum256(snapshot)
	snapshotHash := fmt.Sprintf("%x", digest)
	if len(snapshot) != 2422 || snapshotHash != manifest[1] || string(snapshot[:4]) != "BRD\x01" {
		t.Fatal("native reminder export invalid")
	}
	id := snapshotHash[:16]
	admin("data begin " + id + " " + snapshotHash)
	for offset := 0; offset < len(snapshot); offset += 48 {
		admin(fmt.Sprintf("data chunk %s %d %x", id, offset/48, snapshot[offset:min(offset+48, len(snapshot))]))
	}
	admin("data stage " + id)
	waitAuthority(t, func() bool { return strings.HasPrefix(admin("data status"), "STAGED") })
	if reply := admin("data restore " + id); !strings.HasPrefix(reply, "Error:") {
		t.Fatalf("scheduler restore omitted explicit no-rearm consent: %s", reply)
	}
	admin("data restore " + id + " no-rearm")
	waitAuthority(t, func() bool { return strings.HasPrefix(admin("data status"), "COMMITTED") })
	admin("reminders on")
	restart()
	awaitClock(true)
	learn(true)
	time.Sleep(3 * time.Second)
	if countReminder(restored) != 0 {
		t.Fatal("no-rearm reminder restore replayed an effect after restart")
	}
}
