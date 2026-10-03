package roles

import (
	"encoding/binary"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
)

func statsNeedsTelemetry(command string) bool {
	_, command = splitCommand(command)
	parts := strings.Fields(command)
	if len(parts) != 2 || parts[0] != "stats" {
		return false
	}
	switch parts[1] {
	case "sensors", "radio", "signal", "airtime":
		return true
	}
	return false
}

func (s *Service) statsCommand(topic string, telemetry Telemetry, telemetryErr error) string {
	topic = strings.TrimSpace(topic)
	if topic == "help" {
		return s.commandHelp("stats")
	}
	if topic == "" || topic == "role" {
		return fmt.Sprintf("schema=1 scope=role uptime_s=%d rx_packets=%d tx_packets=%d",
			int64(time.Since(s.started)/time.Second), s.received.Load(), s.sent.Load())
	}
	if !statsNeedsTelemetry("stats " + topic) {
		return "Error: unknown stats topic; use stats help"
	}
	if telemetryErr != nil || s.cfg.Telemetry == nil {
		return "Error: modem statistics snapshot unavailable"
	}
	fields := []string{"schema=1", "scope=modem"}
	value := func(name string, present bool, measurement string) {
		if !present {
			measurement = "unavailable"
		}
		fields = append(fields, name+"="+measurement)
	}
	switch topic {
	case "sensors":
		if telemetry.HasMCUTemperature && (math.IsNaN(float64(telemetry.MCUTemperatureC)) ||
			math.IsInf(float64(telemetry.MCUTemperatureC), 0)) {
			return "Error: modem MCU temperature is not finite"
		}
		value("battery_mv", telemetry.HasBattery, strconv.Itoa(int(telemetry.BatteryMilliVolts)))
		value("mcu_temp_c", telemetry.HasMCUTemperature,
			strconv.FormatFloat(float64(telemetry.MCUTemperatureC), 'f', 2, 32))
	case "radio":
		value("rx_packets", telemetry.HasCounters, strconv.FormatUint(uint64(telemetry.Counters.PacketsRecv), 10))
		value("tx_packets", telemetry.HasCounters, strconv.FormatUint(uint64(telemetry.Counters.PacketsSent), 10))
		value("rx_errors", telemetry.HasCounters, strconv.FormatUint(uint64(telemetry.Counters.PacketsErrors), 10))
	case "signal":
		value("current_rssi_dbm", telemetry.HasCurrentRSSI, strconv.Itoa(int(telemetry.CurrentRSSIDBm)))
		value("noise_floor_dbm", telemetry.HasNoiseFloor && telemetry.NoiseFloorDBm != 0,
			strconv.Itoa(int(telemetry.NoiseFloorDBm)))
	case "airtime":
		value("tx_rf_ms", telemetry.HasAirtime || telemetry.HasTXAirtime, strconv.FormatInt(telemetry.TXAirtime.Milliseconds(), 10))
		value("rx_rf_ms", telemetry.HasAirtime || telemetry.HasRXAirtime, strconv.FormatInt(telemetry.RXAirtime.Milliseconds(), 10))
	}
	reply := strings.Join(fields, " ")
	if len(reply) > 130 {
		return "Error: stats response exceeds encrypted CLI capacity"
	}
	return reply
}

// Telemetry is a value snapshot of physical modem readings. A Has field means
// the associated reading is current and valid, including when its value is zero.
// The provider must clear validity for unavailable or stale cached measurements;
// refreshing the cache and reporting its acquisition errors belong to the caller.
// Counters cover the physical modem, not just this logical role.
type Telemetry struct {
	BatteryMilliVolts uint16
	HasBattery        bool
	NoiseFloorDBm     int16
	HasNoiseFloor     bool
	CurrentRSSIDBm    int8
	HasCurrentRSSI    bool
	MCUTemperatureC   float32
	HasMCUTemperature bool
	Counters          hardware.FirmwareStats
	HasCounters       bool
	// Airtime is measured RF occupancy, excluding queue/network/host waits.
	TXAirtime    time.Duration
	RXAirtime    time.Duration
	HasAirtime   bool
	HasTXAirtime bool
	HasRXAirtime bool
	// ErrorEvents retains the native err_events field name: this is a sticky
	// ERR_EVENT_* bitmask (packet-pool full, CAD timeout, RX-start timeout),
	// not a count of rejected/failed/unknown transmissions.
	ErrorEvents    uint16
	HasErrorEvents bool
}

func telemetryPayload(t Telemetry) ([]byte, error) {
	if !t.HasBattery && !t.HasMCUTemperature {
		return nil, fmt.Errorf("%w: no valid modem sensor measurements", ErrUnsupported)
	}
	var data []byte
	if t.HasBattery {
		// Native Cayenne performs its scaling in float32 before truncation.
		volts := float32(t.BatteryMilliVolts) / 1000
		data = append(data, 1, meshcore.LPPVoltage)
		data = binary.BigEndian.AppendUint16(data, uint16(volts*100))
	}
	if t.HasMCUTemperature {
		scaled := math.Trunc(float64(t.MCUTemperatureC * 10))
		if math.IsNaN(scaled) || math.IsInf(scaled, 0) || scaled < -32768 || scaled > 32767 {
			return nil, fmt.Errorf("invalid modem temperature: %g", t.MCUTemperatureC)
		}
		data = append(data, 1, meshcore.LPPTemperature)
		data = binary.BigEndian.AppendUint16(data, uint16(int16(scaled)))
	}
	return data, nil
}
