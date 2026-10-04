// meshcore-rf-check exercises a running host through a separate physical radio.
// It does not flash firmware, read private host identities or emulate RF with
// software fanout. Room membership for the independent probe survives the run:
// the room protocol has no unauthenticated membership-removal operation.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/app"
)

type options struct {
	config         string
	peer           string
	timeout        time.Duration
	allowUnsafePHY bool
}

func main() {
	var opts options
	flag.StringVar(&opts.config, "config", "", "required host JSON configuration")
	flag.StringVar(&opts.peer, "peer", "", "required secondary physical KISS TCP address")
	flag.DurationVar(&opts.timeout, "timeout", 150*time.Second, "total deadline including cleanup (20s..180s)")
	flag.BoolVar(&opts.allowUnsafePHY, "allow-unsafe-phy", false, "explicitly allow tuning other than 912.525 MHz/BW250k/SF7/CR5/TX<=2 dBm")
	flag.Parse()
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()
	if err := execute(ctx, opts, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "FAIL", err)
		os.Exit(1)
	}
}

func execute(parent context.Context, opts options, output io.Writer) error {
	if opts.config == "" || opts.peer == "" {
		return errors.New("-config and -peer are required; no active-radio defaults are used")
	}
	cfg, err := app.LoadConfig(opts.config)
	if err != nil {
		return fmt.Errorf("configuration: %w", err)
	}
	if err := validateOptions(cfg, opts); err != nil {
		return err
	}
	end := time.Now().Add(opts.timeout)
	ctx, cancel := context.WithDeadline(parent, end.Add(-10*time.Second))
	defer cancel()
	if err := distinctRadios(ctx, cfg.RadioAddress, opts.peer); err != nil {
		return err
	}
	ids, err := loadIdentities(ctx, cfg.StatusListen)
	if err != nil {
		return fmt.Errorf("host status: %w", err)
	}
	password := cfg.RoomPassword
	if cfg.RoomPasswordEnv != "" {
		var ok bool
		password, ok = os.LookupEnv(cfg.RoomPasswordEnv)
		if !ok || password == "" {
			return errors.New("configured room password environment variable is unset or empty")
		}
	}
	if len(password) > 15 {
		return errors.New("room password exceeds the standard companion client's 15-byte login field")
	}
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	check := checker{cfg: cfg, opts: opts, ids: ids, password: password, output: output, logger: logger, end: end}
	return check.run(ctx)
}

func validateOptions(cfg app.Config, opts options) error {
	if opts.timeout < 20*time.Second || opts.timeout > 180*time.Second {
		return errors.New("-timeout must be between 20s and 180s")
	}
	if _, _, err := net.SplitHostPort(opts.peer); err != nil {
		return fmt.Errorf("secondary address: %w", err)
	}
	if !opts.allowUnsafePHY && (cfg.Radio.FreqHz != 912525000 || cfg.Radio.BwHz != 250000 ||
		cfg.Radio.SF != 7 || cfg.Radio.CR != 5 || cfg.TxPower > 2) {
		return errors.New("refusing active/default PHY: require 912525000 Hz, BW250000, SF7, CR5, TX<=2 or explicit -allow-unsafe-phy")
	}
	return nil
}

func distinctRadios(ctx context.Context, primary, peer string) error {
	primaryHost, _, err := net.SplitHostPort(primary)
	if err != nil {
		return err
	}
	peerHost, _, err := net.SplitHostPort(peer)
	if err != nil {
		return err
	}
	if strings.EqualFold(primaryHost, peerHost) {
		return errors.New("primary and secondary must be different physical radio hosts, not merely different ports")
	}
	a, err := net.DefaultResolver.LookupIPAddr(ctx, primaryHost)
	if err != nil {
		return fmt.Errorf("resolve primary: %w", err)
	}
	b, err := net.DefaultResolver.LookupIPAddr(ctx, peerHost)
	if err != nil {
		return fmt.Errorf("resolve secondary: %w", err)
	}
	for _, x := range a {
		for _, y := range b {
			if x.IP.Equal(y.IP) {
				return errors.New("primary and secondary resolve to the same host")
			}
		}
	}
	if len(a) == 0 || len(b) == 0 {
		return errors.New("radio address has no resolved IP")
	}
	return nil
}

func dialAddress(address string) string {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return address
	}
	ip := net.ParseIP(host)
	if host == "" || ip != nil && ip.IsUnspecified() {
		host = "127.0.0.1"
	}
	return net.JoinHostPort(host, port)
}

type identities struct{ base, room, repeater, observer meshcore.Identity }

func loadIdentities(ctx context.Context, address string) (identities, error) {
	var ids identities
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+dialAddress(address)+"/status", nil)
	if err != nil {
		return ids, err
	}
	response, err := (&http.Client{Timeout: 5 * time.Second}).Do(request)
	if err != nil {
		return ids, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return ids, fmt.Errorf("HTTP %d", response.StatusCode)
	}
	var roles map[string]struct {
		PublicKey      string `json:"public_key"`
		RadioConnected *bool  `json:"radio_connected"`
	}
	if err := json.NewDecoder(io.LimitReader(response.Body, 65536)).Decode(&roles); err != nil {
		return ids, err
	}
	seen := make(map[[32]byte]bool)
	for name, dest := range map[string]*meshcore.Identity{"companion": &ids.base, "room": &ids.room, "repeater": &ids.repeater, "observer": &ids.observer} {
		entry, ok := roles[name]
		if !ok || entry.RadioConnected == nil || !*entry.RadioConnected {
			return ids, fmt.Errorf("%s radio missing or offline", name)
		}
		id, err := meshcore.NewIdentityFromHex(entry.PublicKey)
		if err != nil || id.IsZero() {
			return ids, fmt.Errorf("%s public identity invalid", name)
		}
		if seen[id.PublicKey()] {
			return ids, errors.New("host roles do not have independent identities")
		}
		seen[id.PublicKey()] = true
		*dest = id
	}
	return ids, nil
}
