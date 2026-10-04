package roles

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/policy"
)

type hewManagement struct {
	t      *testing.T
	input  io.WriteCloser
	output *bufio.Scanner
	stderr *bytes.Buffer
}

func managementHew(t *testing.T, executable string, room bool) *hewManagement {
	t.Helper()
	kind := "2"
	if room {
		kind = "3"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Minute)
	cmd := exec.CommandContext(ctx, executable, "serve", kind, strings.Repeat("01", 32), "Host", "room", "admin", "1", "-", "-")
	input, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	h := &hewManagement{t: t, input: input, output: bufio.NewScanner(output), stderr: new(bytes.Buffer)}
	cmd.Stderr = h.stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	if !h.output.Scan() || h.output.Text() != "READY "+fixedID(1).String() {
		t.Fatal("Hew startup", h.stderr.String())
	}
	t.Cleanup(func() {
		input.Close()
		if err := cmd.Wait(); err != nil {
			t.Error(err, h.stderr.String())
		}
		cancel()
	})
	return h
}

func (h *hewManagement) run(command string) []string {
	h.t.Helper()
	if _, err := fmt.Fprintln(h.input, command); err != nil {
		h.t.Fatal(err)
	}
	var out []string
	for h.output.Scan() {
		if h.output.Text() == "END" {
			return out
		}
		out = append(out, h.output.Text())
	}
	h.t.Fatal("Hew exited", h.output.Err(), h.stderr.String())
	return nil
}

func (h *hewManagement) packet(raw []byte, local bool) [][]byte {
	h.t.Helper()
	flag := 0
	if local {
		flag = 1
	}
	var packets [][]byte
	for _, line := range h.run(fmt.Sprintf("packet %d %d %x", time.Now().UnixMilli(), flag, raw)) {
		fields := strings.Fields(line)
		if len(fields) != 3 || fields[0] != "TX" {
			h.t.Fatal("Hew packet output", line)
		}
		p, err := hex.DecodeString(fields[2])
		if err != nil {
			h.t.Fatal(err)
		}
		packets = append(packets, p)
	}
	return packets
}

func managementText(t *testing.T, raw []byte, client, server meshcore.LocalIdentity) string {
	t.Helper()
	data := decrypted(t, raw, client, server)
	if len(data) < 5 || data[4] != 4 {
		t.Fatalf("not a CLI response: %x", data)
	}
	return strings.TrimRight(string(data[5:]), "\x00")
}

func TestHewDiscoverySerializedDifferential(t *testing.T) {
	executable := os.Getenv("MESHCORE_HEW_BIN")
	if executable == "" {
		t.Skip("set MESHCORE_HEW_BIN to the native Hew role binary")
	}
	cfg := config(t)
	zero := float32(0)
	cfg.Policy.TXDelay, cfg.Policy.DirectTXDelay = &zero, &zero
	s, radio, id := startRole(t, false, cfg)
	h := managementHew(t, executable, false)
	h.run("measurement 000092c20000904000")
	for index, prefix := range []byte{0x81, 0x80, 0x81, 0x80, 0x81} {
		raw := []byte{0x2e, 0, prefix, 4, byte(index + 1), 2, 3, 4}
		radio.inject(t, raw, false)
		got := h.packet(raw, false)
		if index == 4 {
			radio.quiet(t)
			if len(got) != 0 {
				t.Fatal("discovery rate limit", got)
			}
		} else {
			want := radio.next(t)
			if len(got) != 1 || !bytes.Equal(want, got[0]) {
				t.Fatalf("CONTROL Go=%x Hew=%x", want, got)
			}
		}
	}
	peer := fixedID(3)
	app, err := (&meshcore.AdvertAppData{Type: "REPEATER", Name: "Neighbour"}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	advert := &meshcore.Advert{PublicKey: peer.Identity, Timestamp: 123, RawAppData: app}
	advert.SignWith(peer)
	payload, err := advert.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	raw := append([]byte{0x12, 0}, payload...)
	radio.inject(t, raw, false)
	if got := h.packet(raw, false); len(got) != 0 {
		t.Fatal("zero-hop advert forwarded", got)
	}
	await(t, func() bool { s.mu.RLock(); defer s.mu.RUnlock(); return len(s.neighbours) == 1 })
	client := fixedID(2)
	raw = loginWire(t, false, client, id, 10, 0, "admin", 2, 0, nil)
	radio.inject(t, raw, false)
	h.packet(raw, false)
	radio.next(t)
	for index, tail := range [][]byte{
		{0, 32, 0, 0, 0, 0}, {0, 32, 0, 0, 1, 1}, {0, 32, 0, 0, 2, 6}, {0, 32, 0, 0, 3, 32},
		{0, 32, 1, 0, 0, 255}, {0, 0, 0, 0, 0, 4},
	} {
		plain := binary.LittleEndian.AppendUint32(nil, uint32(20+index))
		plain = append(plain, 6)
		plain = append(plain, tail...)
		raw = peerWire(t, meshcore.PayloadTypeReq, client, id, plain)
		radio.inject(t, raw, false)
		got := h.packet(raw, false)
		want := radio.next(t)
		if len(got) != 1 {
			t.Fatal("neighbour response count", len(got))
		}
		a, b := decrypted(t, want, client, id), decrypted(t, got[0], client, id)
		width := min(int(tail[5]), 32)
		for item := 0; item < int(binary.LittleEndian.Uint16(a[6:8])); item++ {
			at := 8 + item*(width+5) + width
			ageA, ageB := binary.LittleEndian.Uint32(a[at:]), binary.LittleEndian.Uint32(b[at:])
			if ageA > ageB+1 || ageB > ageA+1 {
				t.Fatal("neighbour age", ageA, ageB)
			}
			copy(b[at:at+4], a[at:at+4])
		}
		if !bytes.Equal(a, b) {
			t.Fatalf("neighbour pagination Go=%x Hew=%x", a, b)
		}
	}
	raw = textWire(t, client, id, 30, 4, "set owner.info operator|station")
	radio.inject(t, raw, false)
	h.packet(raw, false)
	radio.next(t)
	raw = peerWire(t, meshcore.PayloadTypeReq, client, id, []byte{31, 0, 0, 0, 7})
	radio.inject(t, raw, false)
	got := h.packet(raw, false)
	want := radio.next(t)
	if len(got) != 1 {
		t.Fatal("owner response count", len(got))
	}
	goOwner := strings.TrimRight(string(decrypted(t, want, client, id)[4:]), "\x00")
	hewOwner := strings.TrimRight(string(decrypted(t, got[0], client, id)[4:]), "\x00")
	if strings.Replace(goOwner, "host-v2-slp-birch\n", "host-v2-slp-willow\n", 1) != hewOwner {
		t.Fatalf("owner Go=%q Hew=%q", goOwner, hewOwner)
	}
	for index, query := range []byte{1, 2, 3, 3, 3} {
		width := index%3 + 1
		path := bytes.Repeat([]byte{0x55}, 2*width)
		plain := binary.LittleEndian.AppendUint32(nil, uint32(100+index))
		plain = append(plain, query, byte((width-1)<<6)|2)
		plain = append(plain, path...)
		payload := append([]byte{id.PublicKey()[0]}, fixedID(4).PublicKeyBytes()...)
		payload = append(payload, seal(t, secret(t, fixedID(4), id.Identity), plain)...)
		raw = wirePacket(7, 2, 0, nil, payload)
		radio.inject(t, raw, false)
		got = h.packet(raw, false)
		if index == 4 {
			radio.quiet(t)
			if len(got) != 0 {
				t.Fatal("anonymous query rate limit", got)
			}
			continue
		}
		want = radio.next(t)
		if len(got) != 1 || !bytes.Equal(want[:2+len(path)], got[0][:2+len(path)]) {
			t.Fatal("anonymous return path")
		}
		a, b := decrypted(t, want, fixedID(4), id), decrypted(t, got[0], fixedID(4), id)
		nowA, nowB := binary.LittleEndian.Uint32(a[4:]), binary.LittleEndian.Uint32(b[4:])
		if nowA > nowB+1 || nowB > nowA+1 {
			t.Fatal("anonymous role time", nowA, nowB)
		}
		copy(b[4:8], a[4:8])
		if !bytes.Equal(a, b) {
			t.Fatalf("anonymous %d Go=%x Hew=%x", query, a, b)
		}
	}
}

// Both services receive the same authenticated serialized packets. The Go
// service uses its normal radio/Node/application pipeline, not a mock command
// dispatcher. Identity/version text is checked separately from common replies.
func TestHewManagementSerializedDifferential(t *testing.T) {
	executable := os.Getenv("MESHCORE_HEW_BIN")
	if executable == "" {
		t.Skip("set MESHCORE_HEW_BIN to the native Hew role binary")
	}
	for _, room := range []bool{false, true} {
		t.Run(fmt.Sprintf("room=%v", room), func(t *testing.T) {
			cfg := config(t)
			zero := uint32(0)
			delay := float32(0)
			loop := policy.LoopStrict
			repeat := !room
			cfg.Policy.LocalAdvertSeconds = &zero
			cfg.Policy.FloodAdvertSeconds = &zero
			cfg.Policy.TXDelay = &delay
			cfg.Policy.DirectTXDelay = &delay
			cfg.Policy.Loop = &loop
			cfg.Policy.Repeat = &repeat
			telemetry := Telemetry{HasBattery: true, BatteryMilliVolts: 3900,
				HasNoiseFloor: true, NoiseFloorDBm: -117, HasCurrentRSSI: true, CurrentRSSIDBm: -82,
				HasMCUTemperature: true, MCUTemperatureC: -12.5, HasCounters: true,
				Counters: hardware.FirmwareStats{PacketsRecv: 997, PacketsSent: 123, PacketsErrors: 4}}
			cfg.Telemetry = func() (Telemetry, error) { return telemetry, nil }
			cfg.CurrentRadio = func() (hardware.RadioConfig, bool) {
				return hardware.RadioConfig{FreqHz: 912525000, BwHz: 250000, SF: 7, CR: 5}, true
			}
			cfg.CurrentPower = func() (uint8, bool) { return 2, true }
			s, radio, id := startRole(t, room, cfg)
			h := managementHew(t, executable, room)
			h.run("measurement 000092c20000904000") // RSSI -73, SNR 4.5, measured RF.
			h.run("telemetry 31 3900 -117 -82 000048c1 997 123 4 0 0 0")
			profile := make([]byte, 18)
			binary.LittleEndian.PutUint32(profile, 912525000)
			binary.LittleEndian.PutUint32(profile[4:], 250000)
			profile[8], profile[9], profile[10] = 7, 5, 2
			binary.LittleEndian.PutUint32(profile[11:], math.Float32bits(1))
			h.run(fmt.Sprintf("profile %x", profile))
			h.run(fmt.Sprintf("configure %x", "retention=durable-replay\n"))
			// Account for the Go startup advert already consumed by startRole.
			advert := h.run(fmt.Sprintf("advert %d", time.Now().Unix()))[0]
			startup, err := hex.DecodeString(strings.TrimPrefix(advert, "WIRE "))
			if err != nil {
				t.Fatal(err)
			}
			startup[0] = 0x12
			h.run(fmt.Sprintf("confirmed %x", startup))
			client := fixedID(2)
			login := loginWire(t, room, client, id, 10, 0, "admin", 2, 0, nil)
			radio.inject(t, login, false)
			got := h.packet(login, false)
			want := radio.next(t)
			if len(got) != 1 {
				t.Fatal("login responses", len(got))
			}
			a, b := decrypted(t, want, client, id), decrypted(t, got[0], client, id)
			if !bytes.Equal(a[4:8], b[4:8]) || a[12] != b[12] {
				t.Fatalf("login Go=%x Hew=%x", a, b)
			}
			h.run(fmt.Sprintf("confirmed %x", got[0]))
			stamp := uint32(20)
			commands := []string{
				"help", "help get", "help wifi", "help radio", "help owner", "help stats",
				"get name", "get role", "get public.key", "get guest.password",
				"get radio", "get freq", "get tx", "get af", "get dutycycle",
				"region list allowed", "region list denied",
				"stats sensors", "stats radio", "stats signal", "stats airtime",
				"set radio 900,250,7,5", "set freq 900", "set tx 20",
				"set name Renamed", "  A1|  get name", "set name bad[name",
				"set owner.info operator|station", "get owner.info",
				"set path.hash.mode 2", "get path.hash.mode", "set path.hash.mode 3",
				"set flood.max 12", "get flood.max", "set loop.detect moderate", "get loop.detect",
				"set multi.acks 1", "get multi.acks", "set allow.read.only on", "get allow.read.only",
				"set guest.password visitor", "get guest.password", "password replacement",
				"setperm " + fixedID(3).String() + " 131",
				"setperm " + fixedID(3).String()[:12] + " 0",
			}
			for _, command := range commands {
				t.Run(command, func(t *testing.T) {
					raw := textWire(t, client, id, stamp, 4, command)
					stamp++
					radio.inject(t, raw, false)
					got := h.packet(raw, false)
					want := radio.next(t)
					if len(got) != 1 {
						t.Fatalf("%q emitted %d packets", command, len(got))
					}
					a, b := managementText(t, want, client, id), managementText(t, got[0], client, id)
					if a != b {
						t.Fatalf("%q Go=%q Hew=%q", command, a, b)
					}
					h.run(fmt.Sprintf("confirmed %x", got[0]))
				})
			}
			for _, kind := range []byte{1, 3, 5} {
				t.Run("binary-"+strconv.Itoa(int(kind)), func(t *testing.T) {
					plain := binary.LittleEndian.AppendUint32(nil, stamp)
					stamp++
					plain = append(plain, kind, 0, 0)
					raw := peerWire(t, meshcore.PayloadTypeReq, client, id, plain)
					radio.inject(t, raw, false)
					got := h.packet(raw, false)
					want := radio.next(t)
					if len(got) != 1 {
						t.Fatal("binary response count", len(got))
					}
					a, b := decrypted(t, want, client, id), decrypted(t, got[0], client, id)
					if kind == 1 {
						// Different process start instants may cross a whole-second boundary.
						ga, hb := binary.LittleEndian.Uint32(a[24:]), binary.LittleEndian.Uint32(b[24:])
						if ga > hb+2 || hb > ga+2 {
							t.Fatalf("uptime Go=%d Hew=%d", ga, hb)
						}
						copy(b[24:28], a[24:28])
					}
					if !bytes.Equal(a, b) {
						t.Fatalf("REQ %d Go=%x Hew=%x", kind, a, b)
					}
					h.run(fmt.Sprintf("confirmed %x", got[0]))
				})
			}
			path := filepath.Join(t.TempDir(), "role.state")
			if out := h.run("save " + path); len(out) != 1 || out[0] != "true" {
				t.Fatal("save", out)
			}
			state, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if string(state[:4]) != "HEW5" && string(state[:4]) != "HEW6" {
				t.Fatal("remote preferences not in committed state")
			}
			restarted := managementHew(t, executable, room)
			if out := restarted.run("load " + path); len(out) != 1 || out[0] != "true" {
				t.Fatal("load", out)
			}
			raw := textWire(t, client, id, stamp, 4, "get name")
			got = restarted.packet(raw, false)
			if len(got) != 1 || managementText(t, got[0], client, id) != "> Renamed" {
				t.Fatal("durable remote name")
			}
			s.mu.RLock()
			if s.state.Name != "Renamed" || s.state.Members[client.String()].Permissions != 3 {
				t.Error("Go state mismatch")
			}
			s.mu.RUnlock()
		})
	}
}
