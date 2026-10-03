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

	"github.com/meshcore-go/meshcore-go/hardware"
)

// PHYTracking selects how a queued link treats the modem's CONFIG readback.
type PHYTracking uint8

const (
	// PHYFixed requires the configured Radio, TxPower and PHYProfile. A
	// different readback, including a later retune, leaves the link offline.
	PHYFixed PHYTracking = iota
	// PHYFollow adopts every valid profile the modem reports, at connection,
	// reconnect and after an in-connection configuration change.
	PHYFollow
)

func (t PHYTracking) String() string {
	if t == PHYFollow {
		return "follow"
	}
	return "fixed"
}

var ErrInvalidPHYProfile = errors.New("modem reported an invalid shared PHY profile")

const maxModemTxPowerDBm = 30

// PHYSettings is one complete queued CONFIG profile.
type PHYSettings struct {
	Radio   hardware.RadioConfig
	TxPower uint8
	Profile PHYProfile
}

// SupportedBandwidthHz includes RadioLib's mainline and legacy rounded
// representations for its four narrow LoRa bandwidths.
func SupportedBandwidthHz(bw uint32) bool {
	switch bw {
	case 7800, 7810, 10400, 10420, 15600, 15630, 20800, 20830, 31250, 41700, 62500, 125000, 250000, 500000:
		return true
	default:
		return false
	}
}

func (s PHYSettings) String() string {
	return fmt.Sprintf("%.3f MHz BW %.1f kHz SF%d CR%d %d dBm factor %g CAD %t threshold %d",
		float64(s.Radio.FreqHz)/1e6, float64(s.Radio.BwHz)/1e3, s.Radio.SF, s.Radio.CR,
		s.TxPower, s.Profile.AirtimeFactor, s.Profile.CADEnabled, s.Profile.InterferenceThreshold)
}

// Validate applies the host's LoRa limits to a profile before it is used.
func (s PHYSettings) Validate() error {
	r := s.Radio
	if r.FreqHz < 150000000 || r.FreqHz > 960000000 || r.SF < 5 || r.SF > 12 ||
		r.CR < 5 || r.CR > 8 || s.TxPower > maxModemTxPowerDBm {
		return errors.New("invalid frequency/SF/CR/transmit power")
	}
	if !SupportedBandwidthHz(r.BwHz) {
		return fmt.Errorf("unsupported LoRa bandwidth %d Hz", r.BwHz)
	}
	return s.Profile.Validate()
}

// wire requires a validated profile; factors narrow once to native float32.
func (s PHYSettings) wire() []byte {
	p := make([]byte, 18)
	copy(p, s.Radio.ToBytes())
	p[10] = s.TxPower
	binary.LittleEndian.PutUint32(p[11:], math.Float32bits(float32(s.Profile.AirtimeFactor)))
	if s.Profile.CADEnabled {
		p[15] = 1
	}
	binary.LittleEndian.PutUint16(p[16:], uint16(s.Profile.InterferenceThreshold))
	return p
}

func parsePHYSettings(p []byte) (PHYSettings, error) {
	if len(p) != 18 {
		return PHYSettings{}, fmt.Errorf("%w: profile length %d", ErrInvalidPHYProfile, len(p))
	}
	if p[15] > 1 {
		return PHYSettings{}, fmt.Errorf("%w: CAD flag %d", ErrInvalidPHYProfile, p[15])
	}
	s := PHYSettings{
		Radio: hardware.RadioConfig{
			FreqHz: binary.LittleEndian.Uint32(p), BwHz: binary.LittleEndian.Uint32(p[4:]),
			SF: p[8], CR: p[9],
		},
		TxPower: p[10],
		Profile: PHYProfile{
			AirtimeFactor:         float64(math.Float32frombits(binary.LittleEndian.Uint32(p[11:]))),
			CADEnabled:            p[15] == 1,
			InterferenceThreshold: int16(binary.LittleEndian.Uint16(p[16:])),
		},
	}
	if err := s.Validate(); err != nil {
		return PHYSettings{}, fmt.Errorf("%w: %v: %x", ErrInvalidPHYProfile, err, p)
	}
	return s, nil
}

// PHYState is one verified CONFIG readback on a queued connection.
type PHYState struct {
	ConfigurationGeneration uint32
	Settings                PHYSettings
	// Since is when this link first verified these settings.
	Since time.Time
}

// PHYTransition records a verified change of effective settings.
type PHYTransition struct {
	At             time.Time
	Cause          string // "retune" within a connection or "reconnect"
	FromGeneration uint32
	ToGeneration   uint32
	From           PHYSettings
	To             PHYSettings
}

// PHYStatus is a coherent snapshot of this link's view of the shared PHY.
type PHYStatus struct {
	Tracking PHYTracking
	Online   bool
	// Effective is nil while offline, before the first readback and in
	// legacy non-queued mode.
	Effective       *PHYState
	Transitions     []PHYTransition // most recent last
	TransitionCount uint64
	Error           string
}

const phyTransitionHistory = 8

func parseConfigResponse(response []byte) (PHYState, error) {
	if len(response) != 24 || response[0] != queuedVersion || response[1] != 0 ||
		binary.LittleEndian.Uint32(response[2:]) == 0 {
		return PHYState{}, fmt.Errorf("shared PHY configuration rejected: %x", response)
	}
	settings, err := parsePHYSettings(response[6:])
	if err != nil {
		return PHYState{}, err
	}
	return PHYState{ConfigurationGeneration: binary.LittleEndian.Uint32(response[2:]), Settings: settings}, nil
}

func (l *Link) expectedSettings() PHYSettings {
	profile := PHYProfile{AirtimeFactor: 1}
	if l.config.PHYProfile != nil {
		profile = *l.config.PHYProfile
	}
	return PHYSettings{Radio: l.config.Radio, TxPower: l.config.TxPower, Profile: profile}
}

// admitPHY applies the tracking policy. Parsing has already rejected
// malformed or invalid profiles under either policy.
func (l *Link) admitPHY(settings PHYSettings) error {
	if l.config.PHYTracking == PHYFollow {
		return nil
	}
	expected := l.expectedSettings()
	if !bytes.Equal(settings.wire(), expected.wire()) {
		return fmt.Errorf("%w: modem reports %s; configured %s", ErrPHYProfileMismatch, settings, expected)
	}
	return nil
}

// readPHY performs CONFIG GET, which is also the modem's per-connection
// acceptance of the current configuration generation: the mast stops
// rejecting submissions as stale. On an established connection callers must
// therefore hold submitMu or have closed the admission gate (see refreshPHY).
func (l *Link) readPHY(ctx context.Context, modem *hardware.KissModem) (PHYState, error) {
	response, err := modem.Request(ctx, hwConfig, []byte{queuedVersion, 0})
	if err != nil {
		return PHYState{}, fmt.Errorf("CONFIG GET: %w", err)
	}
	return parseConfigResponse(response)
}

// settlePHY adopts a verified readback together with the airtime model for its
// modulation. A table read spans many requests, so CONFIG is read again until
// the table and the adopted profile describe the same modulation. Submissions
// must not be admitted on this connection until it returns successfully.
func (l *Link) settlePHY(ctx context.Context, modem *hardware.KissModem, state PHYState, cause string) error {
	for attempt := 0; ; attempt++ {
		if err := l.admitPHY(state.Settings); err != nil {
			return err
		}
		if l.config.PHYGroup == nil {
			l.adoptPHY(state, nil, cause)
			return nil
		}
		var confirmed *PHYState
		table, err := l.config.PHYGroup.airtime(ctx, modulationOf(state.Settings.Radio), func() (*airtimeTable, error) {
			table, err := l.readAirtime(ctx, modem)
			if err != nil {
				return nil, err
			}
			confirm, err := l.readPHY(ctx, modem)
			if err != nil {
				return nil, err
			}
			confirmed = &confirm
			if modulationOf(confirm.Settings.Radio) != modulationOf(state.Settings.Radio) {
				return nil, errModulationChanged
			}
			return table, nil
		})
		if errors.Is(err, errModulationChanged) && confirmed != nil && attempt < 2 {
			state = *confirmed
			continue
		}
		if err != nil {
			return err
		}
		if confirmed != nil {
			if err := l.admitPHY(confirmed.Settings); err != nil {
				return err
			}
			state = *confirmed
		}
		l.adoptPHY(state, table, cause)
		return nil
	}
}

func (l *Link) adoptPHY(state PHYState, table *airtimeTable, cause string) {
	now := time.Now().UTC()
	l.mu.Lock()
	previous := l.phy
	state.Since = now
	if previous != nil && previous.Settings == state.Settings {
		state.Since = previous.Since
	}
	var transition *PHYTransition
	if previous != nil && previous.Settings != state.Settings {
		transition = &PHYTransition{
			At: now, Cause: cause,
			FromGeneration: previous.ConfigurationGeneration, ToGeneration: state.ConfigurationGeneration,
			From: previous.Settings, To: state.Settings,
		}
		l.transitions = append(l.transitions, *transition)
		if len(l.transitions) > phyTransitionHistory {
			l.transitions = append([]PHYTransition(nil), l.transitions[len(l.transitions)-phyTransitionHistory:]...)
		}
		l.transitionCount++
	}
	l.phy = &state
	if table != nil {
		l.airtime.Store(table)
	}
	l.phyError = ""
	l.mu.Unlock()
	if transition != nil {
		l.config.Logger.Warn("shared PHY changed; following modem readback",
			"cause", cause, "from_generation", transition.FromGeneration,
			"to_generation", transition.ToGeneration, "from", transition.From.String(),
			"to", transition.To.String())
	}
	if l.config.PHYGroup != nil {
		l.config.PHYGroup.observe(l, state)
	}
}

// refreshPHY re-reads CONFIG on an established connection. The readback is
// judged while submitMu is held. An unchanged profile reopens submission at
// once; anything else closes this connection's admission gate before submitMu
// is released, so no submission reaches the modem under a readback that has
// not been admitted and adopted with its airtime model. The gate reopens only
// after adoption; on any error it stays closed until the caller retires the
// connection, whose pending jobs then resolve unknown without replay.
// Submissions arriving meanwhile fail fast with ErrPHYSettling rather than
// blocking behind a possibly long airtime rebuild.
func (l *Link) refreshPHY(ctx context.Context, modem *hardware.KissModem) error {
	l.submitMu.Lock()
	state, err := l.readPHY(ctx, modem)
	l.mu.Lock()
	current := l.phy
	unchanged := err == nil && current != nil &&
		current.ConfigurationGeneration == state.ConfigurationGeneration && current.Settings == state.Settings
	if !unchanged && l.modem == modem {
		l.settling = modem
	}
	l.mu.Unlock()
	l.submitMu.Unlock()
	if err != nil || unchanged {
		return err
	}
	if err := l.settlePHY(ctx, modem, state, "retune"); err != nil {
		return err
	}
	l.mu.Lock()
	if l.settling == modem {
		l.settling = nil
	}
	l.mu.Unlock()
	return nil
}

// requestPHYRefresh asks the connection loop to re-read CONFIG without
// blocking the caller, which may be the modem's receive path.
func (l *Link) requestPHYRefresh() {
	select {
	case l.refresh <- struct{}{}:
	default:
	}
}

func (l *Link) setPHYError(err error) {
	l.mu.Lock()
	l.phyError = err.Error()
	l.mu.Unlock()
}

// EffectivePHY returns the current connection's verified profile. It is
// false while offline and in legacy non-queued mode.
func (l *Link) EffectivePHY() (PHYState, bool) {
	l.mu.RLock()
	defer l.mu.RUnlock()
	if l.modem == nil || l.phy == nil {
		return PHYState{}, false
	}
	return *l.phy, true
}

func (l *Link) PHYStatus() PHYStatus {
	l.mu.RLock()
	defer l.mu.RUnlock()
	status := PHYStatus{
		Tracking: l.config.PHYTracking, Online: l.modem != nil,
		Transitions:     append([]PHYTransition(nil), l.transitions...),
		TransitionCount: l.transitionCount, Error: l.phyError,
	}
	if status.Online && l.phy != nil {
		state := *l.phy
		status.Effective = &state
	}
	return status
}

var errModulationChanged = errors.New("shared PHY modulation changed while reading its airtime model")

type airtimeKey struct {
	bandwidth uint32
	sf, cr    uint8
}

func modulationOf(r hardware.RadioConfig) airtimeKey {
	return airtimeKey{bandwidth: r.BwHz, sf: r.SF, cr: r.CR}
}

type airtimeTable [256]uint32

// PHYGroup coordinates Links attached to one physical modem. It shares the
// modem's authoritative airtime tables by modulation, so a controlled retune
// and its return need at most one GET_AIRTIME sweep per modulation, and it
// prompts the other members to re-read CONFIG when one observes a change.
// Each member still performs its own CONFIG GET: the modem fences every
// connection's submissions separately.
type PHYGroup struct {
	mu      sync.Mutex
	tables  map[airtimeKey]*airtimeTable
	loading map[airtimeKey]chan struct{}
	members map[*Link]struct{}
	latest  *PHYState
}

func NewPHYGroup() *PHYGroup {
	return &PHYGroup{
		tables: make(map[airtimeKey]*airtimeTable), loading: make(map[airtimeKey]chan struct{}),
		members: make(map[*Link]struct{}),
	}
}

func (g *PHYGroup) join(l *Link) {
	g.mu.Lock()
	g.members[l] = struct{}{}
	g.mu.Unlock()
}

func (g *PHYGroup) leave(l *Link) {
	g.mu.Lock()
	delete(g.members, l)
	g.mu.Unlock()
}

func (g *PHYGroup) observe(from *Link, state PHYState) {
	g.mu.Lock()
	if g.latest != nil && g.latest.ConfigurationGeneration == state.ConfigurationGeneration &&
		g.latest.Settings == state.Settings {
		g.mu.Unlock()
		return
	}
	g.latest = &state
	members := make([]*Link, 0, len(g.members))
	for member := range g.members {
		if member != from {
			members = append(members, member)
		}
	}
	g.mu.Unlock()
	for _, member := range members {
		member.requestPHYRefresh()
	}
}

// airtime returns a cached table or runs one fetch per modulation at a time.
// A failed fetch is not cached; waiters then fetch on their own connection.
func (g *PHYGroup) airtime(ctx context.Context, key airtimeKey, fetch func() (*airtimeTable, error)) (*airtimeTable, error) {
	for {
		g.mu.Lock()
		if table := g.tables[key]; table != nil {
			g.mu.Unlock()
			return table, nil
		}
		if wait := g.loading[key]; wait != nil {
			g.mu.Unlock()
			select {
			case <-wait:
				continue
			case <-ctx.Done():
				return nil, ctx.Err()
			}
		}
		done := make(chan struct{})
		g.loading[key] = done
		g.mu.Unlock()
		table, err := fetch()
		g.mu.Lock()
		delete(g.loading, key)
		if err == nil {
			g.tables[key] = table
		}
		close(done)
		g.mu.Unlock()
		return table, err
	}
}
