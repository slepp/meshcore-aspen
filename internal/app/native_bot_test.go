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
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/state"
)

func TestNativeBotUsesSharedMastAndPrivateStagedSource(t *testing.T) {
	executable := os.Getenv("BOT_NATIVE_WORKER")
	if executable == "" {
		t.Skip("set BOT_NATIVE_WORKER to the Make-built native production worker")
	}
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"bot"}
	cfg.BotRuntime = "native_lua"
	cfg.BotNativeWorker = executable
	cfg.BotListen = "not-a-listener"
	cfg.StateDir = nativeBotStateDir(t)
	cfg.StatusListen = freeBotStatusAddress(t)
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.completeTX = true
	mast.fastAirtime = true
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	socket := filepath.Join(cfg.StateDir, "bot", "native", "admin.sock")

	start := func() context.CancelFunc {
		ctx, cancel := context.WithCancel(context.Background())
		stopped := make(chan error, 1)
		go func() { stopped <- Run(ctx, cfg, logger) }()
		waitAuthority(t, func() bool {
			response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/status")
			if err != nil {
				return false
			}
			defer response.Body.Close()
			var status map[string]roleStatus
			if json.NewDecoder(response.Body).Decode(&status) != nil {
				return false
			}
			return status["bot"].ApplicationKind == "native_lua" &&
				status["bot"].State == "running" && status["bot"].Endpoint == "" &&
				status["bot"].RadioConnected != nil && *status["bot"].RadioConnected
		})
		response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/readyz")
		if err != nil {
			t.Fatal(err)
		}
		var check readiness
		if err := json.NewDecoder(response.Body).Decode(&check); err != nil {
			response.Body.Close()
			t.Fatal(err)
		}
		response.Body.Close()
		if reason := check.NotReady["bot"]; reason != "" {
			t.Fatalf("ready native bot was not included in host readiness: %s", reason)
		}
		return func() {
			cancel()
			select {
			case err := <-stopped:
				if err != nil {
					t.Errorf("native bot host shutdown: %v", err)
				}
			case <-time.After(5 * time.Second):
				t.Error("native bot host did not stop")
			}
		}
	}
	admin := func(command string) string {
		t.Helper()
		connection, err := net.DialTimeout("unix", socket, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		defer connection.Close()
		if err := connection.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
			t.Fatal(err)
		}
		if _, err := fmt.Fprintln(connection, command); err != nil {
			t.Fatal(err)
		}
		reply, err := bufio.NewReader(connection).ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		return strings.TrimSuffix(reply, "\n")
	}
	stop := start()
	if info, err := os.Stat(socket); err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("native bot owner socket not private: %v, %v", info, err)
	}
	peer := meshcore.NewLocalIdentityFromSeed([32]byte{85})
	bot, err := state.Identity(cfg.StateDir, "bot")
	if err != nil {
		t.Fatal(err)
	}
	appData, err := (&meshcore.AdvertAppData{Type: "CHAT", Name: "Native peer"}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	advert := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: uint32(time.Now().Unix()), RawAppData: appData}
	advert.SignWith(peer)
	advertBytes, err := advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	peerAdvert, err := (&meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0),
		PathLength: 0x81, Path: []byte{0x12, 0x34, 0x56}, Payload: advertBytes,
	}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, peerAdvert)
	time.Sleep(100 * time.Millisecond)
	secret, err := peer.SharedSecret(bot.Identity)
	if err != nil {
		t.Fatal(err)
	}
	text := []byte{0, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(text, uint32(time.Now().Unix()))
	text = append(text, "!status"...)
	ciphertext, err := meshcore.EncryptThenMAC(secret, text)
	if err != nil {
		t.Fatal(err)
	}
	dm := &meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeTxtMsg, 0),
		PathLength: 0x81, Path: []byte{0x12, 0x34, 0x56},
		Payload: append([]byte{bot.PublicKeyBytes()[0], peer.PublicKeyBytes()[0]}, ciphertext...),
	}
	dmBytes, err := dm.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, dmBytes)
	var gotReply bool
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets...)
		terminals := mast.txTerminals
		mast.mu.Unlock()
		for _, raw := range packets {
			p, err := meshcore.PacketFromBytes(raw)
			if err != nil || p.PayloadType() != meshcore.PayloadTypeTxtMsg || len(p.Payload) < 3 {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, p.Payload[2:])
			if err == nil && strings.Contains(string(plain), "Snapshot bot=ready") {
				gotReply = terminals >= 2
				return gotReply
			}
		}
		return false
	})
	if !gotReply {
		t.Fatal("native Lua DM reply was not transmitted with a final RF completion")
	}
	advert.PublicKey = peer.Identity
	advert.Timestamp = uint32(time.Now().Unix()) + 1
	advert.SignWith(peer)
	advertBytes, err = advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	peerAdvert, err = (&meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0),
		PathLength: 0x81, Path: []byte{0x16, 0x38, 0x73}, Payload: advertBytes,
	}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, peerAdvert)
	time.Sleep(1100 * time.Millisecond)
	secret, err = peer.SharedSecret(bot.Identity)
	if err != nil {
		t.Fatal(err)
	}
	text = []byte{0, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(text, uint32(time.Now().Unix())+1)
	text = append(text, "!air"...)
	ciphertext, err = meshcore.EncryptThenMAC(secret, text)
	if err != nil {
		t.Fatal(err)
	}
	dm.Payload = append([]byte{bot.PublicKeyBytes()[0], peer.PublicKeyBytes()[0]}, ciphertext...)
	dmBytes, err = dm.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, dmBytes)
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets...)
		mast.mu.Unlock()
		for _, raw := range packets {
			p, err := meshcore.PacketFromBytes(raw)
			if err != nil || p.PayloadType() != meshcore.PayloadTypeTxtMsg || len(p.Payload) < 3 {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, p.Payload[2:])
			if err == nil && strings.Contains(string(plain), "Air snapshot ms: credit bot=30 all=120") {
				return true
			}
		}
		return false
	})
	if reply := admin("source status"); !strings.Contains(reply, "active=3") {
		t.Fatalf("bundled native source not running: %s", reply)
	}
	if reply := admin("path status"); !strings.Contains(reply, "path_hash_mode=2 width=3") {
		t.Fatalf("native bot did not inherit deployed three-byte hash mode: %s", reply)
	}
	if reply := admin("name Host Bot"); !strings.Contains(reply, "Saved and applied") {
		t.Fatalf("name was not persisted/live: %s", reply)
	}
	if reply := admin("name"); reply != "Name: Host Bot" {
		t.Fatalf("bot name readback diverged: %s", reply)
	}
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return mast.txTerminals == len(mast.txPackets)
	})
	mast.mu.Lock()
	advertStart := len(mast.txPackets)
	mast.mu.Unlock()
	if reply := admin("advert.zerohop"); reply != "Bot zero-hop advert queued; verify RF" {
		t.Fatalf("owner rename advert was not queued immediately: %s", reply)
	}
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets[advertStart:]...)
		transmitted := mast.txTerminals == len(mast.txPackets)
		mast.mu.Unlock()
		for _, raw := range packets {
			packet, err := meshcore.PacketFromBytes(raw)
			if err != nil || packet.PayloadType() != meshcore.PayloadTypeAdvert {
				continue
			}
			advert, err := meshcore.AdvertFromBytes(packet.Payload)
			if err != nil || !advert.Verify() || !advert.PublicKey.Matches(bot.Identity) ||
				advert.AppData().Name != "Host Bot" || advert.AppData().Type != "CHAT" ||
				!packet.IsRouteDirect() || packet.PathLength != 0 || len(packet.Path) != 0 {
				t.Fatalf("owner rename advert was not signed, named and zero-hop: %v %v", packet, err)
			}
			return transmitted
		}
		return false
	})
	mast.mu.Lock()
	ownerAdverts := len(mast.txPackets)
	mast.mu.Unlock()
	if reply := admin("advert.zerohop"); reply != "Error: owner advert rate limited (1 minute)" {
		t.Fatalf("owner bypass ignored its cooldown after terminal RF completion: %s", reply)
	}
	mast.mu.Lock()
	extraAdvert := len(mast.txPackets) != ownerAdverts
	mast.mu.Unlock()
	if extraAdvert {
		t.Fatal("rate-limited owner notification transmitted another advert")
	}
	if reply := admin("channel 54657374 00112233445566778899aabbccddeeff"); !strings.Contains(reply, "reboot required") {
		t.Fatalf("channel change did not disclose restart requirement: %s", reply)
	}
	if reply := admin("channel status"); !strings.Contains(reply, "Channel 0 name=Test key-id=") ||
		strings.Contains(reply, "00112233445566778899aabbccddeeff") {
		t.Fatalf("private channel readback missing or leaked key: %s", reply)
	}
	if reply := admin("airtime status"); reply != "Bot airtime saved=360 ms/min; reboot applies" {
		t.Fatalf("unexpected saved bot airtime: %s", reply)
	}
	if reply := admin("airtime 1200"); reply != "Saved bot airtime; reboot required" {
		t.Fatalf("bot airtime update was not persisted: %s", reply)
	}
	if reply := admin("identity rotate"); !strings.HasPrefix(reply, "Error: use key bot") {
		t.Fatalf("unrequested random identity rotation was exposed: %s", reply)
	}
	checkDataAfterRestart := nativeBotDataRoundTrip(t, admin, bot.String())
	var script []byte
	for index := 0; index < 86; index++ {
		reply := admin(fmt.Sprintf("source read %d", index))
		if reply == "EOF" {
			break
		}
		if !strings.HasPrefix(reply, "DATA ") {
			t.Fatalf("native source read %d: %s", index, reply)
		}
		chunk, err := hex.DecodeString(strings.TrimPrefix(reply, "DATA "))
		if err != nil {
			t.Fatal(err)
		}
		script = append(script, chunk...)
	}
	if len(script) == 0 || len(script) > 4096 {
		t.Fatalf("invalid bundled native script length %d", len(script))
	}
	digest := sha256.Sum256(script)
	const id = "0123456789abcdef"
	if reply := admin(fmt.Sprintf("source begin %s %d %x", id, len(script), digest)); !strings.HasPrefix(reply, "ACK ") {
		t.Fatalf("source stage failed: %s", reply)
	}
	for offset := 0; offset < len(script); offset += 48 {
		end := min(offset+48, len(script))
		reply := admin(fmt.Sprintf("source chunk %s %d %x", id, offset/48, script[offset:end]))
		if !strings.HasPrefix(reply, "ACK ") {
			t.Fatalf("source chunk %d failed: %s", offset/48, reply)
		}
	}
	if reply := admin("source commit " + id); !strings.HasPrefix(reply, "Accepted verification") {
		t.Fatalf("source commit failed: %s", reply)
	}
	waitAuthority(t, func() bool {
		reply := admin("source status")
		return strings.Contains(reply, "source durably saved and active")
	})
	activated := admin("source status")
	if !strings.Contains(activated, "active=0") {
		t.Fatalf("source journal did not select staged script: %s", activated)
	}
	stop()
	stop = start()
	restarted := admin("source status")
	if !strings.Contains(restarted, "active=0") || !strings.Contains(restarted, "source durably saved and active") {
		t.Fatalf("active script or identity was not restored on restart: %s", restarted)
	}
	if reply := admin("name"); reply != "Name: Host Bot" {
		t.Fatalf("bot name lost on restart: %s", reply)
	}
	if reply := admin("channel status"); !strings.Contains(reply, "Channel 0 name=Test key-id=") {
		t.Fatalf("bot channel lost on restart: %s", reply)
	}
	if reply := admin("airtime status"); reply != "Bot airtime saved=1200 ms/min; reboot applies" {
		t.Fatalf("saved bot airtime lost on restart: %s", reply)
	}
	checkDataAfterRestart()
	stop()
	if err := os.Remove(filepath.Join(cfg.StateDir, "bot", "native", "spiffs", "spiffs.a.lua")); err != nil {
		t.Fatalf("cannot simulate missing durable active slot: %v", err)
	}
	recoveryContext, cancelRecovery := context.WithCancel(context.Background())
	recoveryDone := make(chan error, 1)
	go func() { recoveryDone <- Run(recoveryContext, cfg, logger) }()
	defer func() {
		cancelRecovery()
		select {
		case err := <-recoveryDone:
			if err != nil {
				t.Errorf("native bot recovery host shutdown: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Error("native bot recovery host did not stop")
		}
	}()
	waitAuthority(t, func() bool {
		response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/status")
		if err != nil {
			return false
		}
		defer response.Body.Close()
		var status map[string]roleStatus
		return json.NewDecoder(response.Body).Decode(&status) == nil &&
			status["bot"].State == "faulted" && status["bot"].PublicKey == bot.String()
	})
	if reply := admin("source status"); !strings.HasPrefix(reply, "Error:") {
		t.Fatalf("missing native source slot was treated as ready: %s", reply)
	}
	if reply := admin("source remove"); !strings.Contains(reply, "restoring bundled handlers") {
		t.Fatalf("sealed source could not be repaired privately: %s", reply)
	}
	waitAuthority(t, func() bool {
		response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/status")
		if err != nil {
			return false
		}
		defer response.Body.Close()
		var status map[string]roleStatus
		return json.NewDecoder(response.Body).Decode(&status) == nil &&
			status["bot"].State == "running" && status["bot"].PublicKey == bot.String()
	})
	if api := admin("source api"); !strings.Contains(api, "commands=8") {
		t.Fatalf("native source API lost its legacy commands field: %s", api)
	}
	packageAPI := admin("source api package")
	for _, capability := range []string{"cmdmeta", "mesh-chan", "mesh-dest"} {
		if !strings.Contains(packageAPI, capability) {
			t.Fatalf("compiled native capability %q not available to host package: %s", capability, packageAPI)
		}
	}
	if !strings.Contains(packageAPI, "https") {
		t.Fatalf("host package omitted its native HTTPS worker: %s", packageAPI)
	}
	if grant := admin("home"); !strings.Contains(grant, "configured=0 saved=0 applied=0") {
		t.Fatalf("host network grant enabled without configured credentials: %s", grant)
	}
	packageSource := []byte("--@meshcore-bot/1;name=host-native;version=1.0.0;runtime=lua-5.5.1;api=named-commands-v1;caps=cmdmeta,mesh-chan,mesh-dest;schema=none;rollback=none\nfunction hello() reply('native-package-active') end\nfunction announce() return advert() end\n")
	packageHash := sha256.Sum256(packageSource)
	const packageID = "1020304050607080"
	if reply := admin(fmt.Sprintf("source begin %s %d %x", packageID, len(packageSource), packageHash)); !strings.HasPrefix(reply, "ACK ") {
		t.Fatalf("native package staging rejected actual merged capabilities: %s", reply)
	}
	for offset := 0; offset < len(packageSource); offset += 48 {
		end := min(offset+48, len(packageSource))
		if reply := admin(fmt.Sprintf("source chunk %s %d %x", packageID, offset/48, packageSource[offset:end])); !strings.HasPrefix(reply, "ACK ") {
			t.Fatalf("native package chunk %d rejected: %s", offset/48, reply)
		}
	}
	if reply := admin("source commit " + packageID); !strings.HasPrefix(reply, "Accepted verification") {
		t.Fatalf("native package verification rejected: %s", reply)
	}
	waitAuthority(t, func() bool {
		status := admin("source status")
		return strings.Contains(status, "durably saved and active") &&
			!strings.Contains(status, "active=3")
	})
	if metadata := admin("source metadata"); !strings.Contains(metadata, "META name=host-native ver=1.0.0 schema=none") {
		t.Fatalf("native package activation lost its metadata: %s", metadata)
	}
	peer = meshcore.NewLocalIdentityFromSeed([32]byte{88})
	advert.PublicKey = peer.Identity
	advert.Timestamp = uint32(time.Now().Unix()) + 2
	advert.SignWith(peer)
	advertBytes, err = advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	peerAdvert, err = (&meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0),
		PathLength: 0x81, Path: []byte{0x27, 0x48, 0x69}, Payload: advertBytes,
	}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, peerAdvert)
	time.Sleep(100 * time.Millisecond)
	secret, err = peer.SharedSecret(bot.Identity)
	if err != nil {
		t.Fatal(err)
	}
	text = make([]byte, 5)
	binary.LittleEndian.PutUint32(text, uint32(time.Now().Unix())+2)
	text = append(text, "!hello"...)
	ciphertext, err = meshcore.EncryptThenMAC(secret, text)
	if err != nil {
		t.Fatal(err)
	}
	dm.Payload = append([]byte{bot.PublicKeyBytes()[0], peer.PublicKeyBytes()[0]}, ciphertext...)
	dmBytes, err = dm.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, dmBytes)
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets...)
		mast.mu.Unlock()
		for _, raw := range packets {
			packet, err := meshcore.PacketFromBytes(raw)
			if err != nil || packet.PayloadType() != meshcore.PayloadTypeTxtMsg || len(packet.Payload) < 3 {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, packet.Payload[2:])
			if err == nil && strings.Contains(string(plain), "native-package-active") {
				return true
			}
		}
		return false
	})
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return mast.txTerminals == len(mast.txPackets)
	})
	mast.mu.Lock()
	ownerStart := len(mast.txPackets)
	mast.mu.Unlock()
	if reply := admin("advert.zerohop"); reply != "Bot zero-hop advert queued; verify RF" {
		t.Fatalf("owner notification after source activation was not queued: %s", reply)
	}
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		defer mast.mu.Unlock()
		return len(mast.txPackets) > ownerStart && mast.txTerminals == len(mast.txPackets)
	})
	peer = meshcore.NewLocalIdentityFromSeed([32]byte{89})
	advert.PublicKey = peer.Identity
	advert.Timestamp = uint32(time.Now().Unix()) + 3
	advert.SignWith(peer)
	advertBytes, err = advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	peerAdvert, err = (&meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0),
		PathLength: 0x81, Path: []byte{0x39, 0x59, 0x79}, Payload: advertBytes,
	}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, peerAdvert)
	time.Sleep(100 * time.Millisecond)
	secret, err = peer.SharedSecret(bot.Identity)
	if err != nil {
		t.Fatal(err)
	}
	mast.mu.Lock()
	advertStart = len(mast.txPackets)
	mast.mu.Unlock()
	text = make([]byte, 5)
	binary.LittleEndian.PutUint32(text, uint32(time.Now().Unix())+3)
	text = append(text, "!announce"...)
	ciphertext, err = meshcore.EncryptThenMAC(secret, text)
	if err != nil {
		t.Fatal(err)
	}
	dm.Payload = append([]byte{bot.PublicKeyBytes()[0], peer.PublicKeyBytes()[0]}, ciphertext...)
	dmBytes, err = dm.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, dmBytes)
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets[advertStart:]...)
		mast.mu.Unlock()
		for _, raw := range packets {
			packet, err := meshcore.PacketFromBytes(raw)
			if err != nil {
				continue
			}
			if packet.PayloadType() == meshcore.PayloadTypeAdvert {
				t.Fatal("public Lua advert bypassed the 15-minute interval after owner notification")
			}
			if packet.PayloadType() != meshcore.PayloadTypeTxtMsg || len(packet.Payload) < 3 {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, packet.Payload[2:])
			if err == nil && strings.Contains(string(plain), "advert unavailable") {
				return true
			}
		}
		return false
	})
}

func nativeBotStateDir(t *testing.T) string {
	t.Helper()
	// Keep the private Unix socket below sockaddr_un's limit in nested worktrees.
	directory, err := os.MkdirTemp(os.TempDir(), ".n-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := os.RemoveAll(directory); err != nil {
			t.Error(err)
		}
	})
	absolute, err := filepath.Abs(directory)
	if err != nil {
		t.Fatal(err)
	}
	return absolute
}

func freeBotStatusAddress(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	return address
}

func injectBotRF(t *testing.T, mast *authorityMast, packet []byte) {
	t.Helper()
	mast.mu.Lock()
	port := 0
	if len(mast.active) == 0 ||
		!mast.extendedSubports && len(mast.active)%2 != 0 {
		mast.mu.Unlock()
		t.Fatalf("native bot did not own a real mast source or subport: %d", len(mast.active))
	}
	connection := mast.active[len(mast.active)-1]
	if mast.extendedSubports && len(mast.active) == 1 {
		port = 1
	}
	mast.mu.Unlock()
	wire := append(hardware.EncodeFrame(port, hardware.KISS_CMD_DATA, packet),
		hardware.EncodeHardwareFrame(port, hardware.HW_RESP_RX_META, []byte{0x14, 0xb8})...)
	if _, err := connection.Write(wire); err != nil {
		t.Fatal(err)
	}
}

func TestNativeBotSharesOneMKISSSocketWithoutAnotherScheduler(t *testing.T) {
	executable := os.Getenv("BOT_NATIVE_WORKER")
	if executable == "" {
		t.Skip("set BOT_NATIVE_WORKER to the Make-built native production worker")
	}
	cfg := DefaultConfig()
	cfg.EnabledRoles = []string{"bot"}
	cfg.BotRuntime = "native_lua"
	cfg.BotNativeWorker = executable
	cfg.RadioSession = "required"
	cfg.PHYAuthority = "modem"
	cfg.StateDir = nativeBotStateDir(t)
	cfg.StatusListen = freeBotStatusAddress(t)
	mast := newAuthorityMast(t, cfg)
	mast.mu.Lock()
	mast.completeTX, mast.fastAirtime, mast.extendedSubports = true, true, true
	mast.externalSlots, mast.subports = 4, 4
	mast.mu.Unlock()
	cfg.RadioAddress = mast.listener.Addr().String()
	ctx, cancel := context.WithCancel(context.Background())
	stopped := make(chan error, 1)
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	go func() { stopped <- Run(ctx, cfg, logger) }()
	defer func() {
		cancel()
		select {
		case err := <-stopped:
			if err != nil {
				t.Errorf("aggregate native bot shutdown: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Error("aggregate native bot did not stop")
		}
	}()
	waitAuthority(t, func() bool {
		response, err := (&http.Client{Timeout: time.Second}).Get("http://" + cfg.StatusListen + "/status")
		if err != nil {
			return false
		}
		defer response.Body.Close()
		var status map[string]roleStatus
		return json.NewDecoder(response.Body).Decode(&status) == nil &&
			status["bot"].State == "running" && status["bot"].ApplicationKind == "native_lua"
	})
	mast.mu.Lock()
	sockets := len(mast.active)
	mast.mu.Unlock()
	if sockets != 1 {
		t.Fatalf("native bot plus controller opened %d physical sockets, want one aggregate", sockets)
	}
	peer := meshcore.NewLocalIdentityFromSeed([32]byte{87})
	bot, err := state.Identity(cfg.StateDir, "bot")
	if err != nil {
		t.Fatal(err)
	}
	application, err := (&meshcore.AdvertAppData{Type: "CHAT", Name: "MKISS peer"}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	advert := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: uint32(time.Now().Unix()), RawAppData: application}
	advert.SignWith(peer)
	payload, err := advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	packet := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeAdvert, 0),
		PathLength: 0x81, Path: []byte{0x42, 0x43, 0x44}, Payload: payload}
	raw, err := packet.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, raw)
	time.Sleep(100 * time.Millisecond)
	secret, err := peer.SharedSecret(bot.Identity)
	if err != nil {
		t.Fatal(err)
	}
	message := make([]byte, 5)
	binary.LittleEndian.PutUint32(message, uint32(time.Now().Unix()))
	message = append(message, "!status"...)
	sealed, err := meshcore.EncryptThenMAC(secret, message)
	if err != nil {
		t.Fatal(err)
	}
	packet = &meshcore.Packet{
		Header:     meshcore.MakeHeader(meshcore.RouteTypeFlood, meshcore.PayloadTypeTxtMsg, 0),
		PathLength: 0x81, Path: []byte{0x42, 0x43, 0x44},
		Payload: append([]byte{bot.PublicKeyBytes()[0], peer.PublicKeyBytes()[0]}, sealed...),
	}
	raw, err = packet.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	injectBotRF(t, mast, raw)
	waitAuthority(t, func() bool {
		mast.mu.Lock()
		packets := append([][]byte(nil), mast.txPackets...)
		terminals := mast.txTerminals
		mast.mu.Unlock()
		for _, encoded := range packets {
			p, err := meshcore.PacketFromBytes(encoded)
			if err != nil || p.PayloadType() != meshcore.PayloadTypeTxtMsg || len(p.Payload) < 3 {
				continue
			}
			plain, err := meshcore.MACThenDecrypt(secret, p.Payload[2:])
			if err == nil && strings.Contains(string(plain), "Snapshot bot=ready") && terminals >= 2 {
				return true
			}
		}
		return false
	})
}
