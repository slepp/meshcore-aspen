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
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	ctransport "github.com/meshcore-go/meshcore-go/companion/transport"
	"meshcore.local/meshcore/internal/app"
)

type checker struct {
	cfg                   app.Config
	options               options
	output                *json.Encoder
	host, stock           *endpoint
	roles                 map[string]meshcore.Identity
	password, token       string
	stockLogin, hostLogin bool
	hostThreeByte         bool
}

func run(ctx context.Context, cfg app.Config, o options, password string, out io.Writer) (result error) {
	ctx, cancel := context.WithCancelCause(ctx)
	defer cancel(nil)
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return err
	}
	r := &checker{cfg: cfg, options: o, password: password, token: "stock-check-" + hex.EncodeToString(nonce[:]), output: json.NewEncoder(out)}
	base := ctransport.BaseConfig{InboundBufferSize: 1024, TxQueueSize: 16}
	r.host = newEndpoint("host", ctransport.NewTCPTransport(ctransport.TCPConfig{Address: localAddress(cfg.CompanionListen), BaseConfig: base}), cancel)
	r.stock = newEndpoint("stock", newStockSerialTransport(o.serial, out), cancel)
	defer func() {
		// Do not enqueue cleanup commands on a lost transport: SDK reconnection
		// could otherwise execute them in a later, unrelated session.
		for _, peer := range []*endpoint{r.stock, r.host} {
			if peer.lost.Load() {
				result = errors.Join(result, peer.close())
			}
		}
		cleanupCtx, stop := context.WithTimeout(context.Background(), 10*time.Second)
		defer stop()
		for _, entry := range []struct {
			peer     *endpoint
			loggedIn bool
		}{{r.stock, r.stockLogin}, {r.host, r.hostLogin}} {
			if entry.loggedIn {
				if entry.peer.lost.Load() {
					result = errors.Join(result, fmt.Errorf("%s disconnected: room logout could not be confirmed", entry.peer.name))
					continue
				}
				result = errors.Join(result, entry.peer.call(cleanupCtx, "logout", func(ctx context.Context) error {
					return entry.peer.client.Logout(ctx, r.roles["room"])
				}))
			}
		}
		if ctx.Err() != nil {
			result = errors.Join(result, context.Cause(ctx))
		}
		result = errors.Join(result, r.stock.close(), r.host.close())
		if result == nil {
			result = r.emit("complete", "passed", map[string]any{
				"run":                            r.token,
				"host_three_byte_flood_observed": r.hostThreeByte,
				"coverage":                       []string{"MeshCore companion firmware/version and distinct identity", "RF companion/room/repeater adverts", "encrypted direct text and ACK both directions", "companion room login and post ACK", "two signed room deliveries with recipient ACKs and subscription progress", "three-byte flood path through one repeater"},
				"persistent_effects":             "companion radio profile/name and learned contacts/routes remain; queues were read and test posts remain in room history",
			})
		}
	}()
	for _, phase := range []struct {
		name string
		run  func(context.Context) error
	}{
		{"handshake-and-tuning", r.configure},
		{"ota-advert-discovery", r.discover},
		{"route-learning-host-to-stock", func(ctx context.Context) error { return r.exchange(ctx, r.host, r.stock, "learn-host", false) }},
		{"route-learning-stock-to-host", func(ctx context.Context) error { return r.exchange(ctx, r.stock, r.host, "learn-stock", false) }},
		{"direct-host-to-stock", func(ctx context.Context) error { return r.exchange(ctx, r.host, r.stock, "direct-host", true) }},
		{"direct-stock-to-host", func(ctx context.Context) error { return r.exchange(ctx, r.stock, r.host, "direct-stock", true) }},
		{"room-login-stock", func(ctx context.Context) error { return r.login(ctx, r.stock, true) }},
		{"room-login-host", func(ctx context.Context) error { return r.login(ctx, r.host, false) }},
		{"stock-post-to-room", r.stockPost},
		{"host-post-delivered-to-stock", r.roomDelivery},
		{"three-byte-repeater-forwarding", r.threeByteRelay},
	} {
		if err := r.step(ctx, phase.name, phase.run); err != nil {
			return err
		}
	}
	return nil
}

func (r *checker) emit(phase, status string, details any) error {
	return r.output.Encode(struct {
		Phase   string `json:"phase"`
		Status  string `json:"status"`
		Details any    `json:"details,omitempty"`
	}{phase, status, details})
}

func (r *checker) step(ctx context.Context, name string, f func(context.Context) error) error {
	if err := r.emit(name, "start", nil); err != nil {
		return err
	}
	phaseCtx, cancel := context.WithTimeout(ctx, r.options.phaseTimeout)
	defer cancel()
	err := f(phaseCtx)
	if err == nil && phaseCtx.Err() != nil {
		err = context.Cause(phaseCtx)
	}
	if err != nil {
		details := map[string]any{"error": err.Error()}
		for _, peer := range []*endpoint{r.stock, r.host} {
			peer.capture.mu.Lock()
			details[peer.name+"_captured_events"] = len(peer.capture.events)
			details[peer.name+"_rejected_local_or_invalid_rx"] = peer.capture.rejected
			peer.capture.mu.Unlock()
		}
		return errors.Join(fmt.Errorf("%s: %w", name, err), r.emit(name, "failed", details))
	}
	return r.emit(name, "passed", nil)
}

func wait(ctx context.Context, check func() (bool, error)) error {
	tick := time.NewTicker(250 * time.Millisecond)
	defer tick.Stop()
	for {
		if err := ctx.Err(); err != nil {
			return context.Cause(ctx)
		}
		ok, err := check()
		if err != nil || ok {
			return err
		}
		select {
		case <-ctx.Done():
			return context.Cause(ctx)
		case <-tick.C:
		}
	}
}

func (r *checker) configure(ctx context.Context) error {
	var err error
	r.roles, err = readIdentities(ctx, r.cfg, r.options.onchip)
	if err != nil {
		return err
	}
	if err := r.host.handshake(ctx); err != nil {
		return err
	}
	if r.host.self.PublicKey != r.roles["companion"].PublicKey() {
		return errors.New("TCP endpoint identity does not match public host status")
	}
	if err := verifyRadio(r.host.self, r.cfg); err != nil {
		return fmt.Errorf("host: %w", err)
	}
	if err := r.stock.handshake(ctx); err != nil {
		return err
	}
	if err := verifyStock(r.stock.device, r.stock.self, r.roles); err != nil {
		return err
	}
	if r.cfg.TxPower > r.stock.self.MaxTxPower {
		return errors.New("configured power exceeds companion radio's reported maximum")
	}
	params, err := radioCommand(r.cfg.Radio)
	if err != nil {
		return err
	}
	if err := r.stock.call(ctx, "configure", func(ctx context.Context) error {
		if err := r.stock.client.SetRadioParams(ctx, params.Frequency, params.Bandwidth, params.SpreadFactor, params.CodingRate); err != nil {
			return fmt.Errorf("set radio parameters: %w", err)
		}
		if err := r.stock.client.SetTxPower(ctx, r.cfg.TxPower); err != nil {
			return fmt.Errorf("set TX power: %w", err)
		}
		if err := r.stock.client.SetAdvertName(ctx, r.options.name); err != nil {
			return fmt.Errorf("set advert name: %w", err)
		}
		clock, err := r.stock.client.GetDeviceTime(ctx)
		if err != nil {
			return fmt.Errorf("read device time: %w", err)
		}
		now := uint32(time.Now().Unix())
		if clock.Timestamp <= now {
			if err := r.stock.client.SetDeviceTime(ctx, now); err != nil {
				return fmt.Errorf("advance device time: %w", err)
			}
		} else if err := r.emit("stock-clock", "preserved", map[string]any{
			"device_epoch": clock.Timestamp, "host_epoch": now,
			"ahead_seconds": clock.Timestamp - now,
		}); err != nil {
			return err
		}
		r.stock.self, err = r.stock.client.AppStart(ctx, 3, "meshcore-stock-check")
		return err
	}); err != nil {
		return err
	}
	if err := verifyStock(r.stock.device, r.stock.self, r.roles); err != nil {
		return err
	}
	if err := verifyRadio(r.stock.self, r.cfg); err != nil {
		return fmt.Errorf("companion radio readback: %w", err)
	}
	if r.stock.self.Name != r.options.name {
		return fmt.Errorf("companion radio name readback %q differs", r.stock.self.Name)
	}
	return r.emit("identity", "verified", map[string]any{
		"stock_public_key": r.stock.self.Identity().String(), "host_public_key": r.host.self.Identity().String(),
		"stock_firmware": r.stock.device.FirmwareVersionStr, "stock_protocol": r.stock.device.FirmwareVersion,
		"stock_model": r.stock.device.Model, "stock_build": r.stock.device.FirmwareBuildDate,
		"frequency_khz": params.Frequency, "bandwidth_hz": params.Bandwidth,
		"sf": params.SpreadFactor, "cr": params.CodingRate, "tx_dbm": r.cfg.TxPower,
		"host_origin_path_mode": r.host.device.PathHashMode,
	})
}

func advertRX(events []event, id meshcore.Identity, kind, name string, floodOnly bool) *reception {
	return findRX(events, func(rx *reception) bool {
		if rx.packet.PayloadType() != meshcore.PayloadTypeAdvert {
			return false
		}
		advert, err := meshcore.AdvertFromBytes(rx.packet.Payload)
		return err == nil && advert.PublicKey.Matches(id) && advert.Verify() && advert.AppData().Type == kind &&
			(name == "" || advert.AppData().Name == name) && (!floodOnly || rx.packet.IsRouteFlood())
	})
}

func (r *checker) proof(label, receiver string, rx *reception) error {
	return r.emit(label, "ota", map[string]any{
		"receiver": receiver, "rssi_dbm": rx.rssi, "snr_db": rx.snr,
		"payload_type": rx.packet.PayloadType(), "route": rx.packet.Header & 3,
		"path_hash_bytes": rx.packet.PathHashSize(), "path_hops": rx.packet.PathHashCount(),
		"raw_hex": hex.EncodeToString(rx.raw),
	})
}

func ensureContact(ctx context.Context, peer *endpoint, id meshcore.Identity, advert *reception) error {
	return peer.call(ctx, "discovered contact", func(ctx context.Context) error {
		contacts, err := peer.client.GetContacts(ctx)
		if err != nil {
			return err
		}
		for _, contact := range contacts {
			if contact.PublicKey == id.PublicKey() {
				return nil
			}
		}
		// Import only a signature-verified advert actually captured over RF.
		if err := peer.client.ImportContact(ctx, advert.raw); err != nil {
			return err
		}
		_, err = peer.client.GetContactByKey(ctx, id)
		return err
	})
}

func (r *checker) discover(ctx context.Context) error {
	hostMark, stockMark := r.host.capture.mark(), r.stock.capture.mark()
	var stockAdvert *reception
	nextAdvert := time.Time{}
	if err := wait(ctx, func() (bool, error) {
		stockAdvert = advertRX(r.host.capture.since(hostMark), r.stock.self.Identity(), "CHAT", r.options.name, true)
		if stockAdvert != nil {
			return true, nil
		}
		if time.Now().After(nextAdvert) {
			if err := r.stock.call(ctx, "flood advert", func(ctx context.Context) error { return r.stock.client.SendSelfAdvert(ctx, 1) }); err != nil {
				return false, err
			}
			nextAdvert = time.Now().Add(15 * time.Second)
		}
		return false, nil
	}); err != nil {
		return fmt.Errorf("host did not receive companion advert over RF: %w", err)
	}
	if err := r.proof("stock-advert", "host", stockAdvert); err != nil {
		return err
	}
	adverts := make(map[string]*reception)
	nextAdvert = time.Time{}
	if err := wait(ctx, func() (bool, error) {
		events := r.stock.capture.since(stockMark)
		for role, kind := range map[string]string{"companion": "CHAT", "room": "ROOM", "repeater": "REPEATER"} {
			if adverts[role] == nil {
				adverts[role] = advertRX(events, r.roles[role], kind, "", role == "companion")
			}
		}
		if adverts["companion"] != nil && adverts["room"] != nil && adverts["repeater"] != nil {
			return true, nil
		}
		if adverts["companion"] == nil && time.Now().After(nextAdvert) {
			if err := r.host.call(ctx, "flood advert", func(ctx context.Context) error { return r.host.client.SendSelfAdvert(ctx, 1) }); err != nil {
				return false, err
			}
			nextAdvert = time.Now().Add(15 * time.Second)
		}
		return false, nil
	}); err != nil {
		return fmt.Errorf("companion did not receive host adverts (companion=%t room=%t repeater=%t); enable role adverts on the host: %w",
			adverts["companion"] != nil, adverts["room"] != nil, adverts["repeater"] != nil, err)
	}
	for _, role := range []string{"companion", "room", "repeater"} {
		if err := r.proof(role+"-advert", "stock", adverts[role]); err != nil {
			return err
		}
		if err := ensureContact(ctx, r.stock, r.roles[role], adverts[role]); err != nil {
			return err
		}
	}
	r.hostThreeByte = adverts["companion"].packet.PathHashSize() == 3
	if r.host.device.PathHashMode == 2 && !r.hostThreeByte {
		return errors.New("companion received the wrong path width for the host's mode 2 advert")
	}
	if err := ensureContact(ctx, r.host, r.stock.self.Identity(), stockAdvert); err != nil {
		return err
	}
	if err := ensureContact(ctx, r.host, r.roles["room"], adverts["room"]); err != nil {
		return err
	}
	for _, pair := range []struct{ from, to *endpoint }{{r.host, r.stock}, {r.stock, r.host}} {
		if err := pair.from.call(ctx, "reset route for measured learning", func(ctx context.Context) error {
			return pair.from.client.ResetPath(ctx, pair.to.self.Identity())
		}); err != nil {
			return err
		}
	}
	return nil
}

func (e *endpoint) sendText(ctx context.Context, recipient meshcore.Identity, text string, direct bool) (protocol.SentResponse, sentText, error) {
	var sent protocol.SentResponse
	if direct {
		if err := e.call(ctx, "check learned direct route", func(ctx context.Context) error {
			contact, err := e.client.GetContactByKey(ctx, recipient)
			if err == nil && contact.OutPathLen == protocol.OutPathUnknown {
				return errors.New("peer has no learned route for direct delivery")
			}
			return err
		}); err != nil {
			return sent, sentText{}, err
		}
	}
	err := e.call(ctx, "send encrypted text", func(ctx context.Context) (err error) {
		sent, err = e.client.SendTextMessage(ctx, recipient, text, protocol.TxtTypePlain)
		return err
	})
	e.capture.mu.Lock()
	tx := e.capture.sent
	e.capture.mu.Unlock()
	if err != nil {
		return sent, tx, err
	}
	if !sent.HasExtended || sent.Tag != tx.ack(e.self.Identity()) || tx.text != text {
		return sent, tx, errors.New("SENT did not report the expected sender-key SHA-256 ACK tag")
	}
	if direct && sent.IsFlood {
		return sent, tx, errors.New("firmware sent a flood despite the learned direct route")
	}
	return sent, tx, nil
}

func confirmed(events []event, tag uint32) (protocol.PushSendConfirmedResponse, bool) {
	for _, e := range events {
		if ack, ok := e.response.Data.(protocol.PushSendConfirmedResponse); ok && ack.AckCode == tag {
			return ack, true
		}
	}
	return protocol.PushSendConfirmedResponse{}, false
}

func message(events []event, sender meshcore.Identity, text string, kind byte) (protocol.ContactMsgRecvV3Response, bool) {
	for _, e := range events {
		if msg, ok := e.response.Data.(protocol.ContactMsgRecvV3Response); ok &&
			msg.PubKeyPrefix == sender.Prefix() && msg.Text == text && msg.TxtType == kind {
			return msg, true
		}
	}
	return protocol.ContactMsgRecvV3Response{}, false
}

func ackRX(events []event, tag uint32) *reception {
	return findRX(events, func(rx *reception) bool {
		got, ok := packetACK(rx.packet)
		return ok && got == tag
	})
}

func (r *checker) exchange(ctx context.Context, from, to *endpoint, label string, direct bool) error {
	sendMark, receiveMark := from.capture.mark(), to.capture.mark()
	text := r.token + " " + label
	sent, tx, err := from.sendText(ctx, to.self.Identity(), text, direct)
	if err != nil {
		return err
	}
	var received protocol.ContactMsgRecvV3Response
	var ack protocol.PushSendConfirmedResponse
	var dataRX, replyRX *reception
	var gotMessage, gotACK bool
	if err := wait(ctx, func() (bool, error) {
		if err := to.drain(ctx); err != nil {
			return false, err
		}
		incoming := to.capture.since(receiveMark)
		outgoing := from.capture.since(sendMark)
		received, gotMessage = message(incoming, from.self.Identity(), text, protocol.TxtTypePlain)
		ack, gotACK = confirmed(outgoing, sent.Tag)
		if gotMessage && received.SenderTimestamp != tx.timestamp {
			return false, errors.New("delivered message timestamp differs from the actual command")
		}
		dataRX = findRX(incoming, func(rx *reception) bool {
			return datagramFrom(rx, meshcore.PayloadTypeTxtMsg, from.self.Identity(), to.self.Identity()) &&
				(!direct || rx.packet.IsRouteDirect()) && gotMessage && rx.snr == received.SNR
		})
		replyRX = ackRX(outgoing, sent.Tag)
		if replyRX == nil && sent.IsFlood && gotACK {
			// Flood text can return its ACK inside an encrypted PATH. The normal
			// companion confirms the exact tag; the capture records the RF return.
			replyRX = findRX(outgoing, func(rx *reception) bool {
				return datagramFrom(rx, meshcore.PayloadTypePath, to.self.Identity(), from.self.Identity())
			})
		}
		return gotMessage && gotACK && dataRX != nil && replyRX != nil, nil
	}); err != nil {
		return fmt.Errorf("waiting for V3 text/ACK over RF (message=%t ack=%t data_rx=%t ack_rx=%t): %w",
			gotMessage, gotACK, dataRX != nil, replyRX != nil, err)
	}
	if err := r.proof(label+"-data", to.name, dataRX); err != nil {
		return err
	}
	if err := r.proof(label+"-ack", from.name, replyRX); err != nil {
		return err
	}
	return r.emit(label, "delivered-and-acknowledged", map[string]any{
		"text": text, "tag": fmt.Sprintf("%08x", sent.Tag), "round_trip_ms": ack.RoundTrip,
		"sent_flood": sent.IsFlood, "message_snr_db": received.SNR,
	})
}

func (r *checker) login(ctx context.Context, peer *endpoint, requireRF bool) error {
	mark := peer.capture.mark()
	room := r.roles["room"]
	if requireRF {
		if err := peer.call(ctx, "reset room route for measured login", func(ctx context.Context) error {
			return peer.client.ResetPath(ctx, room)
		}); err != nil {
			return err
		}
	}
	if peer == r.stock {
		r.stockLogin = true
	} else {
		r.hostLogin = true
	}
	if err := peer.call(ctx, "room login", func(ctx context.Context) error {
		return peer.client.SendLogin(ctx, room, r.password)
	}); err != nil {
		return err
	}
	var rx *reception
	var success bool
	if err := wait(ctx, func() (bool, error) {
		events := peer.capture.since(mark)
		success = false
		for _, e := range events {
			switch v := e.response.Data.(type) {
			case protocol.PushLoginFailResponse:
				if v.PubKeyPrefix == room.Prefix() {
					return false, errors.New("room rejected login")
				}
			case protocol.PushLoginSuccessResponse:
				if v.PubKeyPrefix == room.Prefix() {
					if !v.HasServerInfo || v.ACL&3 < 2 {
						return false, errors.New("room login did not grant native posting permission")
					}
					success = true
				}
			}
		}
		rx = findRX(events, func(rx *reception) bool {
			return datagramFrom(rx, meshcore.PayloadTypeResponse, room, peer.self.Identity()) ||
				datagramFrom(rx, meshcore.PayloadTypePath, room, peer.self.Identity())
		})
		return success && (!requireRF || rx != nil), nil
	}); err != nil {
		return fmt.Errorf("room login (authenticated=%t ota_response=%t): %w", success, rx != nil, err)
	}
	if requireRF {
		return r.proof("room-login-response", peer.name, rx)
	}
	return r.emit("host-room-login", "subscribed", "room login delivered locally through the shared modem")
}

func (r *checker) stockPost(ctx context.Context) error {
	mark := r.stock.capture.mark()
	text := r.token + " stock-room"
	sent, _, err := r.stock.sendText(ctx, r.roles["room"], text, true)
	if err != nil {
		return err
	}
	var rx *reception
	if err := wait(ctx, func() (bool, error) {
		events := r.stock.capture.since(mark)
		_, ack := confirmed(events, sent.Tag)
		rx = ackRX(events, sent.Tag)
		return ack && rx != nil, nil
	}); err != nil {
		return fmt.Errorf("room did not acknowledge companion post over RF: %w", err)
	}
	if err := r.proof("stock-room-post-ack", "stock", rx); err != nil {
		return err
	}
	return r.emit("stock-room-post", "acknowledged", map[string]any{"text": text, "tag": fmt.Sprintf("%08x", sent.Tag), "ack_key": r.stock.self.Identity().String()})
}

func (r *checker) roomDelivery(ctx context.Context) error {
	for _, label := range []string{"host-room-1", "host-room-2"} {
		if err := r.deliverRoomPost(ctx, label); err != nil {
			return err
		}
	}
	return r.emit("room-subscription-progress", "verified", "a second distinct post followed the first recipient-key ACK; delivery did not stall behind an unacknowledged post")
}

func (r *checker) deliverRoomPost(ctx context.Context, label string) error {
	stockMark, hostMark := r.stock.capture.mark(), r.host.capture.mark()
	room := r.roles["room"]
	text := r.token + " " + label
	sent, _, err := r.host.sendText(ctx, room, text, false)
	if err != nil {
		return err
	}
	var delivery protocol.ContactMsgRecvV3Response
	var dataRX, replyRX *reception
	var attempt int
	var found, producerACK bool
	if err := wait(ctx, func() (bool, error) {
		if err := r.stock.drain(ctx); err != nil {
			return false, err
		}
		stockEvents, hostEvents := r.stock.capture.since(stockMark), r.host.capture.since(hostMark)
		_, producerACK = confirmed(hostEvents, sent.Tag)
		delivery, found = message(stockEvents, room, text, protocol.TxtTypeSignedPlain)
		if !found {
			return false, nil
		}
		author := r.host.self.PublicKey
		if !bytes.Equal(delivery.SenderPrefix, author[:4]) {
			return false, errors.New("room delivery does not identify the TCP companion as its author")
		}
		tags, err := signedACKs(delivery, r.stock.self.Identity())
		if err != nil {
			return false, err
		}
		dataRX = findRX(stockEvents, func(rx *reception) bool {
			return datagramFrom(rx, meshcore.PayloadTypeTxtMsg, room, r.stock.self.Identity()) && rx.snr == delivery.SNR
		})
		for i, tag := range tags {
			if rx := ackRX(hostEvents, tag); rx != nil {
				replyRX, attempt = rx, i
				break
			}
		}
		return producerACK && dataRX != nil && replyRX != nil, nil
	}); err != nil {
		return fmt.Errorf("%s: signed_post=%t producer_ack=%t ota_data=%t recipient_ack=%t; encrypted PATH ACKs cannot be decoded without private keys: %w",
			label, found, producerACK, dataRX != nil, replyRX != nil, err)
	}
	if err := r.proof("room-signed-delivery", "stock", dataRX); err != nil {
		return err
	}
	if err := r.proof("stock-recipient-ack", "host", replyRX); err != nil {
		return err
	}
	tag, _ := packetACK(replyRX.packet)
	return r.emit("room-delivery", "received-and-acknowledged", map[string]any{
		"text": text, "author_prefix": hex.EncodeToString(delivery.SenderPrefix),
		"recipient_public_key": r.stock.self.Identity().String(),
		"recipient_ack":        fmt.Sprintf("%08x", tag), "attempt_bits": attempt,
		"message_snr_db": delivery.SNR,
	})
}
