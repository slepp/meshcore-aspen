package radio

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"sync"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
)

const (
	QueuedProtocolVersion = 1
	HWQueuedHello         = 0x20
	HWQueuedSubmit        = 0x21
	HWQueuedConfig        = 0x22
	HWQueuedSourcePolicy  = 0x23
	HWQueuedStats         = 0x24
	HWQueuedCapacity      = 0x25
	HWQueuedRolePresence  = 0x26
	HWQueuedTXEvent       = 0xfa

	queuedVersion  = QueuedProtocolVersion
	hwHello        = HWQueuedHello
	hwSubmit       = HWQueuedSubmit
	hwConfig       = HWQueuedConfig
	hwSourcePolicy = HWQueuedSourcePolicy
	hwPHYStats     = HWQueuedStats
	hwTXEvent      = HWQueuedTXEvent
	maxQueueDelay  = 0x3fffffff * time.Millisecond
	txReasonStale  = 3
)

// SharedPHYCommandErrorCode guards proxy forwarding of configuration authority.
// Zero allows forwarding; nonzero is a native KISS HW_ERR_* response code.
// CONFIG GET and CAPACITY remain available for profile and capacity readback.
// Other commands retain the proxy's existing handling and MCU validation.
func SharedPHYCommandErrorCode(command byte, payload []byte) byte {
	switch command {
	case HWQueuedHello:
		if len(payload) != 2 {
			return hardware.HW_ERR_INVALID_LENGTH
		}
		if payload[0] != QueuedProtocolVersion || payload[1] != 0 {
			return hardware.HW_ERR_INVALID_PARAM
		}
	case HWQueuedConfig:
		if len(payload) < 2 {
			return hardware.HW_ERR_INVALID_LENGTH
		}
		if payload[0] != QueuedProtocolVersion || payload[1] != 0 {
			return hardware.HW_ERR_INVALID_PARAM
		}
		if len(payload) != 2 {
			return hardware.HW_ERR_INVALID_LENGTH
		}
	case HWQueuedCapacity:
		if len(payload) != 1 {
			return hardware.HW_ERR_INVALID_LENGTH
		}
		if payload[0] != QueuedProtocolVersion {
			return hardware.HW_ERR_INVALID_PARAM
		}
	case HWQueuedRolePresence:
		return hardware.HW_ERR_INVALID_PARAM
	}
	return 0
}

var ErrParityUnavailable = errors.New("queued PHY protocol v1 unavailable")

// ErrPHYSettling rejects a submission, before it is sent, while a changed
// shared PHY readback is being admitted and adopted.
var ErrPHYSettling = fmt.Errorf("%w: shared PHY change not yet adopted", ErrOffline)
var ErrTXOutcomeUnknown = errors.New("PHY submission outcome unknown; not replaying")
var ErrPHYProfileMismatch = errors.New("shared PHY configuration differs from the fixed operator profile; join did not retune")

type PHYProfile struct {
	AirtimeFactor         float64
	CADEnabled            bool
	InterferenceThreshold int16
}

// Validate checks the native float32 factor domain without changing the profile.
func (p PHYProfile) Validate() error {
	return validateFactor(p.AirtimeFactor)
}

type TXState uint8

const (
	TXRejected TXState = iota
	TXAccepted
	TXSucceeded
	TXFailed
	TXUnknown
)

type TXResult struct {
	Generation       uint32
	JobID            uint32
	State            TXState
	Reason           uint8
	QueueWait        time.Duration
	RFAirtime        time.Duration
	EstimatedAirtime time.Duration
	packet           []byte
}

// PacketBytes returns an owned copy of the submitted wire packet, including
// for rejected, failed and unknown outcomes. It does not imply RF success.
func (r TXResult) PacketBytes() []byte {
	return bytes.Clone(r.packet)
}

func (r TXResult) Err() error {
	if r.State == TXAccepted || r.State == TXSucceeded {
		return nil
	}
	return fmt.Errorf("PHY job %d generation %d state %d reason %d; not replaying", r.JobID, r.Generation, r.State, r.Reason)
}

type pendingTX struct {
	id         uint32
	generation uint32
	data       []byte
	done       chan TXResult
}

// TXReceipt identifies one submitted PHY job, even when multiple jobs carry
// identical packets. Result receives only its final RF outcome, not acceptance.
type TXReceipt struct {
	Generation uint32
	JobID      uint32
	Result     <-chan TXResult
}

type PHYStatistics struct {
	ConfigurationGeneration uint32
	AggregateCredit         time.Duration
	AggregateRFAirtime      time.Duration
	AggregateSuccesses      uint32
	AggregateFailures       uint32
	SourceCredit            time.Duration
	SourceRFAirtime         time.Duration
	SourceSuccesses         uint32
	SourceFailures          uint32
	Queued                  uint8
	Transmitting            bool
}

func factorWire(f float64) (uint32, error) {
	if err := validateFactor(f); err != nil {
		return 0, err
	}
	return math.Float32bits(float32(f)), nil
}

func validateFactor(f float64) error {
	if math.IsNaN(f) || math.IsInf(f, 0) || f < 0 || f > math.MaxFloat32 {
		return errors.New("airtime factor must be nonnegative and finite in float32")
	}
	return nil
}

func (l *Link) profileBytes() ([]byte, error) {
	settings := l.expectedSettings()
	if _, err := factorWire(settings.Profile.AirtimeFactor); err != nil {
		return nil, err
	}
	return settings.wire(), nil
}

// negotiate joins one connection and returns its CONFIG readback, already
// checked against the tracking policy. Only a first owner connection applies
// its profile; every other join reads back without retuning.
func (l *Link) negotiate(ctx context.Context, modem *hardware.KissModem) (PHYState, error) {
	owner := byte(0)
	if l.config.ConfigurationOwner {
		owner = 1
	}
	hello, err := modem.Request(ctx, hwHello, []byte{queuedVersion, owner})
	if err != nil {
		return PHYState{}, fmt.Errorf("%w: %v", ErrParityUnavailable, err)
	}
	if len(hello) != 12 || hello[0] != queuedVersion || hello[1] != 0 {
		return PHYState{}, fmt.Errorf("%w: HELLO %x", ErrParityUnavailable, hello)
	}
	generation := binary.LittleEndian.Uint32(hello[2:])
	if generation == 0 {
		return PHYState{}, fmt.Errorf("%w: zero generation", ErrParityUnavailable)
	}
	if l.config.Session != nil && l.config.SessionPort == 0 {
		if _, err := l.config.Session.Negotiate(ctx, modem); err != nil {
			return PHYState{}, err
		}
	}
	request := []byte{queuedVersion, 0}
	l.mu.RLock()
	apply := l.config.ConfigurationOwner && !l.configured
	l.mu.RUnlock()
	if apply {
		expected, err := l.profileBytes()
		if err != nil {
			return PHYState{}, err
		}
		request = make([]byte, 24)
		request[0], request[1] = queuedVersion, 1
		copy(request[2:6], hello[6:10])
		copy(request[6:], expected)
	}
	response, err := modem.Request(ctx, hwConfig, request)
	if err != nil {
		return PHYState{}, err
	}
	state, err := parseConfigResponse(response)
	if err != nil {
		return PHYState{}, err
	}
	if err := l.admitPHY(state.Settings); err != nil {
		return PHYState{}, err
	}
	if l.config.RoleAnnouncement != nil {
		if err := l.announceRole(ctx, modem, generation); err != nil {
			return PHYState{}, err
		}
	}
	l.mu.Lock()
	l.generation, l.nextJob = generation, 0
	l.configured = true
	factor := l.sourceFactor
	l.mu.Unlock()
	return state, setSourceFactor(ctx, modem, generation, factor)
}

func setSourceFactor(ctx context.Context, modem *hardware.KissModem, generation uint32, factor float64) error {
	if err := validateFactor(factor); err != nil {
		return err
	}
	wire := math.Float32bits(float32(factor))
	p := make([]byte, 9)
	p[0] = queuedVersion
	binary.LittleEndian.PutUint32(p[1:], generation)
	binary.LittleEndian.PutUint32(p[5:], wire)
	reply, err := modem.Request(ctx, hwSourcePolicy, p)
	if err != nil {
		return err
	}
	if len(reply) != 6 || reply[0] != queuedVersion || reply[1] != 0 ||
		binary.LittleEndian.Uint32(reply[2:]) != wire {
		return fmt.Errorf("PHY source policy rejected: %x", reply)
	}
	return nil
}

func (l *Link) SetSourceAirtimeFactor(ctx context.Context, factor float64) error {
	if err := validateFactor(factor); err != nil {
		return err
	}
	if !l.config.RequireParity {
		return ErrParityUnavailable
	}
	l.request.Lock()
	defer l.request.Unlock()
	l.mu.RLock()
	modem, generation := l.modem, l.generation
	l.mu.RUnlock()
	if modem == nil {
		return ErrOffline
	}
	if err := setSourceFactor(ctx, modem, generation, factor); err != nil {
		return err
	}
	l.mu.Lock()
	l.sourceFactor = float64(float32(factor))
	l.mu.Unlock()
	return nil
}

func (l *Link) PHYStatistics(ctx context.Context) (PHYStatistics, error) {
	var stats PHYStatistics
	if !l.config.RequireParity {
		return stats, ErrParityUnavailable
	}
	l.mu.RLock()
	modem := l.modem
	l.mu.RUnlock()
	if modem == nil {
		return stats, ErrOffline
	}
	data, err := modem.Request(ctx, hwPHYStats, []byte{queuedVersion})
	if err != nil {
		return stats, err
	}
	if len(data) != 39 || data[0] != queuedVersion {
		return stats, errors.New("invalid PHY statistics response")
	}
	u32 := func(offset int) uint32 { return binary.LittleEndian.Uint32(data[offset:]) }
	ms := func(offset int) time.Duration { return time.Duration(u32(offset)) * time.Millisecond }
	stats = PHYStatistics{
		ConfigurationGeneration: u32(1), AggregateCredit: ms(5),
		AggregateRFAirtime: ms(9), AggregateSuccesses: u32(13), AggregateFailures: u32(17),
		SourceCredit: ms(21), SourceRFAirtime: ms(25), SourceSuccesses: u32(29),
		SourceFailures: u32(33), Queued: data[37], Transmitting: data[38] != 0,
	}
	return stats, nil
}

// SourceGeneration is the mast's current queued-transport generation.
// It is independent of the verified PHY configuration generation.
func (l *Link) SourceGeneration() uint32 {
	l.mu.RLock()
	defer l.mu.RUnlock()
	return l.generation
}

func (l *Link) AddTXResultHandler(handler func(TXResult)) {
	l.mu.Lock()
	l.txHandlers = append(l.txHandlers, handler)
	l.mu.Unlock()
}

// SubmitWithReceipt queues one source packet at its native priority and
// eligibility. The mast alone schedules its transmission. A transport error
// after allocation returns both a receipt and ErrTXOutcomeUnknown; callers
// must not replay that job or interpret acceptance as RF success.
func (l *Link) SubmitWithReceipt(data []byte, priority uint8, delay, expiry time.Duration) (TXReceipt, error) {
	if !l.config.RequireParity {
		return TXReceipt{}, ErrParityUnavailable
	}
	job, err := l.submit(data, priority, delay, expiry)
	if job == nil {
		return TXReceipt{}, err
	}
	return TXReceipt{Generation: job.generation, JobID: job.id, Result: job.done}, err
}

func (l *Link) submit(data []byte, priority uint8, delay, expiry time.Duration) (*pendingTX, error) {
	if len(data) == 0 || len(data) > 255 || delay < 0 || delay > maxQueueDelay ||
		expiry < 0 || expiry > maxQueueDelay {
		return nil, errors.New("invalid queued PHY submission")
	}
	eligible := time.Now().Add(delay)
	l.submitMu.Lock()
	defer l.submitMu.Unlock()
	l.mu.Lock()
	modem := l.modem
	if modem == nil {
		l.mu.Unlock()
		return nil, ErrOffline
	}
	if l.settling == modem {
		l.mu.Unlock()
		return nil, ErrPHYSettling
	}
	if len(l.pending) >= 32 || l.nextJob == math.MaxUint32 {
		l.mu.Unlock()
		return nil, node.ErrTxQueueFull
	}
	l.nextJob++
	id, generation := l.nextJob, l.generation
	job := &pendingTX{id: id, generation: generation, data: append([]byte(nil), data...), done: make(chan TXResult, 1)}
	l.pending[id] = job
	l.mu.Unlock()
	p := make([]byte, 18+len(data))
	p[0] = queuedVersion
	binary.LittleEndian.PutUint32(p[1:], generation)
	binary.LittleEndian.PutUint32(p[5:], id)
	p[9] = priority
	delay = max(0, time.Until(eligible))
	binary.LittleEndian.PutUint32(p[10:], uint32((delay+time.Millisecond-1)/time.Millisecond))
	binary.LittleEndian.PutUint32(p[14:], uint32(expiry/time.Millisecond))
	copy(p[18:], data)
	if err := modem.SendHardwareCommand(hwSubmit, p); err != nil {
		l.requestReset(modem)
		return job, fmt.Errorf("%w: %w", ErrTXOutcomeUnknown, err)
	}
	return job, nil
}

func (l *Link) receiveTXEvent(modem *hardware.KissModem, data []byte) {
	if len(data) != 23 || data[0] != queuedVersion || data[9] > byte(TXUnknown) {
		return
	}
	result := TXResult{
		Generation: binary.LittleEndian.Uint32(data[1:]), JobID: binary.LittleEndian.Uint32(data[5:]),
		State: TXState(data[9]), Reason: data[10],
		QueueWait:        time.Duration(binary.LittleEndian.Uint32(data[11:])) * time.Millisecond,
		RFAirtime:        time.Duration(binary.LittleEndian.Uint32(data[15:])) * time.Millisecond,
		EstimatedAirtime: time.Duration(binary.LittleEndian.Uint32(data[19:])) * time.Millisecond,
	}
	l.mu.Lock()
	job := l.pending[result.JobID]
	if l.modem != modem || job == nil || job.generation != result.Generation {
		l.mu.Unlock()
		return
	}
	result.packet = job.data
	handlers := append([]func(TXResult){}, l.txHandlers...)
	sent := append([]func([]byte){}, l.sent...)
	if result.State != TXAccepted {
		delete(l.pending, result.JobID)
	}
	l.mu.Unlock()
	if result.State == TXRejected && result.Reason == txReasonStale {
		// The modem rejects submissions after a configuration change until
		// this connection reads CONFIG again. Rejection is final; no replay.
		l.requestPHYRefresh()
	}
	if result.State != TXAccepted {
		job.done <- result
		if result.State == TXSucceeded {
			for _, handler := range sent {
				handler(bytes.Clone(job.data))
			}
		} else {
			l.config.Logger.Warn("PHY transmit", "error", result.Err())
		}
	}
	for _, handler := range handlers {
		handler(result)
	}
}

func (l *Link) failPending() {
	l.mu.Lock()
	pending := l.pending
	l.pending = make(map[uint32]*pendingTX)
	handlers := append([]func(TXResult){}, l.txHandlers...)
	l.mu.Unlock()
	for id, job := range pending {
		result := TXResult{Generation: job.generation, JobID: id, State: TXUnknown, Reason: 9, packet: job.data}
		job.done <- result
		for _, handler := range handlers {
			handler(result)
		}
	}
}

type queuedRadio struct {
	link   *Link
	mu     sync.RWMutex
	data   func(*meshcore.Packet)
	raw    hardware.DataFrameHandler
	closed bool
}

var _ node.TxRadio = (*queuedRadio)(nil)

func newQueuedRadio(link *Link) *queuedRadio {
	r := &queuedRadio{link: link}
	link.SetDataHandler(r.receive)
	return r
}

func (r *queuedRadio) receive(data []byte, snr float32, rssi int8, signal bool) {
	r.mu.RLock()
	handler, raw, closed := r.data, r.raw, r.closed
	r.mu.RUnlock()
	if closed {
		return
	}
	if raw != nil {
		raw(data, snr, rssi, signal)
	}
	if handler == nil {
		return
	}
	packet, err := meshcore.PacketFromBytes(data)
	if err != nil {
		return
	}
	packet.SNR, packet.RSSI, packet.HasSignalInfo = snr, rssi, signal
	if signal && snr == -32 && rssi == 127 {
		packet.MarkDoNotRetransmit()
	}
	handler(packet)
}

func (r *queuedRadio) SendData(data []byte) error {
	if !r.Enqueue(data, node.PrioritySend, 0) {
		return node.ErrTxQueueFull
	}
	return nil
}
func (r *queuedRadio) Enqueue(data []byte, priority uint8, delay time.Duration) bool {
	r.mu.RLock()
	closed := r.closed
	r.mu.RUnlock()
	if closed {
		return false
	}
	job, err := r.link.submit(data, priority, delay, 0)
	return err == nil || job != nil
}
func (r *queuedRadio) TxQueueLen() int {
	r.link.mu.RLock()
	defer r.link.mu.RUnlock()
	return len(r.link.pending)
}
func (r *queuedRadio) SetDataHandler(handler func(*meshcore.Packet)) {
	r.mu.Lock()
	r.data = handler
	r.mu.Unlock()
}
func (r *queuedRadio) SetRawDataHandler(handler func([]byte, float32, int8, bool)) {
	r.mu.Lock()
	r.raw = handler
	r.mu.Unlock()
}
func (r *queuedRadio) AddOutboundHandler(handler func([]byte)) { r.link.AddOutboundHandler(handler) }
func (r *queuedRadio) Close() error                            { r.mu.Lock(); r.closed = true; r.mu.Unlock(); return nil }
func (r *queuedRadio) SetSourceAirtimeFactor(ctx context.Context, factor float64) error {
	return r.link.SetSourceAirtimeFactor(ctx, factor)
}
