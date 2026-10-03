package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"flag"
	"math"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/app"
)

func TestOnchipIdentityDiscoveryRequiresDistinctReadyRoles(t *testing.T) {
	one := strings.Repeat("01", 32)
	two := strings.Repeat("02", 32)
	three := strings.Repeat("03", 32)
	body := `{"roles":[
		{"role":"repeater","public_key":"` + one + `","ready":true},
		{"role":"room","public_key":"` + two + `","ready":true},
		{"role":"companion","public_key":"` + three + `","ready":true}
	]}`
	for _, test := range []struct {
		name string
		body string
		ok   bool
	}{
		{"ready", body, true},
		{"offline", strings.Replace(body, `"ready":true`, `"ready":false`, 1), false},
		{"shared key", strings.Replace(body, two, one, 1), false},
		{"duplicate role", strings.Replace(body, `"role":"room"`, `"role":"repeater"`, 1), false},
		{"missing key", strings.Replace(body, two, "", 1), false},
	} {
		t.Run(test.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != "/api/status" {
					t.Errorf("requested wrong status endpoint: %s", r.URL.Path)
				}
				w.Header().Set("Content-Type", "application/json")
				_, _ = w.Write([]byte(test.body))
			}))
			defer server.Close()
			cfg := app.DefaultConfig()
			cfg.StatusListen = strings.TrimPrefix(server.URL, "http://")
			identities, err := readIdentities(context.Background(), cfg, true)
			if (err == nil) != test.ok {
				t.Fatalf("identity discovery: %v", err)
			}
			if test.ok && identities["room"].String() != two {
				t.Fatal("room discovery returned the wrong role identity")
			}
		})
	}
}

func TestNativeRadioWireUnitsAndReadback(t *testing.T) {
	cfg := app.DefaultConfig()
	cfg.Radio.FreqHz, cfg.Radio.BwHz, cfg.TxPower = 912525000, 250000, 2
	cmd, err := radioCommand(cfg.Radio)
	if err != nil {
		t.Fatal(err)
	}
	wire := cmd.ToBytes()
	if len(wire) != 11 || wire[0] != 11 ||
		binary.LittleEndian.Uint32(wire[1:5]) != 912525 ||
		binary.LittleEndian.Uint32(wire[5:9]) != 250000 || wire[9] != 7 || wire[10] != 5 {
		t.Fatalf("native CMD11 must use kHz/Hz/SF/CR: %x", wire)
	}
	info := protocol.SelfInfoResponse{RadioFrequency: 912525, RadioBandwidth: 250000, RadioSpreadFactor: 7, RadioCodingRate: 5, TxPower: 2}
	if err := verifyRadio(info, cfg); err != nil {
		t.Fatal(err)
	}
	info.RadioFrequency = 912525000
	if err := verifyRadio(info, cfg); err == nil {
		t.Fatal("accepted Hz in the native kHz readback field")
	}
	info.RadioFrequency, info.RadioBandwidth = 912525, 250
	if err := verifyRadio(info, cfg); err == nil {
		t.Fatal("accepted kHz in the native Hz bandwidth field")
	}
	cfg.Radio.FreqHz++
	if _, err := radioCommand(cfg.Radio); err == nil {
		t.Fatal("silently truncated an unrepresentable frequency")
	}
}

func TestStockDeviceMustBePinnedAndIndependent(t *testing.T) {
	device := protocol.DeviceInfoResponse{FirmwareVersion: 13, FirmwareVersionStr: "v1.17.1", Model: "Seeed XIAO"}
	self := protocol.SelfInfoResponse{PublicKey: [32]byte{1}}
	roles := map[string]meshcore.Identity{"companion": meshcore.NewIdentity([32]byte{2})}
	if err := verifyStock(device, self, roles); err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*protocol.DeviceInfoResponse, *protocol.SelfInfoResponse){
		func(d *protocol.DeviceInfoResponse, _ *protocol.SelfInfoResponse) { d.FirmwareVersionStr = "v1.17.2" },
		func(d *protocol.DeviceInfoResponse, _ *protocol.SelfInfoResponse) { d.FirmwareVersion = 10 },
		func(d *protocol.DeviceInfoResponse, _ *protocol.SelfInfoResponse) { d.Model = "MeshCore Go TCP host" },
		func(_ *protocol.DeviceInfoResponse, s *protocol.SelfInfoResponse) { s.PublicKey = [32]byte{} },
		func(_ *protocol.DeviceInfoResponse, s *protocol.SelfInfoResponse) {
			s.PublicKey = roles["companion"].PublicKey()
		},
	} {
		d, s := device, self
		mutate(&d, &s)
		if err := verifyStock(d, s, roles); err == nil {
			t.Fatalf("accepted non-independent or wrong-version device: %+v", d)
		}
	}
}

func TestRXEvidenceRejectsReflectionsAndUnmeasuredSignals(t *testing.T) {
	pkt := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeAck, 0), Payload: []byte{1, 2, 3, 4}}
	raw, err := pkt.ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	c := &capture{fail: func(err error) { t.Error(err) }}
	for _, signal := range []struct {
		snr  float32
		rssi int8
	}{
		{-32, 127}, {0, 0}, {float32(math.NaN()), -80},
	} {
		c.observe(protocol.Response{Code: protocol.PushLogRxData, Data: protocol.PushLogRxDataResponse{Raw: raw, LastSNR: signal.snr, LastRSSI: signal.rssi}})
	}
	if c.mark() != 0 || c.rejected != 3 {
		t.Fatal("local/unmeasured receptions became RF evidence")
	}
	c.observe(protocol.Response{Code: protocol.PushLogRxData, Data: protocol.PushLogRxDataResponse{Raw: raw, LastSNR: 0, LastRSSI: -80}})
	rx := ackRX(c.since(0), 0x04030201)
	if rx == nil || rx.rssi != -80 || rx.snr != 0 {
		t.Fatal("measured zero-dB SNR was confused with missing metadata")
	}
}

func TestSignedRoomACKUsesRecipientAndNativeAttemptBits(t *testing.T) {
	recipient := meshcore.NewIdentity([32]byte{1})
	author := meshcore.NewIdentity([32]byte{9, 8, 7, 6})
	msg := protocol.ContactMsgRecvV3Response{
		TxtType: protocol.TxtTypeSignedPlain, SenderTimestamp: 1234,
		SenderPrefix: []byte{9, 8, 7, 6}, Text: "rf-room",
	}
	tags, err := signedACKs(msg, recipient)
	if err != nil {
		t.Fatal(err)
	}
	plain := []byte{0xd2, 4, 0, 0, 0x0a, 9, 8, 7, 6, 'r', 'f', '-', 'r', 'o', 'o', 'm'}
	digest := sha256.Sum256(append(plain, recipient.PublicKeyBytes()...))
	want := binary.LittleEndian.Uint32(digest[:4])
	authorTags, err := signedACKs(msg, author)
	if err != nil {
		t.Fatal(err)
	}
	if tags[2] != want || tags[2] == authorTags[2] {
		t.Fatal("room ACK proof used the author key or lost native attempt bits")
	}
	body, err := (&meshcore.MultiPart{Remaining: 1, WrappedType: meshcore.PayloadTypeAck, WrappedPayload: binary.LittleEndian.AppendUint32(nil, want)}).ToBytes()
	if err != nil {
		t.Fatal(err)
	}
	pkt := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeMultiPart, 0), Payload: body}
	if got, ok := packetACK(pkt); !ok || got != want {
		t.Fatal("multipart ACK tag was not decoded")
	}
	pkt.Header = meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypePath, 0)
	pkt.Payload = binary.LittleEndian.AppendUint32(nil, want)
	if _, ok := packetACK(pkt); ok {
		t.Fatal("opaque PATH payload was treated as a decoded ACK")
	}
	msg.SenderPrefix = nil
	if _, err := signedACKs(msg, recipient); err == nil {
		t.Fatal("signed message without an author prefix became an ACK proof")
	}
}

func TestCLIRequiresExplicitTargetsAndBoundedDeadlines(t *testing.T) {
	args := []string{"-config", "test-config.json", "-serial", "/dev/test-stock"}
	for _, bad := range [][]string{
		nil, {"-config", "test-config.json"}, {"-serial", "/dev/test-stock"},
		append(append([]string(nil), args...), "-timeout", "0s"),
		append(append([]string(nil), args...), "-phase-timeout", "20m"),
	} {
		if _, err := parseOptions(bad, &bytes.Buffer{}); err == nil {
			t.Fatalf("accepted unsafe/incomplete arguments: %v", bad)
		}
	}
	o, err := parseOptions(args, &bytes.Buffer{})
	if err != nil || o.name != "MeshCore Test Peer" || o.phaseTimeout != 3*time.Minute {
		t.Fatalf("options: %+v, %v", o, err)
	}
	if _, err := parseOptions([]string{"-help"}, &bytes.Buffer{}); err != flag.ErrHelp {
		t.Fatalf("help should terminate before reading configuration or opening devices: %v", err)
	}
}
