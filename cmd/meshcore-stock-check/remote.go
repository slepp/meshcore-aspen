package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	ctransport "github.com/meshcore-go/meshcore-go/companion/transport"
	"meshcore.local/meshcore/internal/app"
)

type remoteSource struct {
	Slot       int    `json:"slot"`
	Generation uint32 `json:"generation"`
	Negotiated bool   `json:"negotiated"`
	Connected  bool   `json:"connected"`
	Fault      bool   `json:"fault"`
}

type remoteRole struct {
	Role       string `json:"role"`
	PublicKey  string `json:"public_key"`
	Ready      bool   `json:"ready"`
	Slot       *int   `json:"source_slot"`
	Generation uint32 `json:"source_generation"`
}

type remoteEvent struct {
	Sequence   uint64  `json:"sequence"`
	Direction  string  `json:"direction"`
	State      uint8   `json:"state"`
	Reason     uint8   `json:"reason"`
	Slot       int     `json:"source_slot"`
	Generation uint32  `json:"source_generation"`
	Job        uint32  `json:"job_id"`
	Length     int     `json:"length"`
	RFMillis   *uint32 `json:"rf_ms"`
	Preview    string  `json:"preview_hex"`
}

type remoteSnapshot struct {
	Version     int    `json:"api_version"`
	Publication uint64 `json:"publication"`
	Uptime      uint64 `json:"uptime_ms"`
	Profile     struct {
		Frequency  uint32 `json:"frequency_hz"`
		Bandwidth  uint32 `json:"bandwidth_hz"`
		SF         uint8  `json:"sf"`
		CR         uint8  `json:"cr"`
		TxPower    uint8  `json:"tx_power_dbm"`
		Generation uint32 `json:"generation"`
		Committed  bool   `json:"committed"`
		Fault      bool   `json:"fault"`
	} `json:"profile"`
	Roles []remoteRole `json:"roles"`
	Kiss  struct {
		Clients []remoteSource `json:"clients"`
	} `json:"kiss"`
	// Additive UART status is required when the queued peer uses that source.
	// TCP clients are already represented by kiss.clients in dashboard API v1.
	Stream *remoteSource `json:"stream"`
	Totals struct {
		RX        uint64 `json:"rx_packets"`
		Accepted  uint64 `json:"tx_accepted"`
		Succeeded uint64 `json:"tx_succeeded"`
	} `json:"totals"`
	History struct {
		Events []remoteEvent `json:"events"`
	} `json:"history"`
}

func readRemoteSnapshot(ctx context.Context, address string) (remoteSnapshot, error) {
	var snapshot remoteSnapshot
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+localAddress(address)+"/api/status", nil)
	if err != nil {
		return snapshot, err
	}
	var response *http.Response
	for attempt := 0; ; attempt++ {
		response, err = http.DefaultClient.Do(req)
		if err != nil {
			return snapshot, err
		}
		if response.StatusCode != http.StatusTooManyRequests || attempt == 1 {
			break
		}
		response.Body.Close()
		select {
		case <-time.After(time.Second):
		case <-ctx.Done():
			return snapshot, ctx.Err()
		}
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return snapshot, fmt.Errorf("physical modem status: %s", response.Status)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, 64*1024+1))
	if err != nil {
		return snapshot, err
	}
	if len(body) > 64*1024 {
		return snapshot, errors.New("physical modem status exceeds 64 KiB")
	}
	if err := json.Unmarshal(body, &snapshot); err != nil {
		return snapshot, err
	}
	if snapshot.Version != 1 || snapshot.Publication == 0 || len(snapshot.History.Events) > 32 {
		return snapshot, errors.New("unsupported/incomplete physical dashboard snapshot")
	}
	return snapshot, nil
}

func (s remoteSnapshot) sources() []remoteSource {
	result := append([]remoteSource(nil), s.Kiss.Clients...)
	for i := range result {
		result[i].Connected = true // Native list contains connected TCP clients only.
	}
	if s.Stream != nil {
		result = append(result, *s.Stream)
	}
	return result
}

func (s remoteSnapshot) external(source remoteSource) bool {
	for _, role := range s.Roles {
		if role.Slot != nil && *role.Slot == source.Slot {
			return false
		}
	}
	for _, entry := range s.sources() {
		if entry.Slot == source.Slot && entry.Generation == source.Generation &&
			entry.Generation != 0 && entry.Connected && entry.Negotiated && !entry.Fault {
			return true
		}
	}
	return false
}

func (s remoteSnapshot) validate(cfg app.Config) error {
	p := s.Profile
	if p.Fault || !p.Committed || p.Generation == 0 ||
		p.Frequency != cfg.Radio.FreqHz || p.Bandwidth != cfg.Radio.BwHz ||
		p.SF != cfg.Radio.SF || p.CR != cfg.Radio.CR || p.TxPower != cfg.TxPower {
		return errors.New("physical modem committed profile does not match config; refusing to retune")
	}
	if len(s.Roles) != 5 {
		return errors.New("queued-peer mode requires five on-chip role identities")
	}
	names, identities := map[string]bool{}, map[[32]byte]bool{}
	for _, role := range s.Roles {
		id, err := meshcore.NewIdentityFromHex(role.PublicKey)
		if err != nil || id.IsZero() || !role.Ready || role.Role == "" ||
			names[role.Role] || identities[id.PublicKey()] {
			return fmt.Errorf("invalid, duplicate or unready on-chip identity for %q", role.Role)
		}
		names[role.Role], identities[id.PublicKey()] = true, true
	}
	return nil
}

// The finite event journal must be sampled independently of blocking SDK calls.
// A missed sequence, role restart, or profile change invalidates this run.
type remoteJournal struct {
	mu       sync.Mutex
	initial  remoteSnapshot
	latest   remoteSnapshot
	cursor   uint64
	events   []remoteEvent
	selected *remoteSource
}

func newRemoteJournal(snapshot remoteSnapshot) *remoteJournal {
	j := &remoteJournal{initial: snapshot, latest: snapshot}
	for _, event := range snapshot.History.Events {
		j.cursor = max(j.cursor, event.Sequence)
	}
	return j
}

func (j *remoteJournal) ingest(snapshot remoteSnapshot) error {
	j.mu.Lock()
	defer j.mu.Unlock()
	if snapshot.Publication < j.latest.Publication || snapshot.Uptime < j.latest.Uptime ||
		snapshot.Profile != j.initial.Profile || len(snapshot.Roles) != len(j.initial.Roles) {
		return errors.New("physical modem restarted or its profile/roles changed")
	}
	for i, role := range snapshot.Roles {
		initial := j.initial.Roles[i]
		if role.Role != initial.Role || role.PublicKey != initial.PublicKey ||
			role.Generation != initial.Generation || !role.Ready ||
			(role.Slot == nil) != (initial.Slot == nil) ||
			(role.Slot != nil && *role.Slot != *initial.Slot) {
			return errors.New("on-chip role identity or radio source changed during queued-peer check")
		}
	}
	if j.selected != nil && !snapshot.external(*j.selected) {
		return errors.New("queued peer source disconnected, renegotiated or became unobservable")
	}
	if snapshot.Totals.Accepted < j.latest.Totals.Accepted || snapshot.Totals.Succeeded < j.latest.Totals.Succeeded ||
		snapshot.Totals.RX < j.latest.Totals.RX {
		return errors.New("physical modem counters regressed")
	}
	events := append([]remoteEvent(nil), snapshot.History.Events...)
	sort.Slice(events, func(a, b int) bool { return events[a].Sequence < events[b].Sequence })
	for _, event := range events {
		if event.Sequence <= j.cursor {
			continue
		}
		if event.Sequence != j.cursor+1 {
			return errors.New("physical event history gap; cannot certify queued job lifecycle")
		}
		if len(j.events) == maxEvents {
			return errors.New("queued-peer event capture overflow")
		}
		j.events = append(j.events, event)
		j.cursor = event.Sequence
	}
	j.latest = snapshot
	return nil
}

func (j *remoteJournal) mark() uint64 {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.cursor
}

type remoteJobProof struct {
	Terminal remoteEvent `json:"terminal"`
}

// Native history contains terminal events; admission is counted in aggregate.
func remoteJobs(events []remoteEvent, after uint64, source *remoteSource, match func(remoteEvent) bool) ([]remoteJobProof, error) {
	var proofs []remoteJobProof
	seen := make(map[[3]uint64]bool)
	for _, terminal := range events {
		if terminal.Sequence <= after || terminal.Direction != "tx" || terminal.State == 1 ||
			terminal.Job == 0 || terminal.Generation == 0 ||
			(source != nil && (terminal.Slot != source.Slot || terminal.Generation != source.Generation)) || !match(terminal) {
			continue
		}
		if terminal.State != 2 || terminal.Reason != 0 {
			return nil, fmt.Errorf("queued source %d/%d job %d ended state=%d reason=%d", terminal.Slot, terminal.Generation, terminal.Job, terminal.State, terminal.Reason)
		}
		if terminal.RFMillis == nil || *terminal.RFMillis == 0 {
			return nil, errors.New("queued TX terminal result lacks measured RF occupancy")
		}
		key := [3]uint64{uint64(terminal.Slot), uint64(terminal.Generation), uint64(terminal.Job)}
		if seen[key] {
			return nil, errors.New("duplicate terminal result for queued source/generation/job")
		}
		seen[key] = true
		proofs = append(proofs, remoteJobProof{Terminal: terminal})
	}
	return proofs, nil
}

func (j *remoteJournal) admissionDelta() uint64 {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.latest.Totals.Accepted - j.initial.Totals.Accepted
}

func remotePayload(event remoteEvent, kind byte) ([]byte, bool) {
	raw, err := hex.DecodeString(event.Preview)
	if err != nil || len(raw) < 2 || len(raw) != min(event.Length, 16) || raw[0]>>6 != 0 || raw[0]>>2&15 != kind {
		return nil, false
	}
	offset := 1
	route := raw[0] & 3
	if route == meshcore.RouteTypeTransportDirect || route == meshcore.RouteTypeTransportFlood {
		offset += 4
	}
	if offset >= len(raw) || !meshcore.IsValidPathLen(raw[offset]) || raw[offset]&63 != 0 {
		return nil, false
	}
	return raw[offset+1:], true
}

func remoteAdvertMatch(identity meshcore.Identity) func(remoteEvent) bool {
	return func(event remoteEvent) bool {
		payload, ok := remotePayload(event, meshcore.PayloadTypeAdvert)
		key := identity.PublicKey()
		return ok && len(payload) >= 10 && event.Length >= 102 && bytes.Equal(payload, key[:len(payload)])
	}
}

func remoteTextMatch(from, to meshcore.Identity) func(remoteEvent) bool {
	return func(event remoteEvent) bool {
		payload, ok := remotePayload(event, meshcore.PayloadTypeTxtMsg)
		return ok && len(payload) >= 4 && payload[0] == to.PublicKey()[0] &&
			payload[1] == from.PublicKey()[0] && event.Length >= 22
	}
}

func remoteACKMatch(tag uint32) func(remoteEvent) bool {
	return func(event remoteEvent) bool {
		raw, err := hex.DecodeString(event.Preview)
		if err != nil || len(raw) != event.Length || len(raw) > 16 {
			return false
		}
		packet, err := meshcore.PacketFromBytes(raw)
		if err != nil || packet.PathHashCount() != 0 || !packet.IsRouteDirect() {
			return false
		}
		ack, ok := packetACK(packet)
		return ok && ack == tag
	}
}

func (j *remoteJournal) advert(after uint64, id meshcore.Identity, source *remoteSource) (remoteJobProof, bool, error) {
	j.mu.Lock()
	defer j.mu.Unlock()
	proofs, err := remoteJobs(j.events, after, source, remoteAdvertMatch(id))
	if err != nil {
		return remoteJobProof{}, false, err
	}
	var selected []remoteJobProof
	for _, proof := range proofs {
		candidate := remoteSource{Slot: proof.Terminal.Slot, Generation: proof.Terminal.Generation}
		if source != nil || (j.initial.external(candidate) && j.latest.external(candidate)) {
			selected = append(selected, proof)
		}
	}
	if len(selected) > 1 {
		return remoteJobProof{}, false, errors.New("ambiguous advert jobs; refusing to choose another source or retry")
	}
	if len(selected) == 0 {
		return remoteJobProof{}, false, nil
	}
	if j.latest.Totals.Succeeded <= j.initial.Totals.Succeeded {
		return remoteJobProof{}, false, errors.New("job events and physical totals disagree")
	}
	if source == nil {
		for _, entry := range j.latest.sources() {
			if entry.Slot == selected[0].Terminal.Slot && entry.Generation == selected[0].Terminal.Generation {
				j.selected = &entry
				break
			}
		}
	}
	return selected[0], true, nil
}

// runRemote certifies native applications and queued shared-modem TX, not a
// second radio receiver. It intentionally never calls the OTA-only checker.
func runRemote(ctx context.Context, cfg app.Config, o options, out io.Writer) (result error) {
	if !o.onchip || o.serial == "" || o.config == "" || o.phaseTimeout <= 0 || o.timeout <= 0 {
		return errors.New("queued-peer mode requires -onchip, explicit -serial/-config and positive deadlines")
	}
	ctx, stop := context.WithTimeout(ctx, o.timeout)
	defer stop()
	ctx, cancel := context.WithCancelCause(ctx)
	defer cancel(nil)
	emit := func(phase string, details any) error {
		return json.NewEncoder(out).Encode(map[string]any{"mode": "queued-native-shared-modem", "phase": phase, "details": details})
	}
	if err := emit("scope", "Queued companion transmissions use the shared modem; messages arrive through local reflection"); err != nil {
		return err
	}
	snapshot, err := readRemoteSnapshot(ctx, cfg.StatusListen)
	if err != nil {
		return err
	}
	if err := snapshot.validate(cfg); err != nil {
		return err
	}
	observable := false
	for _, source := range snapshot.sources() {
		observable = observable || snapshot.external(source)
	}
	if !observable {
		return errors.New("no negotiated external source in /api/status; UART peers require stream slot/generation/connected/negotiated fields")
	}
	roles := make(map[string]meshcore.Identity)
	for _, role := range snapshot.Roles {
		id, err := meshcore.NewIdentityFromHex(role.PublicKey)
		if err != nil {
			return err
		}
		roles[role.Role] = id
	}
	remote := newEndpoint("queued-peer", newStockSerialTransport(o.serial, out), cancel)
	host := newEndpoint("onchip-companion", ctransport.NewTCPTransport(ctransport.TCPConfig{
		Address: localAddress(cfg.CompanionListen), BaseConfig: ctransport.BaseConfig{InboundBufferSize: 1024, TxQueueSize: 16},
	}), cancel)
	defer func() {
		if ctx.Err() != nil {
			result = errors.Join(result, context.Cause(ctx))
		}
		result = errors.Join(result, remote.close(), host.close())
	}()
	for _, peer := range []*endpoint{remote, host} {
		if err := peer.handshake(ctx); err != nil {
			return err
		}
		if err := verifyRadio(peer.self, cfg); err != nil {
			return fmt.Errorf("%s read-only profile check: %w", peer.name, err)
		}
	}
	if err := verifyRemoteIdentity(remote.device, remote.self, roles); err != nil {
		return err
	}
	if host.self.PublicKey != roles["companion"].PublicKey() {
		return errors.New("TCP companion identity differs from on-chip status")
	}
	var hostSource remoteSource
	for _, role := range snapshot.Roles {
		if role.Role == "companion" && role.Slot != nil {
			hostSource = remoteSource{Slot: *role.Slot, Generation: role.Generation}
		}
	}
	if hostSource.Generation == 0 {
		return errors.New("on-chip companion source generation missing")
	}
	if err := remote.call(ctx, "native radio stats", func(ctx context.Context) error {
		stats, err := remote.client.GetStats(ctx, protocol.StatsTypeRadio)
		if err != nil {
			return err
		}
		if stats.Radio == nil {
			return errors.New("native radio stats missing")
		}
		return emit("native-peer-readback", map[string]any{
			"public_key": remote.self.Identity().String(), "name": remote.self.Name,
			"model": remote.device.Model, "version": remote.device.FirmwareVersionStr,
			"radio": stats.Radio, "profile": snapshot.Profile,
			"persistence": "active preferences read back; restart not checked",
		})
	}); err != nil {
		return err
	}
	journal := newRemoteJournal(snapshot)
	watchCtx, stopWatch := context.WithCancel(ctx)
	watchDone := make(chan struct{})
	go func() {
		defer close(watchDone)
		tick := time.NewTicker(time.Second)
		defer tick.Stop()
		for {
			select {
			case <-watchCtx.Done():
				return
			case <-tick.C:
				next, err := readRemoteSnapshot(watchCtx, cfg.StatusListen)
				if err == nil {
					err = journal.ingest(next)
				}
				if err != nil {
					if watchCtx.Err() == nil {
						cancel(fmt.Errorf("physical event capture: %w", err))
					}
					return
				}
			}
		}
	}()
	defer func() { stopWatch(); <-watchDone }()
	if err := remoteDiscover(ctx, o.phaseTimeout, remote, host, journal, nil, emit); err != nil {
		return err
	}
	if err := remoteDiscover(ctx, o.phaseTimeout, host, remote, journal, &hostSource, emit); err != nil {
		return err
	}
	journal.mu.Lock()
	peerSource := *journal.selected
	journal.mu.Unlock()
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return err
	}
	token := "queued-" + hex.EncodeToString(nonce[:])
	for _, direction := range []struct {
		from, to *endpoint
	}{{remote, host}, {host, remote}} {
		if err := remoteZeroHop(ctx, direction.from, direction.to.self.Identity()); err != nil {
			return err
		}
	}
	for _, direction := range []struct {
		from, to          *endpoint
		source, ackSource remoteSource
	}{{remote, host, peerSource, hostSource}, {host, remote, hostSource, peerSource}} {
		if err := remoteExchange(ctx, o.phaseTimeout, direction.from, direction.to, token+" "+direction.from.name, journal, direction.source, direction.ackSource, emit); err != nil {
			return err
		}
	}
	stopWatch()
	<-watchDone
	if ctx.Err() != nil {
		return context.Cause(ctx)
	}
	if err := errors.Join(remote.close(), host.close()); err != nil {
		return err
	}
	journal.mu.Lock()
	rxDelta := journal.latest.Totals.RX - journal.initial.Totals.RX
	journal.mu.Unlock()
	return emit("complete", map[string]any{
		"result":          "queued companion and shared modem check passed",
		"external_source": peerSource, "rx_path": "local_reflection",
		"unattributed_physical_rx_counter_delta": rxDelta,
		"unattributed_tx_accepted_delta":         journal.admissionDelta(),
		"persistent_effects":                     "learned contacts and zero-hop routes remain; message queues were consumed; radio profile, device name and clock stay unchanged",
	})
}

func verifyRemoteIdentity(device protocol.DeviceInfoResponse, self protocol.SelfInfoResponse, roles map[string]meshcore.Identity) error {
	model := strings.ToLower(device.Model)
	if device.FirmwareVersion != 13 || strings.TrimPrefix(device.FirmwareVersionStr, "v") != "1.17.1" ||
		!strings.Contains(model, "xiao") || self.Identity().IsZero() || len(roles) != 5 {
		return fmt.Errorf("expected native Xiao companion-v1.17.1/protocol13 and five distinct on-chip identities; model=%q firmware=%q protocol=%d",
			device.Model, device.FirmwareVersionStr, device.FirmwareVersion)
	}
	for role, id := range roles {
		if self.PublicKey == id.PublicKey() {
			return fmt.Errorf("queued peer shares on-chip %s identity", role)
		}
	}
	return nil
}

func remoteDiscover(ctx context.Context, timeout time.Duration, from, to *endpoint, journal *remoteJournal, source *remoteSource, emit func(string, any) error) error {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	var seen atomic.Bool
	record := func(response protocol.Response) {
		switch value := response.Data.(type) {
		case protocol.PushAdvertResponse:
			if value.PublicKey == from.self.PublicKey {
				seen.Store(true)
			}
		case protocol.PushNewAdvertResponse:
			if value.PublicKey == from.self.PublicKey {
				seen.Store(true)
			}
		}
	}
	defer to.client.OnPush(protocol.PushAdvert, record)()
	defer to.client.OnPush(protocol.PushNewAdvert, record)()
	var earliest uint32
	if err := from.call(ctx, "read native advert clock without changing it", func(ctx context.Context) error {
		clock, err := from.client.GetDeviceTime(ctx)
		earliest = clock.Timestamp
		return err
	}); err != nil {
		return err
	}
	if err := to.call(ctx, "check existing advert timestamps", func(ctx context.Context) error {
		contacts, err := to.client.GetContacts(ctx)
		if err != nil {
			return err
		}
		for _, contact := range contacts {
			if contact.PublicKey == from.self.PublicKey && contact.LastAdvert > earliest {
				return fmt.Errorf("saved advert epoch %d is ahead of sender epoch %d; repair the stale contact timestamp before commissioning",
					contact.LastAdvert, earliest)
			}
		}
		return nil
	}); err != nil {
		return err
	}
	mark := journal.mark()
	if err := from.call(ctx, "zero-hop advert", func(ctx context.Context) error { return from.client.SendSelfAdvert(ctx, 0) }); err != nil {
		return err
	}
	var proof remoteJobProof
	var terminal bool
	if err := wait(ctx, func() (bool, error) {
		var err error
		proof, terminal, err = journal.advert(mark, from.self.Identity(), source)
		return terminal && seen.Load(), err
	}); err != nil {
		return fmt.Errorf("%s advert (physical_terminal=%t receiver_push=%t sender_epoch=%d): %w",
			from.name, terminal, seen.Load(), earliest, err)
	}
	if err := to.call(ctx, "received signed advert", func(ctx context.Context) error {
		advert, err := to.client.ExportContact(ctx, from.self.Identity())
		if err != nil {
			return err
		}
		packet, err := meshcore.PacketFromBytes(advert.AdvertData)
		if err != nil {
			return err
		}
		if packet.PayloadType() != meshcore.PayloadTypeAdvert {
			return errors.New("exported contact is not an advert")
		}
		signed, err := meshcore.AdvertFromBytes(packet.Payload)
		if err != nil {
			return err
		}
		if !signed.Verify() || !signed.PublicKey.Matches(from.self.Identity()) || signed.Timestamp < earliest {
			return errors.New("received advert signature/identity mismatch or stale native timestamp")
		}
		_, err = to.client.GetContactByKey(ctx, from.self.Identity())
		return err
	}); err != nil {
		return err
	}
	return emit("shared-modem-advert", map[string]any{
		"sender": from.name, "receiver": to.name, "application_rx": true, "physical_tx": proof, "rx_path": "local_reflection",
		"unattributed_tx_accepted_delta": journal.admissionDelta(),
	})
}

func remoteZeroHop(ctx context.Context, peer *endpoint, identity meshcore.Identity) error {
	return peer.call(ctx, "explicit shared-modem zero-hop route", func(ctx context.Context) error {
		c, err := peer.client.GetContactByKey(ctx, identity)
		if err != nil {
			return err
		}
		if err := peer.client.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
			PublicKey: c.PublicKey, Type: c.Type, Flags: c.Flags, OutPathLen: 0,
			Name: c.AdvertName, LastAdvert: c.LastAdvert, Latitude: c.AdvertLatitude,
			Longitude: c.AdvertLongitude, LastModified: c.LastModified,
		}); err != nil {
			return err
		}
		c, err = peer.client.GetContactByKey(ctx, identity)
		if err == nil && c.OutPathLen != 0 {
			return errors.New("zero-hop route readback differs")
		}
		return err
	})
}

func remoteExchange(ctx context.Context, timeout time.Duration, from, to *endpoint, text string, journal *remoteJournal, source, ackSource remoteSource, emit func(string, any) error) error {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	txMark, rxMark, physicalMark := from.capture.mark(), to.capture.mark(), journal.mark()
	sent, command, err := from.sendText(ctx, to.self.Identity(), text, true)
	if err != nil {
		return err
	}
	var dataProof, ackProof remoteJobProof
	if err := wait(ctx, func() (bool, error) {
		if err := to.drain(ctx); err != nil {
			return false, err
		}
		received, msgOK := message(to.capture.since(rxMark), from.self.Identity(), text, protocol.TxtTypePlain)
		_, ackOK := confirmed(from.capture.since(txMark), sent.Tag)
		if msgOK && received.SenderTimestamp != command.timestamp {
			return false, errors.New("shared-modem text timestamp mismatch")
		}
		journal.mu.Lock()
		events := append([]remoteEvent(nil), journal.events...)
		journal.mu.Unlock()
		data, err := remoteJobs(events, physicalMark, &source, remoteTextMatch(from.self.Identity(), to.self.Identity()))
		if err != nil {
			return false, err
		}
		acks, err := remoteJobs(events, physicalMark, &ackSource, remoteACKMatch(sent.Tag))
		if err != nil {
			return false, err
		}
		if len(data) > 1 {
			return false, errors.New("ambiguous physical text jobs in this phase")
		}
		if len(data) == 1 {
			dataProof = data[0]
		}
		if len(acks) >= 1 {
			ackProof = acks[0]
		}
		return msgOK && ackOK && len(data) == 1 && len(acks) >= 1, nil
	}); err != nil {
		return fmt.Errorf("%s shared-modem text/ACK/physical completion: %w", from.name, err)
	}
	return emit("encrypted-text-and-native-ack", map[string]any{
		"sender": from.name, "receiver": to.name, "text": text, "ack_tag": fmt.Sprintf("%08x", sent.Tag),
		"data_job": dataProof, "ack_job": ackProof, "delivery_medium": "shared-modem local reflection",
		"unattributed_tx_accepted_delta": journal.admissionDelta(),
	})
}
