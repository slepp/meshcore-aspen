package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync/atomic"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/app"
)

type managedTransport interface {
	client.Transport
	SetDisconnectHandler(func())
}

type endpoint struct {
	name    string
	client  *client.Client
	capture *capture
	device  protocol.DeviceInfoResponse
	self    protocol.SelfInfoResponse
	closing atomic.Bool
	lost    atomic.Bool
}

func newEndpoint(name string, transport managedTransport, cancel context.CancelCauseFunc) *endpoint {
	e := &endpoint{name: name}
	fail := func(err error) {
		if !e.closing.Load() {
			cancel(fmt.Errorf("%s connection: %w", name, err))
		}
	}
	e.capture = &capture{fail: fail}
	e.client = client.New(&observedTransport{Transport: transport, capture: e.capture})
	e.client.SetErrorHandler(func(err error) {
		e.lost.Store(true)
		fail(err)
	})
	transport.SetDisconnectHandler(func() {
		e.lost.Store(true)
		fail(errors.New("connection lost; restart the RF check"))
	})
	return e
}

func (e *endpoint) close() error {
	e.closing.Store(true)
	return e.client.Close()
}

func (e *endpoint) call(ctx context.Context, operation string, f func(context.Context) error) error {
	commandCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	if err := f(commandCtx); err != nil {
		return fmt.Errorf("%s %s: %w", e.name, operation, err)
	}
	return nil
}

func (e *endpoint) handshake(ctx context.Context) error {
	if err := e.call(ctx, "connect", e.client.Connect); err != nil {
		return err
	}
	if err := e.call(ctx, "device query", func(ctx context.Context) (err error) {
		e.device, err = e.client.DeviceQuery(ctx)
		return err
	}); err != nil {
		return err
	}
	return e.call(ctx, "app handshake", func(ctx context.Context) (err error) {
		e.self, err = e.client.AppStart(ctx, 3, "meshcore-stock-check")
		return err
	})
}

func (e *endpoint) drain(ctx context.Context) error {
	return e.call(ctx, "sync messages", func(ctx context.Context) error {
		_, err := e.client.GetWaitingMessages(ctx)
		return err
	})
}

func localAddress(address string) string {
	host, port, err := net.SplitHostPort(address)
	if err == nil && (host == "" || host == "0.0.0.0" || host == "::") {
		return net.JoinHostPort("127.0.0.1", port)
	}
	return address
}

func readIdentities(ctx context.Context, cfg app.Config, onchip bool) (map[string]meshcore.Identity, error) {
	path := "/status"
	if onchip {
		path = "/api/status"
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+localAddress(cfg.StatusListen)+path, nil)
	if err != nil {
		return nil, err
	}
	response, err := (&http.Client{Timeout: 10 * time.Second}).Do(req)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("host status: %s", response.Status)
	}
	type roleStatus struct {
		PublicKey string `json:"public_key"`
		Connected *bool  `json:"radio_connected"`
	}
	status := make(map[string]roleStatus)
	decoder := json.NewDecoder(io.LimitReader(response.Body, 1024*1024))
	if onchip {
		var snapshot struct {
			Roles []struct {
				Role      string `json:"role"`
				PublicKey string `json:"public_key"`
				Ready     bool   `json:"ready"`
			} `json:"roles"`
		}
		if err := decoder.Decode(&snapshot); err != nil {
			return nil, err
		}
		for _, entry := range snapshot.Roles {
			if _, duplicate := status[entry.Role]; duplicate {
				return nil, fmt.Errorf("duplicate on-chip role %q", entry.Role)
			}
			status[entry.Role] = roleStatus{PublicKey: entry.PublicKey, Connected: &entry.Ready}
		}
	} else {
		if err := decoder.Decode(&status); err != nil {
			return nil, err
		}
	}
	identities := make(map[string]meshcore.Identity)
	seen := make(map[[32]byte]string)
	for role, entry := range status {
		if entry.PublicKey == "" {
			continue
		}
		id, err := meshcore.NewIdentityFromHex(entry.PublicKey)
		if err != nil || id.IsZero() {
			return nil, fmt.Errorf("invalid public identity for %s", role)
		}
		if other, exists := seen[id.PublicKey()]; exists {
			return nil, fmt.Errorf("%s and %s share an identity", role, other)
		}
		seen[id.PublicKey()] = role
		identities[role] = id
	}
	for _, role := range []string{"companion", "repeater", "room"} {
		entry, exists := status[role]
		if !exists || entry.Connected == nil || !*entry.Connected || identities[role].IsZero() {
			return nil, fmt.Errorf("%s public identity/radio is not ready", role)
		}
	}
	return identities, nil
}

func radioCommand(radio hardware.RadioConfig) (protocol.SetRadioParamsCommand, error) {
	if radio.FreqHz%1000 != 0 {
		return protocol.SetRadioParamsCommand{}, errors.New("native companion frequency has kHz resolution; configured Hz cannot be represented exactly")
	}
	return protocol.SetRadioParamsCommand{
		Frequency: radio.FreqHz / 1000, Bandwidth: radio.BwHz,
		SpreadFactor: radio.SF, CodingRate: radio.CR,
	}, nil
}

func verifyRadio(info protocol.SelfInfoResponse, cfg app.Config) error {
	if info.RadioFrequency != cfg.Radio.FreqHz/1000 || info.RadioBandwidth != cfg.Radio.BwHz ||
		info.RadioSpreadFactor != cfg.Radio.SF || info.RadioCodingRate != cfg.Radio.CR ||
		info.TxPower != cfg.TxPower {
		return fmt.Errorf("radio readback differs: frequency_khz=%d bandwidth_hz=%d sf=%d cr=%d tx_dbm=%d",
			info.RadioFrequency, info.RadioBandwidth, info.RadioSpreadFactor, info.RadioCodingRate, info.TxPower)
	}
	return nil
}

func verifyStock(device protocol.DeviceInfoResponse, self protocol.SelfInfoResponse, roles map[string]meshcore.Identity) error {
	if strings.TrimPrefix(device.FirmwareVersionStr, "v") != "1.17.1" || device.FirmwareVersion != 13 ||
		device.Model == "" || strings.Contains(strings.ToLower(device.Model), "go tcp host") {
		return fmt.Errorf("this check requires MeshCore companion-v1.17.1/protocol 13; received version=%q protocol=%d model=%q",
			device.FirmwareVersionStr, device.FirmwareVersion, device.Model)
	}
	if self.Identity().IsZero() {
		return errors.New("companion radio reported a zero identity")
	}
	for role, id := range roles {
		if self.PublicKey == id.PublicKey() {
			return fmt.Errorf("companion radio shares the %s identity; use a separate MeshCore device", role)
		}
	}
	return nil
}
