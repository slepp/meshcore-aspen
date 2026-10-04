package nativebot

import (
	"bufio"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/radio"
)

const maxFrame = 4096

type Config struct {
	Executable string
	StateDir   string
	Identity   meshcore.LocalIdentity
	Expanded   []byte
	Link       *radio.Link
	Logger     *slog.Logger
	Dormant    bool
}

type Status struct {
	Ready               bool
	Fault               bool
	ObservationsDropped uint32
	Malformed           uint32
	Duplicates          uint32
	Rejected            uint32
	VMFailures          uint32
	Replies             uint32
	Traces              uint32
}

type Worker struct {
	cancel     context.CancelFunc
	done       chan struct{}
	fatal      chan error
	management chan error
	link       *radio.Link
	radio      interface {
		SetRawDataHandler(func([]byte, float32, int8, bool))
	}
	key        [32]byte
	profile    radio.PHYState
	airtime    []uint32
	logger     *slog.Logger
	send       chan []byte
	ctx        context.Context
	ready      chan struct{}
	activated  chan bool
	dormant    bool
	activating bool
	txMu       sync.Mutex
	retiring   bool
	txWG       sync.WaitGroup
	radioDone  chan struct{}

	adminMu      sync.Mutex
	nextAdmin    uint32
	pendingAdmin map[uint32]chan string

	mu     sync.RWMutex
	status Status
}

func privateDirectory(path string) error {
	if err := os.Mkdir(path, 0700); err != nil && !errors.Is(err, os.ErrExist) {
		return err
	}
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode().Perm() != 0700 || info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("%s must be an existing private directory (0700)", path)
	}
	return nil
}

func startWorker(parent context.Context, cfg Config) (*Worker, error) {
	if len(cfg.Expanded) != 64 || !filepath.IsAbs(cfg.Executable) || !filepath.IsAbs(cfg.StateDir) ||
		cfg.Link == nil || cfg.Logger == nil {
		return nil, errors.New("native bot requires an absolute worker/state path, identity and radio")
	}
	profile, ok := cfg.Link.EffectivePHY()
	airtime := cfg.Link.AirtimeTable()
	if !ok || len(airtime) != 256 {
		return nil, errors.New("native bot requires verified effective PHY and complete mast airtime table")
	}
	for _, part := range []string{cfg.StateDir, filepath.Join(cfg.StateDir, "nvs"), filepath.Join(cfg.StateDir, "spiffs")} {
		if err := privateDirectory(part); err != nil {
			return nil, fmt.Errorf("native bot state: %w", err)
		}
	}
	ctx, cancel := context.WithCancel(parent)
	args := []string{cfg.StateDir}
	if cfg.Dormant {
		args = append(args, "--dormant")
	}
	cmd := exec.CommandContext(ctx, cfg.Executable, args...)
	cmd.Stderr = os.Stderr
	input, err := cmd.StdinPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		cancel()
		return nil, fmt.Errorf("start native bot: %w", err)
	}
	w := &Worker{
		cancel: cancel, done: make(chan struct{}), fatal: make(chan error, 1),
		management: make(chan error, 1), link: cfg.Link, profile: profile,
		airtime: airtime, logger: cfg.Logger, pendingAdmin: make(map[uint32]chan string),
		ctx: ctx, ready: make(chan struct{}), activated: make(chan bool, 1), dormant: cfg.Dormant,
	}
	copy(w.key[:], cfg.Identity.PublicKeyBytes())
	send := make(chan []byte, 48)
	w.send = send
	fail := make(chan error, 1)
	report := func(err error) {
		if err != nil && ctx.Err() == nil {
			select {
			case fail <- err:
			default:
			}
			select {
			case w.fatal <- err:
			default:
			}
			cancel()
		}
	}
	go func() {
		defer input.Close()
		for {
			select {
			case <-ctx.Done():
				return
			case packet := <-send:
				if _, err := input.Write(packet); err != nil {
					report(fmt.Errorf("native bot input: %w", err))
					return
				}
			}
		}
	}()
	go func() { report(w.read(ctx, output, send)) }()
	go func() {
		err := cmd.Wait()
		if parent.Err() == nil {
			if err == nil {
				err = errors.New("native bot exited unexpectedly")
			}
			report(fmt.Errorf("native bot process: %w", err))
		}
		close(w.done)
	}()
	var noise int16
	if telemetry, telemetryErr := cfg.Link.Snapshot(); telemetryErr == nil && telemetry.HasNoiseFloor {
		noise = telemetry.NoiseFloorDBm
	} else {
		if telemetryErr == nil {
			telemetryErr = errors.New("mast does not report a noise floor")
		}
		cfg.Logger.Warn("native bot physical noise floor unavailable; reporting unknown", "error", telemetryErr)
	}
	hello, err := helloFrame(cfg.Expanded, profile, airtime, noise)
	if err != nil {
		w.Close()
		return nil, err
	}
	if !enqueue(ctx, send, hello) {
		w.Close()
		return nil, errors.New("native bot HELLO could not be sent")
	}
	timer := time.NewTimer(35 * time.Second)
	defer timer.Stop()
	select {
	case err := <-w.management:
		if err != nil {
			w.Close()
			return nil, err
		}
	case err := <-fail:
		w.Close()
		return nil, err
	case <-timer.C:
		w.Close()
		return nil, errors.New("native bot private management readiness timeout")
	case <-ctx.Done():
		w.Close()
		return nil, ctx.Err()
	}
	w.refreshStats(ctx)
	if err := ctx.Err(); err != nil {
		select {
		case err = <-w.fatal:
		default:
		}
		w.Close()
		return nil, err
	}
	r, stop := cfg.Link.Radio()
	raw, ok := r.(interface {
		SetRawDataHandler(func([]byte, float32, int8, bool))
	})
	if !ok {
		stop()
		w.Close()
		return nil, errors.New("queued radio does not expose raw RF receptions")
	}
	w.radio = raw
	w.radioDone = make(chan struct{})
	raw.SetRawDataHandler(func(packet []byte, snr float32, rssi int8, measured bool) {
		w.mu.RLock()
		dormant := w.dormant
		w.mu.RUnlock()
		if dormant || ctx.Err() != nil {
			return
		}
		if len(packet) == 0 || len(packet) > 255 {
			cfg.Logger.Warn("native bot invalid RF packet dropped", "length", len(packet))
			return
		}
		if !w.profileMatches() {
			w.abort(errors.New("native bot received RF after its verified PHY changed; restart required"))
			return
		}
		local := measured && snr == -32 && rssi == 127
		body := make([]byte, 10+len(packet))
		body[0] = 0x02
		if measured {
			binary.LittleEndian.PutUint32(body[1:], math.Float32bits(float32(rssi)))
			binary.LittleEndian.PutUint32(body[5:], math.Float32bits(snr))
		} else {
			body[9] = 2
		}
		if local {
			body[9] = 1
		}
		copy(body[10:], packet)
		if !enqueue(ctx, send, framed(body)) {
			cfg.Logger.Warn("native bot RF receive overflow; packet dropped")
		}
	})
	go func() {
		defer close(w.radioDone)
		defer stop()
		select {
		case <-ctx.Done():
		case <-w.done:
		}
		raw.SetRawDataHandler(nil)
	}()
	go w.watch(ctx)
	return w, nil
}

func (w *Worker) profileMatches() bool {
	profile, ok := w.link.EffectivePHY()
	return ok && profile.ConfigurationGeneration == w.profile.ConfigurationGeneration &&
		profile.Settings == w.profile.Settings && equalAirtime(w.link.AirtimeTable(), w.airtime)
}

func (w *Worker) watch(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if _, ok := w.link.EffectivePHY(); ok && !w.profileMatches() {
				select {
				case w.fatal <- errors.New("native bot PHY changed; restart required before further RF"):
				default:
				}
				w.cancel()
				return
			}
			w.refreshStats(ctx)
		}
	}
}

func (w *Worker) refreshStats(ctx context.Context) {
	request, cancel := context.WithTimeout(ctx, 750*time.Millisecond)
	defer cancel()
	stats, err := w.link.PHYStatistics(request)
	if err != nil {
		w.logger.Warn("native bot physical airtime snapshot unavailable", "error", err)
		return
	}
	if stats.ConfigurationGeneration != w.profile.ConfigurationGeneration {
		w.abort(errors.New("native bot physical snapshot changed configuration; restart required"))
		return
	}
	data, err := statsFrame(w.link.SourceGeneration(), stats)
	if err != nil {
		w.abort(err)
		return
	}
	if !enqueue(ctx, w.send, data) {
		w.abort(errors.New("native bot physical snapshot could not be delivered"))
	}
}

func statsFrame(generation uint32, stats radio.PHYStatistics) ([]byte, error) {
	if generation == 0 || stats.ConfigurationGeneration == 0 {
		return nil, errors.New("native bot physical snapshot has no verified generation")
	}
	data := make([]byte, 43)
	data[0] = 0x05
	fields := []uint32{
		generation, stats.ConfigurationGeneration,
		durationMS(stats.AggregateCredit), durationMS(stats.SourceCredit),
		durationMS(stats.AggregateRFAirtime), durationMS(stats.SourceRFAirtime),
		stats.AggregateSuccesses, stats.AggregateFailures,
		stats.SourceSuccesses, stats.SourceFailures,
	}
	for i, value := range fields {
		binary.LittleEndian.PutUint32(data[1+4*i:], value)
	}
	data[41] = stats.Queued
	if stats.Transmitting {
		data[42] = 1
	}
	return framed(data), nil
}

func equalAirtime(a, b []uint32) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func (w *Worker) Close() error {
	w.txMu.Lock()
	w.retiring = true
	w.txMu.Unlock()
	w.cancel()
	<-w.done
	w.txWG.Wait()
	if w.radioDone != nil {
		<-w.radioDone
	}
	return nil
}

func (w *Worker) abort(err error) {
	select {
	case w.fatal <- err:
	default:
	}
	w.cancel()
}

func (w *Worker) Fatal() <-chan error { return w.fatal }

func (w *Worker) Snapshot() Status {
	w.mu.RLock()
	defer w.mu.RUnlock()
	return w.status
}

func enqueue(ctx context.Context, channel chan<- []byte, data []byte) bool {
	select {
	case <-ctx.Done():
		return false
	case channel <- data:
		return true
	default:
		return false
	}
}

func framed(payload []byte) []byte {
	data := make([]byte, 4+len(payload))
	binary.LittleEndian.PutUint32(data, uint32(len(payload)))
	copy(data[4:], payload)
	return data
}

func helloFrame(identity []byte, profile radio.PHYState, table []uint32, noise int16) ([]byte, error) {
	if len(identity) != 64 || len(table) != 256 {
		return nil, errors.New("invalid native bot HELLO identity or PHY table")
	}
	data := make([]byte, 1104)
	data[0] = 0x01
	binary.LittleEndian.PutUint16(data[1:], 1)
	copy(data[3:], identity)
	settings := profile.Settings
	binary.LittleEndian.PutUint32(data[67:], settings.Radio.FreqHz)
	binary.LittleEndian.PutUint32(data[71:], settings.Radio.BwHz)
	data[75], data[76], data[77] = settings.Radio.SF, settings.Radio.CR, settings.TxPower
	binary.LittleEndian.PutUint16(data[78:], uint16(noise))
	for i, ms := range table {
		binary.LittleEndian.PutUint32(data[80+i*4:], ms)
		if i > 0 && (ms == 0 || ms > 3600000 || i > 1 && ms < table[i-1]) {
			return nil, fmt.Errorf("mast airtime table invalid at packet size %d", i)
		}
	}
	return framed(data), nil
}

func (w *Worker) read(ctx context.Context, stream io.Reader, send chan<- []byte) error {
	var header [4]byte
	readySeen, managementSeen := false, false
	for {
		if _, err := io.ReadFull(stream, header[:]); err != nil {
			return fmt.Errorf("native bot output: %w", err)
		}
		n := binary.LittleEndian.Uint32(header[:])
		if n < 1 || n > maxFrame {
			return errors.New("native bot output has invalid length")
		}
		body := make([]byte, n)
		if _, err := io.ReadFull(stream, body); err != nil {
			return fmt.Errorf("native bot output: %w", err)
		}
		switch body[0] {
		case 0x81:
			if len(body) < 15 || len(body) > 269 || body[5] > 7 {
				return errors.New("native bot TX request invalid")
			}
			w.mu.RLock()
			dormant := w.dormant && !w.activating
			w.mu.RUnlock()
			if dormant {
				return errors.New("dormant native bot attempted RF submission")
			}
			w.txMu.Lock()
			if !w.retiring {
				w.txWG.Add(1)
				go func() { defer w.txWG.Done(); w.transmit(ctx, send, body) }()
			}
			w.txMu.Unlock()
		case 0x82:
			if readySeen || !managementSeen || len(body) != 34 || body[33] != 1 || !equalKey(body[1:33], w.key[:]) {
				return errors.New("native bot READY public identity mismatch")
			}
			readySeen = true
			w.mu.Lock()
			w.status.Ready = !w.dormant
			w.mu.Unlock()
			close(w.ready)
		case 0x87:
			if !readySeen || len(body) != 2 || body[1] > 1 {
				return errors.New("native bot ACTIVATE reply invalid")
			}
			w.mu.Lock()
			if !w.dormant || !w.activating {
				w.mu.Unlock()
				return errors.New("native bot ACTIVATE reply without a committed activation")
			}
			w.dormant, w.activating, w.status.Ready = false, false, true
			w.mu.Unlock()
			w.activated <- body[1] == 1
		case 0x86:
			if managementSeen || len(body) != 34 || body[33] != 1 || !equalKey(body[1:33], w.key[:]) {
				return errors.New("native bot private management identity mismatch")
			}
			managementSeen = true
			w.management <- nil
		case 0x83:
			if len(body) != 31 || body[1] > 1 || body[2] > 1 {
				return errors.New("native bot STATUS invalid")
			}
			w.mu.Lock()
			w.status = Status{Ready: body[1] == 1, Fault: body[2] == 1,
				ObservationsDropped: binary.LittleEndian.Uint32(body[3:]),
				Malformed:           binary.LittleEndian.Uint32(body[7:]),
				Duplicates:          binary.LittleEndian.Uint32(body[11:]),
				Rejected:            binary.LittleEndian.Uint32(body[15:]),
				VMFailures:          binary.LittleEndian.Uint32(body[19:]),
				Replies:             binary.LittleEndian.Uint32(body[23:]),
				Traces:              binary.LittleEndian.Uint32(body[27:])}
			if w.dormant {
				w.status.Ready = false
			}
			w.mu.Unlock()
		case 0x84:
			if len(body) != 2 {
				return errors.New("native bot ERROR invalid")
			}
			return fmt.Errorf("native bot reported failure code %d", body[1])
		case 0x85:
			if len(body) < 5 || len(body) > 261 {
				return errors.New("native bot ADMIN reply invalid")
			}
			for _, c := range body[5:] {
				if c < 32 || c > 126 {
					return errors.New("native bot ADMIN reply is not printable ASCII")
				}
			}
			id := binary.LittleEndian.Uint32(body[1:])
			w.adminMu.Lock()
			reply := w.pendingAdmin[id]
			if reply != nil {
				delete(w.pendingAdmin, id)
			}
			w.adminMu.Unlock()
			if reply != nil {
				reply <- string(body[5:])
			}
		default:
			return fmt.Errorf("native bot emitted unknown frame type %d", body[0])
		}
	}
}

func (w *Worker) Admin(ctx context.Context, command string) (string, error) {
	if len(command) < 1 || len(command) > 160 {
		return "", errors.New("native bot admin command must be 1..160 bytes")
	}
	for i := range command {
		if command[i] < 32 || command[i] > 126 {
			return "", errors.New("native bot admin command must be printable ASCII")
		}
	}
	w.adminMu.Lock()
	if w.nextAdmin == ^uint32(0) {
		w.adminMu.Unlock()
		return "", errors.New("native bot admin request IDs exhausted; restart required")
	}
	w.nextAdmin++
	id := w.nextAdmin
	reply := make(chan string, 1)
	w.pendingAdmin[id] = reply
	w.adminMu.Unlock()
	defer func() {
		w.adminMu.Lock()
		delete(w.pendingAdmin, id)
		w.adminMu.Unlock()
	}()
	body := make([]byte, 5+len(command))
	body[0] = 0x04
	binary.LittleEndian.PutUint32(body[1:], id)
	copy(body[5:], command)
	if !enqueue(ctx, w.send, framed(body)) {
		return "", errors.New("native bot admin input congested or closed")
	}
	select {
	case result := <-reply:
		if strings.HasPrefix(result, "Error:") {
			return result, errors.New(result)
		}
		return result, nil
	case <-ctx.Done():
		return "", ctx.Err()
	case <-w.ctx.Done():
		return "", errors.New("native bot worker stopped")
	}
}

func (w *Service) listenAdmin(ctx context.Context, path string) error {
	if info, err := os.Lstat(path); err == nil {
		if info.Mode()&os.ModeSocket == 0 {
			return fmt.Errorf("bot admin path %s is not a socket", path)
		}
		if err := os.Remove(path); err != nil {
			return err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	listener, err := net.Listen("unix", path)
	if err != nil {
		return fmt.Errorf("bot admin socket: %w", err)
	}
	if err := os.Chmod(path, 0600); err != nil {
		listener.Close()
		os.Remove(path)
		return err
	}
	w.adminSocket, w.adminPath = listener, path
	go func() {
		<-ctx.Done()
		_ = listener.Close()
	}()
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				if ctx.Err() == nil {
					w.logger.Error("native bot admin socket stopped", "error", err)
					w.abort(fmt.Errorf("native bot admin socket: %w", err))
				}
				return
			}
			w.serveAdmin(ctx, conn)
		}
	}()
	return nil
}

func (w *Service) serveAdmin(ctx context.Context, connection net.Conn) {
	defer connection.Close()
	peer, ok := connection.(*net.UnixConn)
	if !ok {
		return
	}
	raw, err := peer.SyscallConn()
	if err != nil {
		return
	}
	var credential *syscall.Ucred
	var credentialErr error
	err = raw.Control(func(fd uintptr) {
		credential, credentialErr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	})
	if err != nil || credentialErr != nil || credential == nil || credential.Uid != uint32(os.Geteuid()) {
		w.logger.Warn("native bot admin peer rejected")
		return
	}
	if err := connection.SetDeadline(time.Now().Add(10 * time.Second)); err != nil {
		return
	}
	line, err := bufio.NewReader(io.LimitReader(connection, 162)).ReadString('\n')
	if err != nil || !strings.HasSuffix(line, "\n") {
		_, _ = io.WriteString(connection, "Error: expected a newline-terminated command of at most 160 bytes\n")
		return
	}
	if err := connection.SetDeadline(time.Now().Add(80 * time.Second)); err != nil {
		return
	}
	timeout, cancel := context.WithTimeout(ctx, 75*time.Second)
	defer cancel()
	reply, err := w.Admin(timeout, strings.TrimSuffix(line, "\n"))
	if err != nil && reply == "" {
		reply = "Error: " + err.Error()
	}
	if len(reply) > 256 {
		reply = reply[:253] + "..."
	}
	_, _ = io.WriteString(connection, reply+"\n")
}

func equalKey(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func (w *Worker) transmit(ctx context.Context, send chan<- []byte, request []byte) {
	token := binary.LittleEndian.Uint32(request[1:])
	if !w.profileMatches() {
		w.abort(errors.New("native bot TX prevented after its verified PHY changed; restart required"))
		return
	}
	delay := time.Duration(binary.LittleEndian.Uint32(request[6:])) * time.Millisecond
	expiry := time.Duration(binary.LittleEndian.Uint32(request[10:])) * time.Millisecond
	receipt, err := w.link.SubmitWithReceipt(request[14:], request[5], delay, expiry)
	if err != nil && receipt.Result == nil {
		reason := uint8(7)
		if errors.Is(err, radio.ErrOffline) {
			reason = 9
		}
		if !enqueue(ctx, send, txResult(token, radio.TXRejected, reason, 0, 0, 0)) {
			w.abort(errors.New("native bot RF rejection receipt cannot be delivered"))
		}
		w.logger.Warn("native bot RF admission rejected", "error", err)
		return
	}
	if !enqueue(ctx, send, txResult(token, radio.TXAccepted, 0, 0, 0, 0)) {
		w.abort(errors.New("native bot RF admission receipt cannot be delivered"))
		return
	}
	if err != nil {
		if !enqueue(ctx, send, txResult(token, radio.TXUnknown, 9, 0, 0, 0)) {
			w.abort(errors.New("native bot RF unknown receipt cannot be delivered"))
		}
		w.logger.Warn("native bot RF submission uncertain; not replaying", "error", err)
		return
	}
	select {
	case result := <-receipt.Result:
		if !enqueue(ctx, send, txResult(token, result.State, result.Reason,
			durationMS(result.QueueWait), durationMS(result.RFAirtime), durationMS(result.EstimatedAirtime))) {
			w.abort(errors.New("native bot RF terminal receipt cannot be delivered"))
		}
	case <-ctx.Done():
		w.logger.Warn("native bot RF result unresolved at worker retirement; outcome unknown, not replaying", "token", token)
	}
}

func durationMS(value time.Duration) uint32 {
	if value < 0 {
		return 0
	}
	if uint64(value/time.Millisecond) > uint64(^uint32(0)) {
		return ^uint32(0)
	}
	return uint32(value / time.Millisecond)
}

func txResult(token uint32, state radio.TXState, reason uint8, queue, rf, estimate uint32) []byte {
	body := make([]byte, 20)
	body[0] = 0x03
	binary.LittleEndian.PutUint32(body[1:], token)
	body[5], body[6] = byte(state), reason
	binary.LittleEndian.PutUint32(body[7:], queue)
	binary.LittleEndian.PutUint32(body[11:], rf)
	binary.LittleEndian.PutUint32(body[15:], estimate)
	if state == radio.TXSucceeded || state == radio.TXFailed && rf != 0 && reason != 9 {
		body[19] = 1
	}
	return framed(body)
}
