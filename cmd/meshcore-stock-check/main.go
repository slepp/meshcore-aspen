// meshcore-stock-check exchanges RF traffic with a MeshCore companion radio
// through its USB interface.
// It changes that radio's tuning/name, discovers contacts and exchanges test text.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
	"unicode/utf8"

	"meshcore.local/meshcore/internal/app"
)

type options struct {
	config, serial, name  string
	timeout, phaseTimeout time.Duration
	onchip                bool
	queuedPeer            bool
	repairContact         string
}

func parseOptions(args []string, output io.Writer) (options, error) {
	var o options
	fs := flag.NewFlagSet("meshcore-stock-check", flag.ContinueOnError)
	fs.SetOutput(output)
	fs.StringVar(&o.config, "config", "", "required explicit host configuration file")
	fs.StringVar(&o.serial, "serial", "", "USB serial path for the MeshCore companion radio")
	fs.StringVar(&o.name, "name", "MeshCore Test Peer", "radio's persistent advertised name")
	fs.BoolVar(&o.onchip, "onchip", false, "read native on-chip roles from the configured status address")
	fs.BoolVar(&o.queuedPeer, "queued-peer", false, "commission a queued PHY-less USB companion against the on-chip modem")
	fs.StringVar(&o.repairContact, "repair-contact", "", "clear a named contact's advert timestamp through USB; requires its full public key and preserves identity/contact settings")
	fs.DurationVar(&o.timeout, "timeout", 12*time.Minute, "maximum time for the RF check")
	fs.DurationVar(&o.phaseTimeout, "phase-timeout", 3*time.Minute, "timeout per phase, including periodic advert discovery")
	fs.Usage = func() {
		fmt.Fprintln(output, "Usage: meshcore-stock-check -config FILE -serial DEVICE [options]")
		fmt.Fprintln(output, "Exchanges RF messages with a MeshCore companion radio. Tuning, name and learned contacts remain; cleanup tries room logout and closes connections.")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return o, err
	}
	if fs.NArg() != 0 || o.config == "" || o.serial == "" {
		return o, errors.New("explicit -config and -serial are required; positional arguments are not accepted")
	}
	if o.timeout <= 0 || o.phaseTimeout <= 0 || o.phaseTimeout > o.timeout {
		return o, errors.New("deadlines must be positive, with phase-timeout no greater than timeout")
	}
	if !utf8.ValidString(o.name) || len(o.name) == 0 || len(o.name) > 31 || strings.ContainsRune(o.name, 0) {
		return o, errors.New("name must contain 1..31 valid UTF-8 bytes without NUL")
	}
	return o, nil
}

func execute(args []string, out, diagnostics io.Writer) error {
	o, err := parseOptions(args, diagnostics)
	if errors.Is(err, flag.ErrHelp) {
		return nil
	}
	if err != nil {
		return err
	}
	cfg, err := app.LoadConfig(o.config)
	if err != nil {
		return fmt.Errorf("configuration: %w", err)
	}
	if _, err := radioCommand(cfg.Radio); err != nil {
		return err
	}
	password := cfg.RoomPassword
	if cfg.RoomPasswordEnv != "" {
		var found bool
		password, found = os.LookupEnv(cfg.RoomPasswordEnv)
		if !found || password == "" {
			return fmt.Errorf("room password environment variable %s is empty or unset", cfg.RoomPasswordEnv)
		}
	}
	if len(password) > 15 || strings.ContainsRune(password, 0) {
		return errors.New("companion login requires a password of at most 15 bytes without NUL")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	ctx, cancel := context.WithTimeout(ctx, o.timeout)
	defer cancel()
	if o.repairContact != "" {
		return repairContact(ctx, o, out)
	}
	if o.queuedPeer {
		return runRemote(ctx, cfg, o, out)
	}
	return run(ctx, cfg, o, password, out)
}

func main() {
	if err := execute(os.Args[1:], os.Stdout, os.Stderr); err != nil {
		fmt.Fprintln(os.Stderr, "RF check failed:", err)
		os.Exit(1)
	}
}
