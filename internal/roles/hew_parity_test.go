package roles

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
)

// The migration test consumes actual Go-written state, not a hand-authored
// approximation of diskState's JSON field and byte-slice encodings.
func TestWillowMigrationFixture(t *testing.T) {
	root := os.Getenv("MESHCORE_WILLOW_FIXTURE")
	if root == "" {
		t.Skip("set MESHCORE_WILLOW_FIXTURE to a new private synthetic fixture directory")
	}
	if err := os.Mkdir(root, 0700); err != nil {
		t.Fatal(err)
	}
	for _, role := range []struct {
		name string
		seed byte
		room bool
	}{{"repeater", 1, false}, {"room", 4, true}, {"bot", 5, false}} {
		dir := filepath.Join(root, role.name)
		if err := os.Mkdir(dir, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "identity.seed"), bytes.Repeat([]byte{role.seed}, 32), 0600); err != nil {
			t.Fatal(err)
		}
		if role.name == "bot" {
			continue
		}
		cfg := Config{StateDir: dir, Name: "Birch-" + role.name, Password: "room", AdminPassword: "admin",
			Retention: DurableReplay, AirtimeEstimator: func(int) uint32 { return 100 }}
		mode, loop, hops := policy.PathHashMode(2), policy.LoopStrict, uint8(16)
		cfg.Policy.PathHashMode, cfg.Policy.Loop, cfg.Policy.FloodMaxHops = &mode, &loop, &hops
		radio := newWireRadio()
		id := fixedID(role.seed)
		var service *Service
		var err error
		if role.room {
			service, err = NewRoom(id, radio, cfg)
		} else {
			service, err = NewRepeater(id, radio, cfg)
		}
		if err != nil {
			t.Fatal(err)
		}
		service.mu.Lock()
		peer := fixedID(2)
		service.state.Members[peer.String()] = &member{Key: peer.PublicKey(), Permissions: 130,
			LastTimestamp: 101, SyncSince: 99, Path: []byte{0x12, 0x34, 0x56}, PathLength: 129, KnownPath: true, Attempt: 3}
		if !role.room {
			service.state.Members[peer.String()].Permissions = 3
		}
		service.state.MemberOrder = []string{peer.String()}
		service.state.Clock, service.state.Posted, service.state.Pushed = 102, 7, 3
		additional := 3
		if role.room {
			additional = 7
			for i := 0; i < 12; i++ {
				p := post{Author: fixedID(3).PublicKey(), Timestamp: uint32(81 + i), Text: fmt.Sprintf("retained %02d", i)}
				if i == 1 {
					p.Author = peer.PublicKey()
				}
				if i == 2 {
					p.Text, p.RawText = "", []byte{0xff, 0xc3, 0x61}
				}
				if i == 11 {
					p.Text = strings.Repeat("x", 151)
				}
				service.state.History = append(service.state.History, p)
			}
			service.state.Posted = 12
		}
		for i := 0; i < additional; i++ {
			admin := fixedID(byte(10 + i))
			service.state.Members[admin.String()] = &member{Key: admin.PublicKey(), Permissions: 3,
				LastTimestamp: uint32(70 + i), SyncSince: 69, Attempt: byte(251 + i%5)}
			service.state.MemberOrder = append(service.state.MemberOrder, admin.String())
		}
		service.state.Preferences.Regions = []policy.Region{{ID: 1, Name: "#fixture-a"}, {ID: 2, Name: "#fixture-b"}}
		service.state.HomeRegion, service.state.NextRegionID = 1, 3
		service.state.ManagedDefaultRegion, service.state.DefaultRegion = true, 0
		legacy := service.state.clone()
		legacy.Version = 1
		legacy.Members = make(map[string]*member)
		if role.room {
			legacy.MemberOrder = legacy.MemberOrder[:6]
			legacy.History = legacy.History[:10]
			for _, key := range legacy.MemberOrder {
				legacy.Members[key] = service.state.Members[key]
			}
		} else {
			legacy.MemberOrder = nil
		}
		stale, marshalErr := json.Marshal(legacy)
		if marshalErr != nil {
			t.Fatal(marshalErr)
		}
		if err := os.WriteFile(filepath.Join(dir, "state.v1.json"), stale, 0600); err != nil {
			t.Fatal(err)
		}
		if output := os.Getenv("MESHCORE_WILLOW_HISTORY_ORACLE"); output != "" && role.room {
			original := service.state.clone()
			random := service.random
			service.random = func(data []byte) (int, error) {
				clear(data)
				return len(data), nil
			}
			service.state.Members[peer.String()].Active = true
			service.state.Members[peer.String()].SyncSince = 0
			service.nextPush = time.Time{}
			var frames []string
			for step := 0; step < 200 && len(frames) < 11; step++ {
				packets, syncErr := service.syncAt(time.Unix(120, 0).Add(time.Duration(step) * 150 * time.Millisecond))
				if syncErr != nil {
					t.Fatal(syncErr)
				}
				for _, packet := range packets {
					raw, encodeErr := packet.ToBytes()
					if encodeErr != nil {
						t.Fatal(encodeErr)
					}
					frames = append(frames, hex.EncodeToString(raw))
					service.acknowledge(service.state.Members[peer.String()].PendingACK)
				}
			}
			if len(frames) != 11 {
				t.Fatalf("Go history oracle produced %d deliveries", len(frames))
			}
			raw, encodeErr := json.Marshal(frames)
			if encodeErr != nil {
				t.Fatal(encodeErr)
			}
			if err := os.WriteFile(output, raw, 0600); err != nil {
				t.Fatal(err)
			}
			service.state, service.random = original, random
		}
		err = service.save()
		service.mu.Unlock()
		if err != nil {
			t.Fatal(err)
		}
		if err := service.Close(); err != nil {
			t.Fatal(err)
		}
	}
}

// Opt-in: build experiments/hew-roles first. No device or socket is opened.
func TestHewRoomStateMachineDifferential(t *testing.T) {
	binary := os.Getenv("MESHCORE_HEW_BIN")
	if binary == "" {
		t.Skip("set MESHCORE_HEW_BIN to the native Hew prototype binary")
	}

	hewRoomStateMachine(t, binary)
}

func TestHewRepeaterSerializedDifferential(t *testing.T) {
	binary := os.Getenv("MESHCORE_HEW_BIN")
	if binary == "" {
		t.Skip("set MESHCORE_HEW_BIN to the native Hew prototype binary")
	}
	cfg := config(t)
	strict, maxHops, maxAdverts := policy.LoopStrict, uint8(16), uint8(8)
	cfg.Policy.Loop, cfg.Policy.FloodMaxHops, cfg.Policy.AdvertMaxHops = &strict, &maxHops, &maxAdverts
	regions := []policy.Region{{ID: 1, Name: "#ab"}}
	cfg.Policy.Regions = &regions
	digest := sha256.Sum256([]byte("#ab"))
	var regionKey [16]byte
	copy(regionKey[:], digest[:16])
	service, radio, id := startRole(t, false, cfg)
	cmd := exec.Command(binary, "serve", "2", strings.Repeat("01", 32), "Host", "room", "admin", "1", "-", "-")
	input, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	scanner := bufio.NewScanner(output)
	if !scanner.Scan() || scanner.Text() != "READY "+id.String() {
		t.Fatal("Hew startup", stderr.String())
	}
	settings := fmt.Sprintf("regions=%x\nwildcard=1\ndefault_scope=\n", regionKey)
	fmt.Fprintln(input, "signal 18") // wireRadio injects measured SNR 4.5 dB.
	if !scanner.Scan() || scanner.Text() != "END" {
		t.Fatal("Hew signal metadata")
	}
	fmt.Fprintf(input, "configure %x\n", settings)
	if !scanner.Scan() || scanner.Text() != "true" || !scanner.Scan() || scanner.Text() != "END" {
		t.Fatal("Hew regional configuration", scanner.Text())
	}
	t.Cleanup(func() {
		input.Close()
		if err := cmd.Wait(); err != nil {
			t.Error(err, stderr.String())
		}
	})
	run := func(raw []byte, local bool) [][]byte {
		t.Helper()
		flag := 0
		if local {
			flag = 1
		}
		fmt.Fprintf(input, "packet %d %d %x\n", time.Now().UnixMilli(), flag, raw)
		var packets [][]byte
		for scanner.Scan() {
			line := scanner.Text()
			if line == "END" {
				return packets
			}
			parts := strings.Fields(line)
			if len(parts) != 3 || parts[0] != "TX" {
				t.Fatal("unexpected Hew output", line)
			}
			packet, err := hex.DecodeString(parts[2])
			if err != nil {
				t.Fatal(err)
			}
			packets = append(packets, packet)
		}
		t.Fatal("Hew exited", scanner.Err(), stderr.String())
		return nil
	}
	for width := 1; width <= 3; width++ {
		encoded := byte((width - 1) << 6)
		payload := []byte{byte(width), 0x31, 0x32}
		flood := append([]byte{0x15, encoded}, payload...)
		own := bytes.Clone(id.PublicKeyBytes()[:width])
		direct := append([]byte{0x16, encoded | 2}, own...)
		direct = append(direct, bytes.Repeat([]byte{0x91}, width)...)
		direct = append(direct, byte(width), 0x41, 0x42)
		other := bytes.Clone(direct)
		other[2] ^= 0x10
		loop := append([]byte{0x15, encoded | 1}, own...)
		loop = append(loop, byte(width), 0x51, 0x52)
		local := append([]byte{0x15, encoded}, byte(width), 0x61, 0x62)
		for _, test := range []struct {
			name     string
			raw      []byte
			local    bool
			response bool
		}{
			{"flood", flood, false, true},
			{"duplicate", flood, false, false},
			{"local reflection", local, true, false},
			{"wrong direct next hop", other, false, false},
			{"direct", direct, false, true},
			{"self loop", loop, false, false},
		} {
			t.Run(fmt.Sprintf("width%d/%s", width, test.name), func(t *testing.T) {
				radio.inject(t, test.raw, test.local)
				got := run(test.raw, test.local)
				if !test.response {
					radio.quiet(t)
					if len(got) != 0 {
						t.Fatalf("Hew unexpectedly transmitted %x", got)
					}
					return
				}
				want := radio.next(t)
				if len(got) != 1 || !bytes.Equal(want, got[0]) {
					t.Fatalf("Go %x Hew %x", want, got)
				}
			})
		}
	}
	for flags := byte(0); flags < 4; flags++ {
		width := 1 << flags
		payload := []byte{flags, 0x71, 0x72, 0x73, 1, 2, 3, 4, flags}
		payload = append(payload, id.PublicKeyBytes()[:width]...)
		payload = append(payload, id.PublicKeyBytes()[:width]...)
		payload = append(payload, bytes.Repeat([]byte{0x91}, width)...)
		first := append([]byte{0x26, 0}, payload...)
		returning := append([]byte{0x26, 1, 8}, payload...)
		end := append([]byte{0x26, 3, 8, 4, 0}, payload...)
		wrong := bytes.Clone(first)
		wrong[2] ^= 0x40
		wrong[11] ^= 0x10
		for _, test := range []struct {
			name     string
			raw      []byte
			response bool
		}{{"forward", first, true}, {"duplicate", first, false}, {"return", returning, true}, {"end", end, false}, {"wrong", wrong, false}} {
			t.Run(fmt.Sprintf("trace%d/%s", width, test.name), func(t *testing.T) {
				radio.inject(t, test.raw, false)
				got := run(test.raw, false)
				if !test.response {
					radio.quiet(t)
					if len(got) != 0 {
						t.Fatalf("unexpected TRACE %x", got)
					}
				} else {
					want := radio.next(t)
					if len(got) != 1 || !bytes.Equal(want, got[0]) {
						t.Fatalf("TRACE Go %x Hew %x", want, got)
					}
				}
			})
		}
	}
	checkScoped := func(payload []byte) {
		t.Helper()
		code := policy.TransportCode(policy.Scope{Name: "#ab", Key: regionKey}, 5, payload)
		scoped := append([]byte{0x14, byte(code), byte(code >> 8), byte(code), byte(code >> 8), 128}, payload...)
		radio.inject(t, scoped, false)
		got := run(scoped, false)
		want := radio.next(t)
		if len(got) != 1 || !bytes.Equal(want, got[0]) {
			t.Fatalf("scoped Go %x Hew %x", want, got)
		}
	}
	checkScoped([]byte("scoped relay"))
	for mode := policy.LoopOff; mode <= policy.LoopStrict; mode++ {
		service.mu.Lock()
		service.state.Preferences.Loop = mode
		service.mu.Unlock()
		fmt.Fprintf(input, "configure %x\n", fmt.Sprintf("loop=%d\n", mode))
		if !scanner.Scan() || scanner.Text() != "true" || !scanner.Scan() || scanner.Text() != "END" {
			t.Fatal("Hew loop configuration")
		}
		checkScoped([]byte(fmt.Sprintf("scoped relay loop=%d", mode)))
		for width := 1; width <= 3; width++ {
			for count := 0; count <= 4; count++ {
				t.Run(fmt.Sprintf("loop%d/width%d/matches%d", mode, width, count), func(t *testing.T) {
					raw := []byte{0x15, byte((width-1)<<6 | count)}
					raw = append(raw, bytes.Repeat(id.PublicKeyBytes()[:width], count)...)
					raw = append(raw, 0xa7, byte(mode), byte(width), byte(count))
					radio.inject(t, raw, false)
					got := run(raw, false)
					thresholds := [3][3]int{{4, 2, 1}, {2, 1, 1}, {1, 1, 1}}
					if mode != policy.LoopOff && count >= thresholds[mode-1][width-1] {
						radio.quiet(t)
						if len(got) != 0 {
							t.Fatalf("Hew forwarded rejected loop %x", got)
						}
					} else {
						want := radio.next(t)
						if len(got) != 1 || !bytes.Equal(want, got[0]) {
							t.Fatalf("loop Go %x Hew %x", want, got)
						}
					}
				})
			}
		}
	}
}
func hewRoomStateMachine(t *testing.T, binary string) {
	cfg := config(t)
	cfg.Retention = NativeRetention
	s, radio, id := startRole(t, true, cfg)
	cmd := exec.Command(binary, "serve", "3", strings.Repeat("01", 32), "Host", "room", "admin", "1", "-", "-")
	input, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	scanner := bufio.NewScanner(output)
	if !scanner.Scan() || scanner.Text() != "READY "+id.String() {
		t.Fatal("Hew startup", stderr.String())
	}
	t.Cleanup(func() {
		input.Close()
		if err := cmd.Wait(); err != nil {
			t.Error(err, stderr.String())
		}
	})
	run := func(command string) []string {
		t.Helper()
		if _, err := io.WriteString(input, command+"\n"); err != nil {
			t.Fatal(err)
		}
		var lines []string
		for scanner.Scan() {
			if scanner.Text() == "END" {
				return lines
			}
			lines = append(lines, scanner.Text())
		}
		t.Fatal("Hew exited", scanner.Err(), stderr.String())
		return nil
	}
	hewPacket := func(raw []byte) [][]byte {
		t.Helper()
		var packets [][]byte
		for _, line := range run(fmt.Sprintf("packet %d 0 %x", time.Now().UnixMilli(), raw)) {
			parts := strings.Fields(line)
			if len(parts) != 3 || parts[0] != "TX" {
				t.Fatalf("unexpected Hew output %q", line)
			}
			packet, err := hex.DecodeString(parts[2])
			if err != nil {
				t.Fatal(err)
			}
			packets = append(packets, packet)
		}
		return packets
	}
	client := fixedID(2)
	for _, test := range []struct {
		name     string
		raw      []byte
		response bool
		login    bool
	}{
		{"wrong password", loginWire(t, true, client, id, 10, 0, "wrong", 2, 0, nil), false, false},
		{"authenticated login", loginWire(t, true, client, id, 10, 0, "room", 2, 0, nil), true, true},
		{"replayed login", loginWire(t, true, client, id, 10, 0, "room", 2, 0, nil), false, false},
		{"post", textWire(t, client, id, 11, 0, "hello"), true, false},
		{"retry different flags", textWire(t, client, id, 11, 1, "hello"), true, false},
		{"old post", textWire(t, client, id, 9, 0, "old"), false, false},
		{"denied administrator command", textWire(t, client, id, 12, 4, "get name"), false, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			radio.inject(t, test.raw, false)
			got := hewPacket(test.raw)
			if !test.response {
				radio.quiet(t)
				if len(got) != 0 {
					t.Fatalf("Hew unexpectedly transmitted: %x", got)
				}
				return
			}
			want := radio.next(t)
			if len(got) != 1 {
				t.Fatalf("Hew transmitted %d packets", len(got))
			}
			if test.login {
				a, b := decrypted(t, want, client, id), decrypted(t, got[0], client, id)
				if !bytes.Equal(a[4:8], b[4:8]) || a[12] != b[12] || want[0] != got[0][0] {
					t.Fatalf("login mismatch Go %x Hew %x", a, b)
				}
			} else if !bytes.Equal(want, got[0]) {
				t.Fatalf("Go %x Hew %x", want, got[0])
			}
		})
	}
	s.mu.RLock()
	wantMember := *s.state.Members[client.String()]
	wantPosts := s.state.Posted
	s.mu.RUnlock()
	status := strings.Join(run("status"), " ")
	cursor := strings.Join(run("cursor "+client.String()), " ")
	for _, field := range []string{
		"permission=" + strconv.Itoa(int(wantMember.Permissions)),
		"stamp=" + strconv.FormatUint(uint64(wantMember.LastTimestamp), 10),
		"since=" + strconv.FormatUint(uint64(wantMember.SyncSince), 10),
	} {
		if !strings.Contains(cursor, field) {
			t.Fatalf("cursor %q missing %q", cursor, field)
		}
	}
	if !strings.Contains(status, fmt.Sprintf("posted=%d", wantPosts)) {
		t.Fatal(status)
	}
	// Same multi-byte return-path update and same keepalive proof.
	path := []byte{0x81, 0x12, 0x34, 0x56, 0x0f}
	raw := peerWire(t, meshcore.PayloadTypePath, client, id, path)
	radio.inject(t, raw, false)
	if got := hewPacket(raw); len(got) != 0 {
		t.Fatal(got)
	}
	await(t, func() bool { s.mu.RLock(); defer s.mu.RUnlock(); return s.state.Members[client.String()].KnownPath })
	keep := []byte{20, 0, 0, 0, 2, 0, 0, 0, 0}
	raw = peerWire(t, meshcore.PayloadTypeReq, client, id, keep)
	radio.inject(t, raw, false)
	got := hewPacket(raw)
	want := radio.next(t)
	if len(got) != 1 || !bytes.Equal(want, got[0]) {
		t.Fatalf("keepalive Go %x Hew %x", want, got)
	}
}
