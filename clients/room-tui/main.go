package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"

	tea "charm.land/bubbletea/v2"
)

var releaseVersion = "dev"
var sourceRevision = "unknown"

func run() error {
	service := flag.String("service", "", "Room Worker origin (HTTPS, or localhost for development)")
	stateDir := flag.String("state-dir", "", "Private state directory; default is scoped to the service under the user config directory")
	version := flag.Bool("version", false, "Print terminal client version and source revision")
	flag.Parse()
	if *version {
		fmt.Printf("Aspen Rooms TUI %s (%s)\n", releaseVersion, sourceRevision)
		return nil
	}
	if flag.NArg() != 0 || *service == "" {
		return errors.New("use room-tui --service https://aspen.ve6slp.ca; approve this device in your signed-in browser")
	}
	client, err := newClient(*service)
	if err != nil {
		return err
	}
	defer client.http.CloseIdleConnections()
	store, saved, err := openState(*stateDir, client.origin)
	if err != nil {
		return err
	}
	defer store.close()
	if err := client.restoreCookies(saved.Cookies); err != nil {
		return err
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	model := newModel(ctx, cancel, client, store, saved)
	_, err = tea.NewProgram(model, tea.WithContext(ctx)).Run()
	if model.quitting {
		return model.exitErr
	}
	return err
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "Aspen Rooms:", err)
		os.Exit(1)
	}
}
