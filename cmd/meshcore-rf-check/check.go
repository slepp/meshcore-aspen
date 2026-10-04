package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
	"github.com/meshcore-go/meshcore-go/companion/transport"
	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/app"
	"meshcore.local/meshcore/internal/radio"
)

type checker struct {
	cfg      app.Config
	opts     options
	ids      identities
	password string
	output   io.Writer
	logger   *slog.Logger
	end      time.Time
}
type session struct {
	client    *client.Client
	login     chan protocol.PushLoginSuccessResponse
	confirmed chan protocol.PushSendConfirmedResponse
}
type cleanup func(context.Context) error

func sentACK(sent protocol.SentResponse) (uint32, error) {
	if sent.HasExtended {
		return sent.Tag, nil
	}
	if sent.HasAckCode {
		return sent.AckCode, nil
	}
	return 0, errors.New("private send response has no ACK tag")
}

func runCleanups(ctx context.Context, actions []cleanup) error {
	var failures error
	for i := len(actions) - 1; i >= 0; i-- {
		if err := actions[i](ctx); err != nil {
			failures = errors.Join(failures, err)
		}
	}
	return failures
}

func (c *checker) run(ctx context.Context) (result error) {
	var cleanups []cleanup
	defer func() {
		end := c.end
		if shorter := time.Now().Add(10 * time.Second); shorter.Before(end) {
			end = shorter
		}
		cleanupCtx, cancel := context.WithDeadline(context.Background(), end)
		defer cancel()
		failures := runCleanups(cleanupCtx, cleanups)
		if failures != nil {
			result = errors.Join(result, fmt.Errorf("cleanup: %w", failures))
		} else {
			fmt.Fprintf(c.output, "PASS stage=cleanup actions=%d\n", len(cleanups))
		}
		if result == nil {
			fmt.Fprintln(c.output, "PASS stage=complete")
		}
	}()
	var sessions [2]*session
	var channelSlot byte
	for i := range sessions {
		connection := client.New(transport.NewTCPTransport(transport.TCPConfig{
			Address:    dialAddress(c.cfg.CompanionListen),
			BaseConfig: transport.BaseConfig{Logger: c.logger, TxQueueSize: 8, InboundBufferSize: 128},
		}))
		cleanups = append(cleanups, func(context.Context) error { return connection.Close() })
		s := &session{client: connection, login: make(chan protocol.PushLoginSuccessResponse, 8), confirmed: make(chan protocol.PushSendConfirmedResponse, 8)}
		connection.OnPush(protocol.PushLoginSuccess, func(r protocol.Response) {
			if p, ok := r.Data.(protocol.PushLoginSuccessResponse); ok {
				select {
				case s.login <- p:
				default:
				}
			}
		})
		connection.OnPush(protocol.PushSendConfirmed, func(r protocol.Response) {
			if p, ok := r.Data.(protocol.PushSendConfirmedResponse); ok {
				select {
				case s.confirmed <- p:
				default:
				}
			}
		})
		step, cancel := context.WithTimeout(ctx, 5*time.Second)
		err := connection.Connect(step)
		if err == nil {
			var info protocol.DeviceInfoResponse
			info, err = connection.DeviceQuery(step)
			if err == nil && info.MaxChannels == 0 {
				err = errors.New("companion advertises no channels")
			}
			if err == nil {
				if i == 0 {
					channelSlot = info.MaxChannels - 1
				} else if channelSlot != info.MaxChannels-1 {
					err = errors.New("clients disagree on channel capacity")
				}
			}
		}
		if err == nil {
			var self protocol.SelfInfoResponse
			self, err = connection.AppStart(step, 3, "MeshCore RF Check")
			if err == nil && !self.Identity().Matches(c.ids.base) {
				err = errors.New("companion client identity differs from host status")
			}
			if err == nil && (self.RadioFrequency != c.cfg.Radio.FreqHz/1000 || self.RadioBandwidth != c.cfg.Radio.BwHz ||
				self.RadioSpreadFactor != c.cfg.Radio.SF || self.RadioCodingRate != c.cfg.Radio.CR || self.TxPower != c.cfg.TxPower) {
				err = errors.New("running companion PHY differs from the supplied isolated configuration")
			}
		}
		cancel()
		if err != nil {
			return fmt.Errorf("companion client %d setup: %w", i+1, err)
		}
		sessions[i] = s
	}
	fmt.Fprintln(c.output, "PASS stage=companion_clients clients=2 shared_identity=1")
	contacts, err := sessions[0].client.GetContacts(ctx)
	if err != nil {
		return fmt.Errorf("discover role contacts: %w", err)
	}
	roomSeen, repeaterSeen := false, false
	for _, contact := range contacts {
		roomSeen = roomSeen || contact.Identity().Matches(c.ids.room) && contact.Type == meshcore.AdvertTypeRoom
		repeaterSeen = repeaterSeen || contact.Identity().Matches(c.ids.repeater) && contact.Type == meshcore.AdvertTypeRepeater
	}
	if !roomSeen || !repeaterSeen {
		return errors.New("discover role contacts: signed room/repeater adverts are missing from companion")
	}
	prior, err := sessions[0].client.GetChannel(ctx, channelSlot)
	if err != nil {
		return fmt.Errorf("save channel slot: %w", err)
	}
	cleanups = append(cleanups, func(ctx context.Context) error {
		if err := sessions[0].client.SetChannel(ctx, channelSlot, prior.Name, prior.Secret); err != nil {
			return fmt.Errorf("restore channel: %w", err)
		}
		got, err := sessions[0].client.GetChannel(ctx, channelSlot)
		if err == nil && (got.Name != prior.Name || got.Secret != prior.Secret) {
			err = errors.New("restored channel differs from original")
		}
		return err
	})
	var psk [16]byte
	var random [8]byte
	if _, err := rand.Read(psk[:]); err != nil {
		return err
	}
	if _, err := rand.Read(random[:]); err != nil {
		return err
	}
	token := hex.EncodeToString(random[:])
	if err := sessions[0].client.SetChannel(ctx, channelSlot, "rf-check-"+token, psk); err != nil {
		return fmt.Errorf("install temporary channel: %w", err)
	}
	for i, s := range sessions {
		got, err := s.client.GetChannel(ctx, channelSlot)
		if err != nil {
			return fmt.Errorf("client %d test channel: %w", i+1, err)
		}
		if got.Secret != psk {
			return fmt.Errorf("client %d did not observe shared test channel", i+1)
		}
	}
	probe, err := meshcore.GenerateLocalIdentity(rand.Reader)
	if err != nil {
		return err
	}
	link, err := radio.Open(ctx, radio.Config{Address: c.opts.peer, Radio: c.cfg.Radio, TxPower: c.cfg.TxPower, Logger: c.logger})
	if err != nil {
		return fmt.Errorf("secondary physical KISS setup: %w", err)
	}
	cleanups = append(cleanups, func(context.Context) error { return link.Close() })
	received := newCapture()
	link.AddOutboundHandler(received.sent)
	cleanups = append(cleanups, func(context.Context) error {
		c.linkEvidence(link, received, "secondary_final_counters")
		return nil
	})
	rf, stop := link.Radio()
	cleanups = append(cleanups, func(context.Context) error { err := rf.Close(); stop(); return err })
	rf.SetRawDataHandler(received.receive)
	fmt.Fprintf(c.output, "INFO stage=secondary_phy primary=%s peer=%s frequency_hz=%d bandwidth_hz=%d sf=%d cr=%d tx_dbm=%d secondary_readback=verified primary_companion_phy=matched\n",
		c.cfg.RadioAddress, c.opts.peer, c.cfg.Radio.FreqHz, c.cfg.Radio.BwHz, c.cfg.Radio.SF, c.cfg.Radio.CR, c.cfg.TxPower)
	c.linkEvidence(link, received, "secondary_initial_counters")

	upstream := "rf-up-" + token
	flood, err := groupPacket(psk, upstream, uint32(time.Now().Unix()))
	if err != nil {
		return err
	}
	originalWire, err := flood.ToBytes()
	if err != nil {
		return err
	}
	mqttSubscriber, err := openMQTTProbe(ctx, c.cfg.MQTT, c.ids.observer, originalWire)
	if err != nil {
		return err
	}
	cleanups = append(cleanups, func(context.Context) error { mqttSubscriber.Close(); return nil })
	repeaterKey := c.ids.repeater.PublicKey()
	fmt.Fprintf(c.output, "INFO stage=repeater_forward expected_appended_path=%x path_hash_bytes=3\n", repeaterKey[:3])
	if err := c.sendVerified(ctx, rf, received, flood, "secondary_group_transmit"); err != nil {
		return fmt.Errorf("secondary group transmit: %w", err)
	}
	if err := c.rfStage(ctx, received, "repeater_forward", 25*time.Second, func(p *meshcore.Packet) bool { return forwardedBy(p, flood, c.ids.repeater) }); err != nil {
		return err
	}
	mqttContext, mqttCancel := context.WithTimeout(ctx, 15*time.Second)
	mqttEvidence, err := mqttSubscriber.wait(mqttContext)
	mqttCancel()
	if err != nil {
		return err
	}
	fmt.Fprintf(c.output, "PASS stage=rf_observer_mqtt matched_original_packets=1 local_loopback=false snr_db=%.2f rssi_dbm=%d\n", mqttEvidence.snr, mqttEvidence.rssi)
	if err := c.messagesStage(ctx, sessions, "rf_group_to_both_clients", 20*time.Second, func(m client.WaitingMessage) bool {
		return m.Channel != nil && m.Channel.ChannelIdx == channelSlot && m.Channel.Text == "rf-check: "+upstream
	}); err != nil {
		return err
	}
	if err := c.baseLogin(ctx, sessions); err != nil {
		return err
	}
	secret, err := probe.SharedSecret(c.ids.room)
	if err != nil {
		return err
	}
	stamp := uint32(time.Now().Unix())
	login, err := roomLogin(probe, c.ids.room, secret, stamp, c.password)
	if err != nil {
		return err
	}
	if err := c.sendVerified(ctx, rf, received, login, "remote_room_login_transmit"); err != nil {
		return fmt.Errorf("remote room login transmit: %w", err)
	}
	step, cancel := context.WithTimeout(ctx, 30*time.Second)
	var roomPath route
	loginFrame, err := received.wait(step, "remote_room_login", func(p *meshcore.Packet) bool {
		path, ok := loginResponse(p, probe.Identity, c.ids.room, secret)
		if ok {
			roomPath = path
		}
		return ok
	})
	cancel()
	if err != nil {
		return err
	}
	c.rfEvidence("remote_room_login", received, loginFrame)
	fmt.Fprintln(c.output, "NOTICE stage=room_cleanup probe_membership_retained=1 reason=no_guest_membership_removal_protocol")
	pathReply, err := reciprocalPath(probe, c.ids.room, secret, loginFrame.packet, roomPath)
	if err != nil {
		return err
	}
	if err := c.sendVerified(ctx, rf, received, pathReply, "room_return_path_transmit"); err != nil {
		return fmt.Errorf("return room path: %w", err)
	}
	postText := "rf-room-" + token
	postBody := textPlain(stamp+1, postText)
	post, err := datagram(probe, c.ids.room, secret, meshcore.PayloadTypeTxtMsg, postBody, roomPath)
	if err != nil {
		return err
	}
	if err := c.sendVerified(ctx, rf, received, post, "remote_room_post_transmit"); err != nil {
		return fmt.Errorf("remote room post transmit: %w", err)
	}
	wantACK := ackProof(postBody, probe.Identity)
	if err := c.rfStage(ctx, received, "remote_room_post_ack", 25*time.Second, func(p *meshcore.Packet) bool { return matchesACK(p, wantACK) }); err != nil {
		return err
	}
	probePrefix := probe.PublicKey()
	if err := c.messagesStage(ctx, sessions, "remote_author_room_post_to_both_clients", 40*time.Second, func(m client.WaitingMessage) bool {
		return m.Contact != nil && m.Contact.PubKeyPrefix == c.ids.room.Prefix() && m.Contact.TxtType == 2 &&
			bytes.Equal(m.Contact.SenderPrefix, probePrefix[:4]) && m.Contact.Text == postText
	}); err != nil {
		return err
	}
	if err := c.downlink(ctx, sessions[0], rf, received, probe, psk, channelSlot, token, &cleanups); err != nil {
		return err
	}
	if received.dropped.Load() != 0 {
		return errors.New("RF packet capture full; packets were lost")
	}
	fmt.Fprintf(c.output, "PASS stage=rf_reception physical_rx=%d rejected_local_or_unmeasured=%d\n", received.physical.Load(), received.rejected.Load())
	return nil
}

func (c *checker) rfEvidence(stage string, received *capture, frame reception) {
	fmt.Fprintf(c.output, "PASS stage=%s physical_rx=%d snr_db=%.2f rssi_dbm=%d hops=%d\n",
		stage, received.physical.Load(), frame.snr, frame.rssi, frame.packet.PathHashCount())
}

func (c *checker) linkEvidence(link *radio.Link, received *capture, stage string) {
	sample, err := link.Snapshot()
	if err != nil {
		fmt.Fprintf(c.output, "INFO stage=%s online=%t completed_tx=%d telemetry_error=%q\n", stage, link.Online(), received.transmitted.Load(), err.Error())
		return
	}
	fmt.Fprintf(c.output, "INFO stage=%s completed_tx=%d counters_valid=%t firmware_rx=%d firmware_tx=%d firmware_errors=%d noise_valid=%t noise_dbm=%d rssi_valid=%t current_rssi_dbm=%d\n",
		stage, received.transmitted.Load(), sample.HasCounters, sample.Counters.PacketsRecv, sample.Counters.PacketsSent, sample.Counters.PacketsErrors,
		sample.HasNoiseFloor, sample.NoiseFloorDBm, sample.HasCurrentRSSI, sample.CurrentRSSIDBm)
}

func (c *checker) sendVerified(ctx context.Context, rf node.Radio, received *capture, packet *meshcore.Packet, stage string) error {
	raw, err := packet.ToBytes()
	if err != nil {
		return err
	}
	fingerprint := sha256.Sum256(packet.Payload)
	fmt.Fprintf(c.output, "INFO stage=%s event=enqueue header=0x%02x payload_type=%d path_len=0x%02x wire_bytes=%d payload_sha256_prefix=%x\n",
		stage, packet.Header, packet.PayloadType(), packet.PathLength, len(raw), fingerprint[:8])
	if err := rf.SendData(raw); err != nil {
		return err
	}
	step, cancel := context.WithTimeout(ctx, 32*time.Second)
	defer cancel()
	if err := received.waitTX(step, stage, raw); err != nil {
		return err
	}
	fmt.Fprintf(c.output, "PASS stage=%s event=tx_done completed_tx=%d payload_sha256_prefix=%x\n", stage, received.transmitted.Load(), fingerprint[:8])
	return nil
}
func (c *checker) rfStage(ctx context.Context, received *capture, name string, timeout time.Duration, match func(*meshcore.Packet) bool) error {
	step, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	frame, err := received.wait(step, name, match)
	if err != nil {
		return err
	}
	c.rfEvidence(name, received, frame)
	return nil
}
func (c *checker) messagesStage(ctx context.Context, sessions [2]*session, name string, timeout time.Duration, match func(client.WaitingMessage) bool) error {
	step, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	found := [2]bool{}
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	for {
		for i, s := range sessions {
			if found[i] {
				continue
			}
			messages, err := s.client.GetWaitingMessages(step)
			if err != nil {
				return fmt.Errorf("%s client %d: %w", name, i+1, err)
			}
			for _, message := range messages {
				if match(message) {
					found[i] = true
				}
			}
		}
		if found[0] && found[1] {
			fmt.Fprintf(c.output, "PASS stage=%s independent_clients=2\n", name)
			return nil
		}
		select {
		case <-ticker.C:
		case <-step.Done():
			return fmt.Errorf("%s: %w (client1=%t client2=%t)", name, step.Err(), found[0], found[1])
		}
	}
}
func (c *checker) baseLogin(ctx context.Context, sessions [2]*session) error {
	step, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()
	if err := sessions[0].client.SendLogin(step, c.ids.room, c.password); err != nil {
		return fmt.Errorf("base room login: %w", err)
	}
	for i, s := range sessions {
		for {
			select {
			case push := <-s.login:
				if push.PubKeyPrefix != c.ids.room.Prefix() {
					continue
				}
				if !push.HasServerInfo || push.ACL&3 < 2 {
					return fmt.Errorf("base room login client %d: missing read/write permissions", i+1)
				}
			case <-step.Done():
				return fmt.Errorf("base room login client %d: %w", i+1, step.Err())
			}
			break
		}
	}
	fmt.Fprintln(c.output, "PASS stage=base_room_login clients=2")
	return nil
}
func (c *checker) downlink(ctx context.Context, s *session, rf node.Radio, received *capture, probe meshcore.LocalIdentity, psk [16]byte, slot byte, token string, cleanups *[]cleanup) error {
	groupText := "rf-down-" + token
	if _, err := s.client.SendChannelTextMessage(ctx, slot, groupText, 0); err != nil {
		return fmt.Errorf("base group downlink: %w", err)
	}
	if err := c.rfStage(ctx, received, "base_group_decrypted_on_secondary", 25*time.Second, func(p *meshcore.Packet) bool { return groupMatches(p, psk, groupText) }); err != nil {
		return err
	}
	contacts, err := s.client.GetContacts(ctx)
	if err != nil {
		return err
	}
	for _, contact := range contacts {
		if contact.Identity().Matches(probe.Identity) {
			return errors.New("random probe identity unexpectedly exists in companion contacts")
		}
	}
	*cleanups = append(*cleanups, func(ctx context.Context) error {
		contacts, err := s.client.GetContacts(ctx)
		if err != nil {
			return fmt.Errorf("inspect probe cleanup: %w", err)
		}
		for _, contact := range contacts {
			if contact.Identity().Matches(probe.Identity) {
				if err := s.client.RemoveContact(ctx, probe.Identity); err != nil {
					return fmt.Errorf("remove temporary probe contact: %w", err)
				}
			}
		}
		contacts, err = s.client.GetContacts(ctx)
		if err != nil {
			return fmt.Errorf("verify probe cleanup: %w", err)
		}
		for _, contact := range contacts {
			if contact.Identity().Matches(probe.Identity) {
				return errors.New("temporary probe contact survived removal")
			}
		}
		return nil
	})
	if err := s.client.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: probe.PublicKey(), Type: meshcore.AdvertTypeChat, OutPathLen: 0, Name: "MeshCore RF Check",
		LastModified: uint32(time.Now().Unix()),
	}); err != nil {
		return fmt.Errorf("add temporary probe contact: %w", err)
	}
	text := "rf-private-" + token
	step, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()
	sent, err := s.client.SendTextMessage(step, probe.Identity, text, 0)
	if err != nil {
		return fmt.Errorf("base private downlink: %w", err)
	}
	declaredACK, err := sentACK(sent)
	if err != nil {
		return err
	}
	secret, err := probe.SharedSecret(c.ids.base)
	if err != nil {
		return err
	}
	var plain []byte
	frame, err := received.wait(step, "base_private_decrypted_on_secondary", func(p *meshcore.Packet) bool {
		body, err := privatePlain(p, probe.Identity, c.ids.base, secret, text)
		if err == nil {
			plain = body
		}
		return err == nil
	})
	if err != nil {
		return err
	}
	c.rfEvidence("base_private_decrypted_on_secondary", received, frame)
	crc := ackProof(plain, c.ids.base)
	if declaredACK != crc {
		return errors.New("base private ACK tag differs from computed SHA-256")
	}
	ack := &meshcore.Packet{Header: 0x0e, Payload: binary.LittleEndian.AppendUint32(nil, crc)}
	if err := c.sendVerified(step, rf, received, ack, "secondary_private_ack_transmit"); err != nil {
		return fmt.Errorf("secondary private ACK transmit: %w", err)
	}
	for {
		select {
		case confirmation := <-s.confirmed:
			if confirmation.AckCode != crc {
				continue
			}
			fmt.Fprintln(c.output, "PASS stage=private_ack_round_trip confirmations=1")
			return nil
		case <-step.Done():
			return fmt.Errorf("private ACK round trip: %w", step.Err())
		}
	}
}
