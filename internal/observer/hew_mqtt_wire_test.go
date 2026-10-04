package observer

import (
	"bytes"
	"context"
	"encoding/hex"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/eclipse/paho.mqtt.golang/packets"
)

func runHewMQTT(t *testing.T, args ...string) []byte {
	t.Helper()
	binary := os.Getenv("MESHCORE_HEW_MQTT_BIN")
	if binary == "" {
		t.Skip("set MESHCORE_HEW_MQTT_BIN to the host-only native MQTT checks binary")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, binary, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("native MQTT checks failed: %v\n%s", err, out)
	}
	return out
}

func TestHewMQTTCodecBoundsAndFragmentation(t *testing.T) {
	if out := runHewMQTT(t); string(out) != "MQTT_CODEC_CHECKS_OK\n" {
		t.Fatalf("unexpected native check output: %q", out)
	}
}

// Paho is the existing Go observer's MQTT implementation. This compares actual
// serialized bytes, without creating a broker or making any network connection.
func TestHewMQTTWireDifferential(t *testing.T) {
	var golden [][]byte
	add := func(packet packets.ControlPacket) {
		t.Helper()
		var encoded bytes.Buffer
		if err := packet.Write(&encoded); err != nil {
			t.Fatal(err)
		}
		golden = append(golden, encoded.Bytes())
	}
	connect := func(id, username string, password []byte, topic string, keepalive uint16) {
		t.Helper()
		p := packets.NewControlPacket(packets.Connect).(*packets.ConnectPacket)
		p.ProtocolName, p.ProtocolVersion = "MQTT", 4
		p.CleanSession, p.WillFlag, p.WillQos, p.WillRetain = true, true, 1, true
		p.ClientIdentifier, p.Username, p.Password = id, username, password
		p.UsernameFlag, p.PasswordFlag = username != "", len(password) != 0
		p.WillTopic, p.WillMessage, p.Keepalive = topic, []byte("offline"), keepalive
		add(p)
	}
	connect("meshcore-observer-010203040506", "", nil, "meshcore/test/status", 10)
	connect("meshcore-observer-010203040506", "reader", []byte("test-password"), "meshcore/test/status", 10)
	connect("観測者", "üser", []byte{0, 1, 2, 255}, "meshcore/日本/status", 65535)
	for _, size := range []int{0, 1, 125, 126, 127, 128, 16370, 16383, 16384, 65535} {
		for _, retained := range []bool{false, true} {
			p := packets.NewControlPacket(packets.Publish).(*packets.PublishPacket)
			p.TopicName, p.Qos, p.Retain = "meshcore/test/packets", 1, retained
			p.MessageID = uint16(min(size+1, 65535))
			p.Payload = make([]byte, size)
			for i := range p.Payload {
				p.Payload[i] = byte(i % 251)
			}
			add(p)
		}
	}
	add(packets.NewControlPacket(packets.Pingreq))
	add(packets.NewControlPacket(packets.Disconnect))

	lines := strings.Split(strings.TrimSpace(string(runHewMQTT(t, "vectors"))), "\n")
	if len(lines) != len(golden) {
		t.Fatalf("native frames=%d, Go frames=%d", len(lines), len(golden))
	}
	for i, line := range lines {
		got, err := hex.DecodeString(line)
		if err != nil {
			t.Fatalf("frame%d: %v", i, err)
		}
		if !bytes.Equal(got, golden[i]) {
			t.Fatalf("frame%d differs: native bytes=%d, Go bytes=%d", i, len(got), len(golden[i]))
		}
		remaining := bytes.NewReader(got)
		if _, err := packets.ReadPacket(remaining); err != nil || remaining.Len() != 0 {
			t.Fatalf("frame%d rejected by Go decoder: %v, trailing=%d", i, err, remaining.Len())
		}
	}
	t.Logf("%d native CONNECT/PUBLISH/PINGREQ/DISCONNECT frames match Paho", len(golden))
}
