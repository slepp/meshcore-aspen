package observer

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"os"
	"os/exec"
	"reflect"
	"strings"
	"testing"
	"time"
)

func runHewObservation(t *testing.T, args ...string) []byte {
	t.Helper()
	binary := os.Getenv("MESHCORE_HEW_OBSERVATION_BIN")
	if binary == "" {
		t.Skip("set MESHCORE_HEW_OBSERVATION_BIN to the native observation checks binary")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, binary, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("native observation checks failed: %v\n%s", err, out)
	}
	return out
}

func TestHewObservationBoundsAndAdmission(t *testing.T) {
	if out := runHewObservation(t); string(out) != "OBSERVATION_CHECKS_OK\n" {
		t.Fatalf("unexpected native check output: %q", out)
	}
}

// Exercise the actual Go observer encoders without starting their publisher.
func TestHewObservationDifferential(t *testing.T) {
	var golden []any
	stamp := time.UnixMilli(1735689600120).UTC()
	observerFor := func(format string) *Observer {
		t.Helper()
		o, err := New(strings.Repeat("a1", 32), Config{
			Format: format, TopicPrefix: "/meshcore/", IATA: "YYC", Origin: `Observer "test"`,
		})
		if err != nil {
			t.Fatal(err)
		}

		o.session = "fixture"
		return o
	}
	itemFor := func(raw []byte) observation {
		return observation{data: raw, timestamp: stamp, sequence: 42,
			hasSignal: true, snr: 10.5, rssi: -90}
	}
	decode := func(data []byte) any {
		t.Helper()
		var value any
		if err := json.Unmarshal(data, &value); err != nil {
			t.Fatal(err)
		}
		return value
	}
	add := func(o *Observer, item observation) {
		t.Helper()
		var data []byte
		var err error
		if publicFormat(o.cfg.Format) {
			data, err = o.publicPacket(item)
		} else {
			data, err = json.Marshal(o.event(item))
		}
		if err != nil {
			golden = append(golden, map[string]any{"error": err.Error()})
		} else {
			golden = append(golden, decode(data))
		}
	}
	rawHex := func(text string) []byte {
		t.Helper()
		raw, err := hex.DecodeString(text)
		if err != nil {
			t.Fatal(err)
		}
		return raw
	}

	internal := observerFor("internal-v1")
	golden = append(golden, internal.topic, "online")
	for header := range 256 {
		add(internal, itemFor([]byte{byte(header), 0, 0, 0, 0, 0, 1}))
	}
	for _, encoded := range []string{"", "00", "0000", "000000", "00000000",
		"0000000000", "01", "0104aa", "01c0", "0181aabbcc0102"} {
		add(internal, itemFor(rawHex(encoded)))
	}
	add(internal, itemFor(append([]byte{1, 0}, bytes.Repeat([]byte{1}, 185)...)))
	unusual := itemFor(rawHex("150001"))
	unusual.timestamp = time.UnixMilli(-1).UTC()
	add(internal, unusual)
	unusual.timestamp, unusual.hasSignal = stamp, false
	add(internal, unusual)
	unusual.hasSignal, unusual.snr, unusual.rssi = true, -32, 127
	add(internal, unusual)

	for _, format := range []string{PublicFormat, CaptureFormat} {
		o := observerFor(format)
		golden = append(golden, o.topic, decode([]byte(o.publicStatus("online", stamp))))
		for route := range 4 {
			for width := 1; width <= 3; width++ {
				for _, count := range []int{0, 2} {
					raw := []byte{byte(20 | route)}
					if route == 0 || route == 3 {
						raw = append(raw, 1, 0, 2, 0)
					}
					raw = append(raw, byte((width-1)<<6|count))
					raw = append(raw, bytes.Repeat([]byte{165}, width*count)...)
					raw = append(raw, 1, 2, 3)
					add(o, itemFor(raw))
				}
			}
		}
		add(o, itemFor(rawHex("2581aabbcc0102")))
		for quarter := -128; quarter < 128; quarter++ {
			item := itemFor(rawHex("150001"))
			item.snr = float32(quarter) / 4
			add(o, item)
		}
		rejected := itemFor(rawHex("150001"))
		rejected.hasSignal = false
		add(o, rejected)
		rejected.hasSignal, rejected.snr, rejected.rssi = true, -32, 127
		add(o, rejected)
		rejected = itemFor(rawHex("150001"))
		rejected.timestamp = time.UnixMilli(0).UTC()
		add(o, rejected)
		var filter uint16
		o.cfg.PacketFilter = &filter
		add(o, itemFor(rawHex("150001")))
	}

	lines := strings.Split(strings.TrimSpace(string(runHewObservation(t, "vectors"))), "\n")
	if len(lines) != len(golden) {
		t.Fatalf("native rows=%d, Go rows=%d", len(lines), len(golden))
	}
	for i, line := range lines {
		got := decode([]byte(line))
		if !reflect.DeepEqual(got, golden[i]) {
			want, _ := json.Marshal(golden[i])
			t.Fatalf("observation%d differs:\nnative=%s\nGo=%s", i, line, want)
		}
	}
	t.Logf("%d native topic/status/packet observations match the actual Go observer", len(golden))
}
