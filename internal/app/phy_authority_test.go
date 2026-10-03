package app

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"log/slog"
	"math"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/radio"
	"meshcore.local/meshcore/internal/state"
)

type authorityMast struct {
	listener          net.Listener
	clients           chan net.Conn
	done              chan struct{}
	mu                sync.Mutex
	active            []net.Conn
	profile           []byte
	owners            []byte
	sets              int
	gets              int
	capacityRequests  int
	capacitySupported bool
	unsupportedCode   byte
	malformedCapacity bool
	capacityVersion   byte
	externalSlots     byte
	localSlots        byte
	extendedSubports  bool
	silentExtended    bool
	subports          byte
	claimSupported    bool
	claimError        byte
	claimMalformed    bool
	claimStale        bool
	nativeMask        byte
	replyMask         byte
	replyFlags        byte
	nativeKeys        [][32]byte
	claimed           map[net.Conn]radio.RoleAnnouncement
	claims            []radio.RoleAnnouncement
	claimGenerations  []uint32
	nextGeneration    uint32
	preClaimRX        bool
	submissions       int
	completeTX        bool
	txPackets         [][]byte
	txTerminals       int
	staleRejections   int
	configGeneration  uint32
	airtimeReads      int
	fastAirtime       bool
}

// retune changes the mast's effective PHY as a persistent change or a
// tempradio switch would, bumping the configuration generation.
func (m *authorityMast) retune(change func(profile []byte)) {
	m.mu.Lock()
	change(m.profile)
	m.configGeneration++
	m.mu.Unlock()
}

func newAuthorityMast(t *testing.T, cfg Config) *authorityMast {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	profile := make([]byte, 18)
	copy(profile, cfg.Radio.ToBytes())
	profile[10] = cfg.TxPower
	binary.LittleEndian.PutUint32(profile[11:], math.Float32bits(1))
	mast := &authorityMast{
		listener: listener, clients: make(chan net.Conn, 8),
		done: make(chan struct{}), profile: profile,
		capacitySupported: true, capacityVersion: radio.QueuedProtocolVersion,
		unsupportedCode: hardware.HW_ERR_UNKNOWN_CMD,
		externalSlots:   8, localSlots: 0, subports: 4,
		claimSupported:   true,
		claimed:          make(map[net.Conn]radio.RoleAnnouncement),
		configGeneration: 1,
	}
	var workers sync.WaitGroup
	go func() {
		defer close(mast.done)
		for {
			client, err := listener.Accept()
			if err != nil {
				workers.Wait()
				return
			}
			mast.mu.Lock()
			mast.active = append(mast.active, client)
			mast.mu.Unlock()
			mast.clients <- client
			workers.Add(1)
			go func() {
				defer workers.Done()
				defer client.Close()
				defer func() {
					mast.mu.Lock()
					delete(mast.claimed, client)
					mast.mu.Unlock()
				}()
				mast.serve(client)
			}()
		}
	}()
	t.Cleanup(func() {
		listener.Close()
		mast.mu.Lock()
		for _, client := range mast.active {
			client.Close()
		}
		mast.mu.Unlock()
		<-mast.done
	})
	return mast
}

func (m *authorityMast) serve(client net.Conn) {
	var remainder []byte
	buffer := make([]byte, 1024)
	var generation, seen [4]uint32
	for {
		n, err := client.Read(buffer)
		if err != nil {
			return
		}
		frames, rest, failures := hardware.ExtractFrames(append(remainder, buffer[:n]...))
		if len(failures) != 0 {
			return
		}
		remainder = rest
		for _, frame := range frames {
			if frame.Port >= 4 || frame.Command != hardware.KISS_CMD_SETHARDWARE || len(frame.Data) == 0 {
				return
			}
			port := frame.Port
			command, data := frame.Data[0], frame.Data[1:]
			var response []byte
			var complete bool
			switch command {
			case radio.HWQueuedHello:
				if len(data) != 2 || data[0] != radio.QueuedProtocolVersion {
					return
				}
				m.mu.Lock()
				m.owners = append(m.owners, data[1])
				m.nextGeneration++
				generation[port] = m.nextGeneration
				m.mu.Unlock()
				response = make([]byte, 13)
				response[0], response[1] = command|0x80, radio.QueuedProtocolVersion
				binary.LittleEndian.PutUint32(response[3:], generation[port])
				m.mu.Lock()
				binary.LittleEndian.PutUint32(response[7:], m.configGeneration)
				m.mu.Unlock()
				response[11], response[12] = 12, data[1]
				m.mu.Lock()
				preClaimRX := m.preClaimRX
				m.mu.Unlock()
				if preClaimRX {
					if _, err := client.Write(hardware.EncodeDataFrame([]byte{0x15, 0, 1, 2})); err != nil {
						return
					}
				}
			case radio.HWQueuedConfig:
				if len(data) < 2 || data[0] != radio.QueuedProtocolVersion {
					return
				}
				m.mu.Lock()
				switch data[1] {
				case 0:
					m.gets++
				case 1:
					if len(data) != 24 {
						m.mu.Unlock()
						return
					}
					m.sets++
					if string(m.profile) != string(data[6:]) {
						copy(m.profile, data[6:])
						m.configGeneration++
					}
				default:
					m.mu.Unlock()
					return
				}
				seen[port] = m.configGeneration
				response = make([]byte, 25)
				response[0], response[1] = command|0x80, radio.QueuedProtocolVersion
				binary.LittleEndian.PutUint32(response[3:], m.configGeneration)
				copy(response[7:], m.profile)
				m.mu.Unlock()
			case radio.HWQueuedSourcePolicy:
				if len(data) != 9 {
					return
				}
				response = append([]byte{command | 0x80, radio.QueuedProtocolVersion, 0}, data[5:]...)
			case radio.HWQueuedCapacity:
				if len(data) == 2 && data[0] == radio.QueuedProtocolVersion {
					m.mu.Lock()
					if m.silentExtended {
						m.mu.Unlock()
						continue
					}
					if m.extendedSubports {
						response = []byte{command | 0x80, radio.QueuedProtocolVersion, m.externalSlots, m.localSlots, m.subports, 1}
					} else {
						response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_INVALID_PARAM}
					}
					m.mu.Unlock()
					break
				}
				if len(data) != 1 || data[0] != radio.QueuedProtocolVersion {
					return
				}
				m.mu.Lock()
				m.capacityRequests++
				if m.capacitySupported {
					response = []byte{command | 0x80, m.capacityVersion, m.externalSlots, m.localSlots}
					if m.malformedCapacity {
						response = response[:3]
					}
				} else {
					response = []byte{hardware.HW_RESP_ERROR, m.unsupportedCode}
				}
				m.mu.Unlock()
			case radio.HWQueuedRolePresence:
				m.mu.Lock()
				if !m.claimSupported {
					response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_UNKNOWN_CMD}
				} else {
					if len(data) != 34 || data[0] != radio.QueuedProtocolVersion || data[1] > byte(radio.ObserverRole) {
						m.mu.Unlock()
						return
					}
					var claim radio.RoleAnnouncement
					claim.Role = radio.MastRole(data[1])
					copy(claim.PublicKey[:], data[2:])
					m.claims = append(m.claims, claim)
					m.claimGenerations = append(m.claimGenerations, generation[port])
					reason := m.claimError
					var flags byte
					if m.nativeMask&(1<<claim.Role) != 0 {
						flags |= 1
					}
					for _, key := range m.nativeKeys {
						if key == claim.PublicKey {
							flags |= 4
						}
					}
					for other, previous := range m.claimed {
						if other != client {
							if previous.Role == claim.Role {
								flags |= 2
							}
							if previous.PublicKey == claim.PublicKey {
								flags |= 8
							}
						}
					}
					if reason == 0 {
						m.claimed[client] = claim
					}
					response = make([]byte, 9)
					response[0], response[1], response[2] = command|0x80, 1, reason
					binary.LittleEndian.PutUint32(response[3:], generation[port])
					response[7] = m.nativeMask
					if m.replyMask != 0 {
						response[7] = m.replyMask
					}
					response[8] = flags | m.replyFlags
					if m.claimStale {
						binary.LittleEndian.PutUint32(response[3:], generation[port]+1)
					}
					if m.claimMalformed {
						response = response[:8]
					}
				}
				m.mu.Unlock()
			case radio.HWQueuedSubmit:
				if len(data) < 19 {
					return
				}
				m.mu.Lock()
				m.submissions++
				m.txPackets = append(m.txPackets, append([]byte(nil), data[18:]...))
				stale := seen[port] != m.configGeneration
				complete = m.completeTX
				if stale {
					m.staleRejections++
				}
				m.mu.Unlock()
				response = make([]byte, 24)
				response[0], response[1] = radio.HWQueuedTXEvent, radio.QueuedProtocolVersion
				copy(response[2:10], data[1:9])
				response[10] = byte(radio.TXAccepted)
				if stale {
					// Mirrors firmware: fenced until this connection reads CONFIG.
					response[10], response[11] = byte(radio.TXRejected), 3
				}
			case hardware.HW_CMD_GET_AIRTIME:
				if len(data) != 1 {
					return
				}
				m.mu.Lock()
				m.airtimeReads++
				sf := uint32(m.profile[8])
				fast := m.fastAirtime
				m.mu.Unlock()
				response = make([]byte, 5)
				response[0] = command | 0x80
				airtime := uint32(data[0]) * sf
				if fast && data[0] != 0 {
					airtime = 10
				}
				binary.LittleEndian.PutUint32(response[1:], airtime)
			case radio.HWQueuedStats:
				if len(data) != 1 || data[0] != radio.QueuedProtocolVersion {
					return
				}
				response = make([]byte, 40)
				response[0], response[1] = command|0x80, radio.QueuedProtocolVersion
				m.mu.Lock()
				binary.LittleEndian.PutUint32(response[2:], m.configGeneration)
				binary.LittleEndian.PutUint32(response[6:], 120)
				binary.LittleEndian.PutUint32(response[10:], uint32(m.txTerminals)*10)
				binary.LittleEndian.PutUint32(response[14:], uint32(m.txTerminals))
				binary.LittleEndian.PutUint32(response[22:], 30)
				binary.LittleEndian.PutUint32(response[26:], uint32(m.txTerminals)*10)
				binary.LittleEndian.PutUint32(response[30:], uint32(m.txTerminals))
				m.mu.Unlock()
			case 0x19:
				response = []byte{command | 0x80, 1}
			default:
				response = []byte{hardware.HW_RESP_ERROR, hardware.HW_ERR_NO_CALLBACK}
			}
			if _, err := client.Write(hardware.EncodeFrame(port, hardware.KISS_CMD_SETHARDWARE, response)); err != nil {
				return
			}
			if command == radio.HWQueuedSubmit && complete && response[10] == byte(radio.TXAccepted) {
				response[10] = byte(radio.TXSucceeded)
				binary.LittleEndian.PutUint32(response[16:], 10)
				binary.LittleEndian.PutUint32(response[20:], 10)
				if _, err := client.Write(hardware.EncodeFrame(port, hardware.KISS_CMD_SETHARDWARE, response)); err != nil {
					return
				}
				m.mu.Lock()
				m.txTerminals++
				m.mu.Unlock()
			}
		}
	}
}

func waitAuthority(t *testing.T, predicate func() bool) {
	t.Helper()
	deadline := time.After(9 * time.Second)
	tick := time.NewTicker(10 * time.Millisecond)
	defer tick.Stop()
	for !predicate() {
		select {
		case <-deadline:
			t.Fatal("timed out waiting for radio authority transition")
		case <-tick.C:
		}
	}
}

func TestModemAuthorityNeverSendsConfigSetOnStartOrReconnect(t *testing.T) {
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	capacity := 4
	cfg.RadioClientCapacity = &capacity
	mast := newAuthorityMast(t, cfg)
	cfg.RadioAddress = mast.listener.Addr().String()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	linkConfig := cfg.configurationLink("controller", logger, nil, nil)
	if linkConfig.ConfigurationOwner || linkConfig.PHYTracking != radio.PHYFollow {
		t.Fatalf("modem authority would send CONFIG SET or pin the PHY: %+v", linkConfig)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	link, err := radio.Open(ctx, linkConfig)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = link.Close() })
	first := <-mast.clients
	mast.mu.Lock()
	sets, gets := mast.sets, mast.gets
	owners := append([]byte(nil), mast.owners...)
	mast.mu.Unlock()
	if sets != 0 || gets != 1 || len(owners) != 1 || owners[0] != 0 {
		t.Fatalf("startup requested ownership or CONFIG SET: sets=%d gets=%d HELLO=%v", sets, gets, owners)
	}
	// The mast was retuned while the connection was down.
	mast.retune(func(p []byte) { p[0] ^= 1 })
	first.Close()
	waitAuthority(t, func() bool {
		state, ok := link.EffectivePHY()
		return ok && state.ConfigurationGeneration == 2
	})
	state, _ := link.EffectivePHY()
	if state.Settings.Radio.FreqHz != cfg.Radio.FreqHz^1 {
		t.Fatalf("reconnect did not adopt the mast profile: %+v", state)
	}
	mast.mu.Lock()
	defer mast.mu.Unlock()
	if mast.sets != 0 || len(mast.owners) < 2 {
		t.Fatalf("reconnect requested ownership or CONFIG SET: sets=%d HELLO=%v", mast.sets, mast.owners)
	}
	for _, owner := range mast.owners {
		if owner != 0 {
			t.Fatalf("verify-only link requested HELLO owner=%d", owner)
		}
	}
}

func TestModemAuthorityFollowsInitialMastProfile(t *testing.T) {
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	mast := newAuthorityMast(t, cfg)
	mast.profile[8], mast.profile[10] = 9, 14
	cfg.RadioAddress = mast.listener.Addr().String()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := radio.Open(ctx, cfg.configurationLink("controller", slog.Default(), nil, nil))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = link.Close() })
	phy := cfg.sharedPHYStatus(link.PHYStatus())
	if !phy.Valid || phy.PHYTracking != "follow" || phy.Effective == nil ||
		phy.Effective.SpreadingFactor != 9 || phy.Effective.TxPowerDBm != 14 ||
		phy.Requested.SpreadingFactor != cfg.Radio.SF || phy.MatchesRequested == nil || *phy.MatchesRequested ||
		phy.ConfigurationGeneration != 1 || phy.Transitions != 0 {
		t.Fatalf("followed mast profile status: %+v", phy)
	}
	mast.mu.Lock()
	defer mast.mu.Unlock()
	if mast.sets != 0 {
		t.Fatalf("follower retuned the mast: sets=%d", mast.sets)
	}
}

func TestModemAuthorityFixedTrackingRejectsInitialProfileMismatch(t *testing.T) {
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	cfg.PHYTracking = "fixed"
	mast := newAuthorityMast(t, cfg)
	mast.profile[0] ^= 1
	cfg.RadioAddress = mast.listener.Addr().String()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := radio.Open(ctx, cfg.configurationLink("controller", slog.Default(), nil, nil))
	if link != nil {
		_ = link.Close()
		t.Fatal("mismatched physical profile was accepted")
	}
	if !errors.Is(err, radio.ErrPHYProfileMismatch) {
		t.Fatalf("profile mismatch did not fail closed: %v", err)
	}
	mast.mu.Lock()
	defer mast.mu.Unlock()
	if mast.sets != 0 || mast.gets != 1 {
		t.Fatalf("mismatched PHY was retuned: sets=%d gets=%d", mast.sets, mast.gets)
	}
}

// A mast-wide change reaches every host link through the shared PHY group:
// the owner's stale submission prompts a readback, and the role worker keeps
// its identity and application while its source follows the new profile.
func TestModemAuthorityRetuneKeepsRolesAliveAndRefreshesStatus(t *testing.T) {
	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	cfg.StateDir = t.TempDir()
	mast := newAuthorityMast(t, cfg)
	cfg.RadioAddress = mast.listener.Addr().String()
	cfg.phyGroup = radio.NewPHYGroup()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	owner, err := radio.Open(ctx, cfg.configurationLink("controller", logger, nil, nil))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = owner.Close() })
	var opens atomic.Int32
	worker := routingStartWithIdentity(t, ctx, cfg, "repeater", func(ctx context.Context, identity meshcore.LocalIdentity) (sourceLink, error) {
		opens.Add(1)
		return cfg.openRoleSource(ctx, "repeater", identity, logger, nil)
	})
	waitAuthority(t, func() bool {
		status := worker.status()
		return status.ApplicationStartedAt != nil && status.RadioConnected != nil && *status.RadioConnected &&
			status.PHYGeneration == 1
	})
	before := worker.status()
	mast.mu.Lock()
	airtimeReads := mast.airtimeReads
	mast.mu.Unlock()
	if airtimeReads != 256 || owner.AirtimeEstimator()(10) != 10*uint32(cfg.Radio.SF) {
		t.Fatalf("shared airtime model: reads=%d estimate=%d", airtimeReads, owner.AirtimeEstimator()(10))
	}

	mast.retune(func(p []byte) { p[8] = 9 })
	nodeRadio, stop := owner.Radio()
	defer stop()
	if !nodeRadio.(node.TxRadio).Enqueue([]byte{0x11}, 1, 0) {
		t.Fatal("enqueue after retune")
	}
	waitAuthority(t, func() bool {
		status := worker.status()
		state, ok := owner.EffectivePHY()
		return ok && state.ConfigurationGeneration == 2 && status.PHYGeneration == 2
	})
	after := worker.status()
	if opens.Load() != 1 || after.PublicKey != before.PublicKey ||
		!after.ApplicationStartedAt.Equal(*before.ApplicationStartedAt) || after.State != before.State {
		t.Fatalf("retune restarted the role: opens=%d before=%+v after=%+v", opens.Load(), before, after)
	}
	started := time.Now()
	status := cfg.ownerStatus(started, owner)
	phy := status.SharedPHY
	if status.PHYGeneration != 2 || phy == nil || !phy.Valid || phy.Effective.SpreadingFactor != 9 ||
		*phy.MatchesRequested || phy.Transitions != 1 || len(phy.RecentTransitions) != 1 ||
		phy.RecentTransitions[0].Cause != "retune" || phy.RecentTransitions[0].From.SpreadingFactor != cfg.Radio.SF ||
		phy.RecentTransitions[0].To.SpreadingFactor != 9 || status.AirtimeMS[10] != 90 ||
		owner.AirtimeEstimator()(10) != 90 {
		t.Fatalf("owner status after retune: %+v %+v", status, phy)
	}

	// tempradio returns: both links follow back and the cached table is reused.
	mast.retune(func(p []byte) { p[8] = cfg.Radio.SF })
	if !nodeRadio.(node.TxRadio).Enqueue([]byte{0x22}, 1, 0) {
		t.Fatal("enqueue after return")
	}
	waitAuthority(t, func() bool {
		state, ok := owner.EffectivePHY()
		return ok && state.ConfigurationGeneration == 3 && worker.status().PHYGeneration == 3
	})
	mast.mu.Lock()
	defer mast.mu.Unlock()
	if phy := cfg.sharedPHYStatus(owner.PHYStatus()); !*phy.MatchesRequested || phy.Transitions != 2 ||
		mast.airtimeReads != 512 || mast.sets != 0 || opens.Load() != 1 || mast.staleRejections == 0 {
		t.Fatalf("return not followed: %+v reads=%d sets=%d stale=%d", phy, mast.airtimeReads, mast.sets, mast.staleRejections)
	}
}

func TestHostAuthorityRetainsInitialConfigurationOwnership(t *testing.T) {
	cfg := DefaultConfig()
	mast := newAuthorityMast(t, cfg)
	cfg.RadioAddress = mast.listener.Addr().String()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := radio.Open(ctx, cfg.configurationLink("observer", slog.Default(), nil, nil))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = link.Close() })
	mast.mu.Lock()
	defer mast.mu.Unlock()
	if mast.sets != 1 || len(mast.owners) != 1 || mast.owners[0] != 1 {
		t.Fatalf("default host authority changed: sets=%d HELLO=%v", mast.sets, mast.owners)
	}
}

func TestHostAuthorityAppliesChangedSharedPHYAfterConfigReload(t *testing.T) {
	path := filepath.Join(t.TempDir(), "host.json")
	const first = `{"state_dir":"state","enabled_roles":[],"radio":{"FreqHz":910525000,"BwHz":62500,"SF":7,"CR":5},"tx_power":2}`
	const second = `{"state_dir":"state","enabled_roles":[],"radio":{"FreqHz":912525000,"BwHz":250000,"SF":7,"CR":5},"tx_power":2}`
	if err := os.WriteFile(path, []byte(first), 0600); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	identity, err := state.Identity(cfg.StateDir, "room")
	if err != nil {
		t.Fatal(err)
	}
	mast := newAuthorityMast(t, cfg)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	connect := func(config Config) *radio.Link {
		t.Helper()
		config.RadioAddress = mast.listener.Addr().String()
		link, err := radio.Open(ctx, config.configurationLink("controller", slog.Default(), nil, nil))
		if err != nil {
			t.Fatal(err)
		}
		return link
	}
	link := connect(cfg)
	if err := link.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(second), 0600); err != nil {
		t.Fatal(err)
	}
	reloaded, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if reloaded.StateDir != cfg.StateDir {
		t.Fatal("configuration reload changed role state authority")
	}
	if current, err := state.Identity(reloaded.StateDir, "room"); err != nil || current.PublicKey() != identity.PublicKey() {
		t.Fatalf("PHY reconfiguration changed the room identity: %v", err)
	}
	link = connect(reloaded)
	defer link.Close()
	effective, ok := link.EffectivePHY()
	if !ok || effective.Settings.Radio != reloaded.Radio || effective.Settings.TxPower != reloaded.TxPower {
		t.Fatalf("reloaded owner did not apply and verify shared PHY: %+v, %t", effective, ok)
	}
	mast.mu.Lock()
	defer mast.mu.Unlock()
	if mast.sets != 2 || mast.configGeneration != 2 || len(mast.owners) != 2 ||
		mast.owners[0] != 1 || mast.owners[1] != 1 {
		t.Fatalf("config reload did not apply exactly one shared owner profile: sets=%d generation=%d owners=%v",
			mast.sets, mast.configGeneration, mast.owners)
	}
}

func TestLivePHYHooksFollowEffectiveProfileOnlyWhenFollowing(t *testing.T) {
	retuned := radio.PHYState{ConfigurationGeneration: 2, Settings: radio.PHYSettings{
		Radio: hardware.RadioConfig{FreqHz: 915000000, BwHz: 250000, SF: 9, CR: 8}, TxPower: 14,
	}}
	link := &lifecycleLink{phy: &retuned}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	for _, tc := range []struct {
		name, authority, tracking string
		follows                   bool
	}{
		{"host", "host", "", false},
		{"modem-fixed", "modem", "fixed", false},
		{"modem-follow", "modem", "", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := DefaultConfig()
			cfg.PHYAuthority, cfg.PHYTracking = PHYAuthority(tc.authority), tc.tracking
			proxy := cfg.botProxyConfig(link, logger)
			if proxy.Radio != cfg.Radio || proxy.TxPower != cfg.TxPower || (proxy.EffectivePHY != nil) != tc.follows {
				t.Fatalf("bot proxy PHY reference: %+v", proxy)
			}
			if tc.follows {
				live, power, ok := proxy.EffectivePHY()
				link.offline.Store(true)
				_, _, offline := proxy.EffectivePHY()
				link.offline.Store(false)
				if !ok || live != retuned.Settings.Radio || power != 14 || offline {
					t.Fatalf("bot proxy live PHY: %+v %d %v offline=%v", live, power, ok, offline)
				}
			}
		})
	}

	cfg := DefaultConfig()
	cfg.PHYAuthority = "modem"
	worker := &companionWorker{cfg: cfg, role: "companion", logger: logger}
	server := worker.serverConfig(link)
	if server.RadioConfig != retuned.Settings.Radio || server.TxPower != 14 || server.EffectivePHY == nil {
		t.Fatalf("companion start-time PHY: %+v", server)
	}
	// A later tempradio return reaches the running server through the hook.
	returned := radio.PHYState{ConfigurationGeneration: 3, Settings: radio.PHYSettings{Radio: cfg.Radio, TxPower: cfg.TxPower}}
	link.phy = &returned
	if live, power, ok := server.EffectivePHY(); !ok || live != cfg.Radio || power != int8(cfg.TxPower) {
		t.Fatalf("companion live PHY: %+v %d %v", live, power, ok)
	}
}
