package radio

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
)

type testConnection struct {
	net.Conn
	writeMu sync.Mutex
}

func (c *testConnection) send(wire []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	_, err := c.Write(wire)
	return err
}

type testPHY struct {
	listener            net.Listener
	conns               chan *testConnection
	sent                chan []byte
	dropTX              atomic.Bool
	zeroBattery         atomic.Bool
	mu                  sync.Mutex
	active              []*testConnection
	wg                  sync.WaitGroup
	parity              bool
	jobs                chan []byte
	generation          atomic.Uint32
	configurationWrites atomic.Int32
	disconnects         atomic.Int32
	profile             []byte
	configGeneration    uint32
	configReads         atomic.Int32
	configResponse      func([]byte) []byte
	sourcePolicies      chan []byte
	statsRequests       chan struct{}
	airtimeRequests     chan []byte
	airtimeResponse     func(byte) []byte
	presenceRequests    atomic.Int32
	// presenceResponse answers ROLE_PRESENCE; nil stays silent.
	presenceResponse func(attempt int32, generation uint32) []byte
}

func newTestPHY(t *testing.T) *testPHY {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	phy := &testPHY{listener: listener, conns: make(chan *testConnection, 8), sent: make(chan []byte, 8)}
	phy.wg.Add(1)
	go func() {
		defer phy.wg.Done()
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			client := &testConnection{Conn: conn}
			phy.mu.Lock()
			phy.active = append(phy.active, client)
			phy.mu.Unlock()
			phy.conns <- client
			phy.wg.Add(1)
			go func() {
				defer phy.wg.Done()
				defer conn.Close()
				defer phy.disconnects.Add(1)
				phy.serve(client)
			}()
		}
	}()
	t.Cleanup(func() {
		listener.Close()
		phy.mu.Lock()
		for _, conn := range phy.active {
			conn.Close()
		}
		phy.mu.Unlock()
		phy.wg.Wait()
	})
	return phy
}

func (p *testPHY) serve(conn *testConnection) {
	generation := p.generation.Add(1)
	// Like the mast, reject submissions until this connection has read the
	// current configuration generation.
	var seen uint32
	var remainder []byte
	var config []byte
	var power byte
	buffer := make([]byte, 1024)
	for {
		n, err := conn.Read(buffer)
		if err != nil {
			return
		}
		frames, rest, _ := hardware.ExtractFrames(append(remainder, buffer[:n]...))
		remainder = rest
		for _, frame := range frames {
			if frame.Command == 0 {
				p.sent <- bytes.Clone(frame.Data)
				if p.dropTX.Load() {
					return
				}
				conn.send(hardware.EncodeHardwareFrame(0, 0xf8, []byte{1}))
				continue
			}
			if frame.Command != 6 || len(frame.Data) == 0 {
				continue
			}
			command, data := frame.Data[0], frame.Data[1:]
			var response []byte
			switch command {
			case hwHello:
				if !p.parity {
					response = []byte{0xf1, 5}
					break
				}
				response = make([]byte, 13)
				response[0], response[1] = 0xa0, 1
				binary.LittleEndian.PutUint32(response[3:], generation)
				p.mu.Lock()
				if len(p.profile) > 0 {
					binary.LittleEndian.PutUint32(response[7:], p.configGeneration)
				}
				p.mu.Unlock()
				response[11], response[12] = 12, data[1]
			case hwConfig:
				response = make([]byte, 25)
				response[0], response[1] = 0xa2, 1
				p.mu.Lock()
				if data[1] == 1 {
					if !bytes.Equal(p.profile, data[6:]) {
						p.configGeneration++
					}
					p.profile = bytes.Clone(data[6:])
					p.configurationWrites.Add(1)
				} else {
					p.configReads.Add(1)
				}
				if len(p.profile) > 0 {
					binary.LittleEndian.PutUint32(response[3:], p.configGeneration)
					copy(response[7:], p.profile)
					seen = p.configGeneration
				}
				if p.configResponse != nil {
					response = p.configResponse(response)
				}
				p.mu.Unlock()
			case hwSourcePolicy:
				if p.sourcePolicies != nil {
					p.sourcePolicies <- bytes.Clone(data)
				}
				response = append([]byte{0xa3, 1, 0}, data[5:]...)
			case hwPHYStats:
				if p.statsRequests != nil {
					p.statsRequests <- struct{}{}
				}
				continue
			case hwSubmit:
				if p.jobs != nil {
					p.jobs <- bytes.Clone(data)
				}
				event := make([]byte, 23)
				event[0] = 1
				copy(event[1:9], data[1:9])
				event[9] = byte(TXAccepted)
				p.mu.Lock()
				if seen != p.configGeneration {
					event[9], event[10] = byte(TXRejected), txReasonStale
				}
				p.mu.Unlock()
				response = append([]byte{hwTXEvent}, event...)
			case HWQueuedRolePresence:
				attempt := p.presenceRequests.Add(1)
				if p.presenceResponse == nil {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_NO_CALLBACK}
					break
				}
				if response = p.presenceResponse(attempt, generation); response == nil {
					continue
				}
			case 0x19:
				response = []byte{0x9a, 1}
			case 0x09:
				config = bytes.Clone(data)
				response = []byte{0xf0}
			case 0x0a:
				power = data[0]
				response = []byte{0xf0}
			case 0x0b:
				response = append([]byte{0x8b}, config...)
			case 0x0c:
				response = []byte{0x8c, power}
			case 0x17:
				response = []byte{0x97}
			case hardware.HW_CMD_GET_BATTERY:
				response = []byte{command | 0x80, 0xe4, 0x0c}
				if p.zeroBattery.Load() {
					response = []byte{command | 0x80, 0, 0}
				}
			case hardware.HW_CMD_GET_AIRTIME:
				if p.airtimeRequests != nil {
					p.airtimeRequests <- bytes.Clone(data)
				}
				if len(data) != 1 {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_LENGTH}
				} else if p.airtimeResponse == nil {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_UNKNOWN_CMD}
				} else {
					response = p.airtimeResponse(data[0])
					if response == nil {
						continue
					}
				}
			case hardware.HW_CMD_GET_NOISE_FLOOR:
				response = []byte{command | 0x80, 0x88, 0xff}
			case hardware.HW_CMD_GET_CURRENT_RSSI:
				response = []byte{command | 0x80, 0xb0}
			case hardware.HW_CMD_GET_MCU_TEMP:
				response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_NO_CALLBACK}
			case hardware.HW_CMD_GET_STATS:
				response = append([]byte{command | 0x80}, make([]byte, 12)...)
				response[1], response[5], response[9] = 9, 4, 2
			}
			conn.send(hardware.EncodeFrame(0, 6, response))
		}
	}
}

// retune changes the committed profile as an authorized mast change would.
func (p *testPHY) retune(t *testing.T, change func([]byte)) {
	t.Helper()
	p.mu.Lock()
	defer p.mu.Unlock()
	if len(p.profile) != 18 {
		t.Fatal("retune before an initial profile")
	}
	change(p.profile)
	p.configGeneration++
}

func openTestLink(t *testing.T, phy *testPHY) *Link {
	t.Helper()
	link, err := Open(context.Background(), Config{
		Address: phy.listener.Addr().String(),
		Radio:   hardware.RadioConfig{FreqHz: 910525000, BwHz: 62500, SF: 7, CR: 5},
		TxPower: 22, Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { link.Close() })
	return link
}

func TestRadioKeepsSignalsAndSuppressesLocalRetransmit(t *testing.T) {
	phy := newTestPHY(t)
	link := openTestLink(t, phy)
	radio, stop := link.Radio()
	defer stop()
	received := make(chan *meshcore.Packet, 2)
	radio.SetDataHandler(func(packet *meshcore.Packet) { received <- packet })
	conn := <-phy.conns
	for _, item := range []struct {
		meta  []byte
		local bool
		rssi  int8
		snr   float32
	}{
		{[]byte{0x80, 0x7f}, true, 127, -32},
		{[]byte{0x14, 0xc4}, false, -60, 5},
	} {
		wire := append(hardware.EncodeDataFrame([]byte{0x15, 0, 1, 2}),
			hardware.EncodeHardwareFrame(0, 0xf9, item.meta)...)
		if err := conn.send(wire); err != nil {
			t.Fatal(err)
		}
		select {
		case packet := <-received:
			if packet.IsMarkedDoNotRetransmit() != item.local ||
				!packet.HasSignalInfo || packet.RSSI != item.rssi || packet.SNR != item.snr {
				t.Fatalf("incorrect local/signal state: %+v", packet)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("packet not delivered")
		}
	}
}

func TestReconnectDoesNotReplayUncertainTransmission(t *testing.T) {
	phy := newTestPHY(t)
	link := openTestLink(t, phy)
	<-phy.conns
	phy.dropTX.Store(true)
	packet := []byte{0x15, 0, 1, 2}
	if err := link.SendData(packet); err == nil {
		t.Fatal("lost TxDone was reported as successful")
	}

	if got := <-phy.sent; !bytes.Equal(got, packet) {
		t.Fatal("wrong transmitted packet")
	}
	select {
	case <-phy.conns:
	case <-time.After(6 * time.Second):
		t.Fatal("modem was not reconnected")
	}
	deadline := time.Now().Add(2 * time.Second)
	for !link.Online() && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if !link.Online() {
		t.Fatal("reconnected modem not configured")
	}
	select {
	case <-phy.sent:
		t.Fatal("uncertain transmission was replayed")
	case <-time.After(100 * time.Millisecond):
	}
	phy.dropTX.Store(false)
	if err := link.SendData([]byte{0x15, 0, 3, 4}); err != nil {
		t.Fatal(err)
	}
}

func TestTelemetryOmitsUnsupportedBatterySentinel(t *testing.T) {
	phy := newTestPHY(t)
	phy.zeroBattery.Store(true)
	link := openTestLink(t, phy)
	snapshot, err := link.Snapshot()
	if err != nil || snapshot.HasBattery || snapshot.BatteryMilliVolts != 0 || !snapshot.HasCounters {
		t.Fatalf("unsupported battery reported as a sensor reading: %+v (%v)", snapshot, err)
	}
}

func TestTelemetryPreservesReadingsAndUnavailableFields(t *testing.T) {
	link := openTestLink(t, newTestPHY(t))
	snapshot, err := link.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	if !snapshot.HasBattery || snapshot.BatteryMilliVolts != 3300 ||
		!snapshot.HasNoiseFloor || snapshot.NoiseFloorDBm != -120 ||
		!snapshot.HasCurrentRSSI || snapshot.CurrentRSSIDBm != -80 ||
		snapshot.HasMCUTemperature || !snapshot.HasCounters ||
		snapshot.Counters != (hardware.FirmwareStats{PacketsRecv: 9, PacketsSent: 4, PacketsErrors: 2}) {
		t.Fatalf("incorrect physical telemetry: %+v", snapshot)
	}
	link.mu.Lock()
	link.sampled = time.Now().Add(-time.Minute)
	link.mu.Unlock()
	if _, err := link.Snapshot(); err == nil {
		t.Fatal("stale measurements presented as current")
	}
	link.Close()
	if _, err := link.Snapshot(); !errors.Is(err, ErrOffline) {
		t.Fatalf("offline measurements presented as current: %v", err)
	}
}

func TestResetReplacesOldFailureButIgnoresLateOldSender(t *testing.T) {
	old, current := &hardware.KissModem{}, &hardware.KissModem{}
	link := &Link{modem: current, reset: make(chan *hardware.KissModem, 1)}
	link.reset <- old
	link.requestReset(current)
	link.requestReset(old)
	if got := <-link.reset; got != current {
		t.Fatal("current failure was lost or overwritten by an obsolete connection")
	}
	link.requestReset(old)
	select {
	case <-link.reset:
		t.Fatal("late old failure requested a fresh connection reset")
	default:
	}
}
