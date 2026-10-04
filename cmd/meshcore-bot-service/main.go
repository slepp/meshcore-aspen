package main

import (
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

const tokenEnv = "MESHCORE_BOT_SERVICE_TOKEN"

func main() {
	if err := run(os.Args[1:]); err != nil {
		log.Fatal(err)
	}
}

func run(args []string) error {
	flags := flag.NewFlagSet("meshcore-bot-service", flag.ContinueOnError)
	listen := flags.String("listen", "127.0.0.1:8787", "IP address and port to listen on")
	cert := flags.String("tls-cert", "", "TLS certificate PEM file")
	key := flags.String("tls-key", "", "TLS private key PEM file")
	weather := flags.Bool("weather", false, "enable Open-Meteo weather lookups")
	compute := flags.Bool("compute", false, "enable bounded SHA256 digest RPC with request-ID deduplication")
	geocodeURL := flags.String("geocode-url", defaultGeocodeURL, "Open-Meteo geocoding endpoint")
	forecastURL := flags.String("forecast-url", defaultForecastURL, "Open-Meteo forecast endpoint")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("unexpected positional arguments")
	}
	host, _, err := net.SplitHostPort(*listen)
	if err != nil {
		return fmt.Errorf("invalid listen address: %w", err)
	}
	ip := net.ParseIP(host)
	if ip == nil {
		return errors.New("listen address must use an IP literal")
	}
	token := os.Getenv(tokenEnv)
	if err := validateToken(token); err != nil {
		return err
	}
	if (*cert == "") != (*key == "") {
		return errors.New("tls-cert and tls-key must be supplied together")
	}
	if !ip.IsLoopback() && (token == "" || *cert == "") {
		return errors.New("non-loopback listening requires a token and TLS certificate/key")
	}
	provider, err := newWeatherProvider(&http.Client{}, *geocodeURL, *forecastURL, time.Now)
	if err != nil {
		return err
	}
	handler := newRPCServer(token, *weather, provider)
	if *compute {
		handler.compute = newComputeService()
	}
	server := &http.Server{
		Handler: handler,
		// Go includes the TLS handshake in both read deadlines.
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       13 * time.Second,
		WriteTimeout:      8 * time.Second,
		IdleTimeout:       20 * time.Second,
		MaxHeaderBytes:    8 << 10,
		TLSConfig:         &tls.Config{MinVersion: tls.VersionTLS12},
	}
	listener, err := net.Listen("tcp", *listen)
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	defer listener.Close()
	if *cert != "" {
		log.Printf("meshcore-bot-service listening with TLS on %s", listener.Addr())
		return server.ServeTLS(listener, *cert, *key)
	}
	log.Printf("meshcore-bot-service listening on %s", listener.Addr())
	return server.Serve(listener)
}

func validateToken(token string) error {
	if token == "" {
		return nil
	}
	if len(token) < 32 || len(token) > 256 || strings.IndexFunc(token, func(r rune) bool { return r < 33 || r > 126 }) >= 0 {
		return errors.New("MESHCORE_BOT_SERVICE_TOKEN must be 32-256 printable ASCII bytes without whitespace")
	}
	return nil
}
