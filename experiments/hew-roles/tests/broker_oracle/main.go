// Test-only oracle: the application's actual Mochi broker and configured bounds.
package main

import (
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"

	"meshcore.local/meshcore/internal/observer"
)

func main() {
	cfg := observer.BrokerConfig{Address: "127.0.0.1:0", Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	if len(os.Args) >= 3 {
		cfg.Username, cfg.Password = os.Args[1], os.Args[2]
	}
	if len(os.Args) == 4 {
		cfg.Address = net.JoinHostPort(os.Args[3], "0")
	}
	broker, err := observer.StartBroker(cfg)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("ORACLE_READY address=%s\n", broker.Addr())
	done := make(chan os.Signal, 1)
	signal.Notify(done, syscall.SIGINT, syscall.SIGTERM)
	<-done
	if err := broker.Close(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
