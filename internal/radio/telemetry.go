package radio

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/meshcore-go/meshcore-go/hardware"
)

type Telemetry struct {
	BatteryMilliVolts uint16                 `json:"battery_mv"`
	HasBattery        bool                   `json:"has_battery"`
	NoiseFloorDBm     int16                  `json:"noise_floor_dbm"`
	HasNoiseFloor     bool                   `json:"has_noise_floor"`
	CurrentRSSIDBm    int8                   `json:"current_rssi_dbm"`
	HasCurrentRSSI    bool                   `json:"has_current_rssi"`
	MCUTemperatureC   float32                `json:"mcu_temperature_c"`
	HasMCUTemperature bool                   `json:"has_mcu_temperature"`
	Counters          hardware.FirmwareStats `json:"counters"`
	HasCounters       bool                   `json:"has_counters"`
}

// Snapshot never blocks radio ingress or returns stale readings after a disconnect.
func (l *Link) Snapshot() (Telemetry, error) {
	l.mu.RLock()
	defer l.mu.RUnlock()
	if l.modem == nil {
		return Telemetry{}, ErrOffline
	}
	if l.sampled.IsZero() || time.Since(l.sampled) > 45*time.Second {
		return Telemetry{}, errors.New("physical modem telemetry is stale")
	}
	return l.telemetry, nil
}

func (l *Link) sampleTelemetry(ctx context.Context, modem *hardware.KissModem) error {
	var sample Telemetry
	for _, query := range []struct {
		name string
		read func() error
	}{
		{"battery", func() (err error) {
			sample.BatteryMilliVolts, err = modem.Battery(ctx)
			sample.HasBattery = err == nil && sample.BatteryMilliVolts != 0
			return err
		}},
		{"noise floor", func() (err error) {
			sample.NoiseFloorDBm, err = modem.NoiseFloor(ctx)
			sample.HasNoiseFloor = err == nil
			return err
		}},
		{"current RSSI", func() (err error) {
			sample.CurrentRSSIDBm, err = modem.CurrentRSSI(ctx)
			sample.HasCurrentRSSI = err == nil
			return err
		}},
		{"MCU temperature", func() (err error) {
			sample.MCUTemperatureC, err = modem.MCUTemp(ctx)
			sample.HasMCUTemperature = err == nil
			return err
		}},
		{"packet counters", func() (err error) {
			sample.Counters, err = modem.FirmwareCounters(ctx)
			sample.HasCounters = err == nil
			return err
		}},
	} {
		if err := query.read(); err != nil {
			if errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_NO_CALLBACK)) ||
				errors.Is(err, hardware.HwErrorFor(hardware.HW_ERR_UNKNOWN_CMD)) {
				l.config.Logger.Debug("optional modem telemetry unavailable", "reading", query.name, "error", err)
				continue
			}
			// A late uncorrelated reply must not satisfy a later request.
			return fmt.Errorf("%s telemetry: %w", query.name, err)
		}
	}
	l.mu.Lock()
	l.telemetry, l.sampled = sample, time.Now()
	l.mu.Unlock()
	return nil
}
