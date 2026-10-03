package kissproxy

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"sync"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/radio"
)

type Config struct {
	Upstream      string
	Radio         hardware.RadioConfig
	TxPower       uint8
	MaxClients    int
	RequireParity bool
	Logger        *slog.Logger
	// EffectivePHY, when set, replaces Radio/TxPower as the reference for
	// legacy SET_RADIO/SET_TX_POWER: the host follows the mast's live profile.
	// While it reports false the profile is unverified and both are rejected.
	EffectivePHY func() (hardware.RadioConfig, uint8, bool)
}

type Server struct {
	identity meshcore.LocalIdentity
	config   Config
}

func New(identity meshcore.LocalIdentity, cfg Config) *Server {
	if cfg.MaxClients <= 0 {
		cfg.MaxClients = 2
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	return &Server{identity: identity, config: cfg}
}

func (s *Server) Serve(ctx context.Context, listener net.Listener) error {
	ctx, cancel := context.WithCancel(ctx)
	stop := context.AfterFunc(ctx, func() { listener.Close() })
	defer stop()
	var workers sync.WaitGroup
	defer func() {
		cancel()
		workers.Wait()
	}()
	slots := make(chan struct{}, s.config.MaxClients)
	for {
		client, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		select {
		case slots <- struct{}{}:
		default:
			s.config.Logger.Warn("bot KISS connection limit reached")
			client.Close()
			continue
		}
		workers.Add(1)
		go func() {
			defer workers.Done()
			defer func() { <-slots }()
			defer client.Close()
			if err := s.session(ctx, client); err != nil && ctx.Err() == nil &&
				!errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
				s.config.Logger.Error("bot KISS session ended", "error", err)
			}
		}()
	}
}

func (s *Server) session(ctx context.Context, client net.Conn) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	upstream, err := (&net.Dialer{Timeout: 5 * time.Second, KeepAlive: 15 * time.Second}).
		DialContext(ctx, "tcp", s.config.Upstream)
	if err != nil {
		return fmt.Errorf("connecting bot to physical modem: %w", err)
	}
	defer upstream.Close()
	stop := context.AfterFunc(ctx, func() {
		client.Close()
		upstream.Close()
	})
	defer stop()
	out := make(chan []byte, 32)
	failures := make(chan error, 3)
	send := func(wire []byte) error {
		select {
		case out <- wire:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		default:
			return errors.New("slow bot client exceeded output queue")
		}
	}
	var workers sync.WaitGroup
	launch := func(fn func() error) {
		workers.Add(1)
		go func() {
			defer workers.Done()
			failures <- fn()
			cancel()
		}()
	}
	launch(func() error {
		for {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case wire := <-out:
				if err := write(client, wire); err != nil {
					return err
				}
			}
		}
	})
	launch(func() error {
		return readFrames(client, func(frame *hardware.KissFrame) error {
			if response, handled := s.local(frame); handled {
				return send(response)
			}
			return write(upstream, hardware.EncodeFrame(frame.Port, frame.Command, frame.Data))
		})
	})
	launch(func() error {
		signal := true
		var pending []byte
		return readFrames(upstream, func(frame *hardware.KissFrame) error {
			wire := hardware.EncodeFrame(frame.Port, frame.Command, frame.Data)
			if len(pending) > 0 {
				if frame.Command != hardware.KISS_CMD_SETHARDWARE ||
					len(frame.Data) != 3 || frame.Data[0] != hardware.HW_RESP_RX_META {
					return errors.New("physical modem split DATA/RxMeta sequence")
				}
				wire = append(pending, wire...)
				pending = nil
				return send(wire)
			}
			if frame.Command == hardware.KISS_CMD_DATA && signal {
				pending = wire
				return nil
			}
			if frame.Command == hardware.KISS_CMD_SETHARDWARE &&
				len(frame.Data) == 2 && frame.Data[0] == hardware.HwResp(hardware.HW_CMD_GET_SIGNAL_REPORT) {
				signal = frame.Data[1] != 0
			}
			return send(wire)
		})
	})
	err = <-failures
	cancel()
	client.Close()
	upstream.Close()
	workers.Wait()
	return err
}

func write(conn net.Conn, data []byte) error {
	if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return err
	}
	n, err := conn.Write(data)
	if err == nil && n != len(data) {
		return io.ErrShortWrite
	}
	return err
}

func readFrames(conn net.Conn, handler func(*hardware.KissFrame) error) error {
	var remainder []byte
	var deadline time.Time
	buffer := make([]byte, 1024)
	for {
		n, err := conn.Read(buffer)
		if n > 0 {
			frames, rest, failures := hardware.ExtractFrames(append(remainder, buffer[:n]...))
			if len(failures) > 0 {
				return fmt.Errorf("malformed KISS frame: %w", failures[0])
			}
			remainder = rest
			if len(remainder) >= hardware.KISS_MAX_ENCODED_FRAME_SIZE {
				return hardware.ErrFrameTooLarge
			}
			// A lone FEND is idle fill, not a partial frame. Once a body
			// starts, trickling bytes must not extend its completion budget.
			if len(remainder) > 1 {
				if deadline.IsZero() || len(frames) > 0 {
					deadline = time.Now().Add(5 * time.Second)
				}
			} else {
				deadline = time.Time{}
			}
			if err := conn.SetReadDeadline(deadline); err != nil {
				return err
			}
			for _, frame := range frames {
				if err := handler(frame); err != nil {
					return err
				}
			}
		}
		if err != nil {
			return err
		}
	}
}

func hwReply(command byte, payload []byte) []byte {
	return hardware.EncodeHardwareFrame(0, command, payload)
}

func hwError(code byte) ([]byte, bool) {
	return hwReply(hardware.HW_RESP_ERROR, []byte{code}), true
}

// local virtualizes identity and protects the shared PHY from one bot's retuning.
func (s *Server) local(frame *hardware.KissFrame) ([]byte, bool) {
	if frame.Port != 0 {
		return hwError(hardware.HW_ERR_INVALID_PARAM)
	}
	if frame.Command == hardware.KISS_CMD_DATA {
		if len(frame.Data) < 1 || len(frame.Data) > hardware.KISS_MAX_PACKET_SIZE {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		return nil, false
	}
	if frame.Command != hardware.KISS_CMD_SETHARDWARE {
		return hwError(hardware.HW_ERR_INVALID_PARAM)
	}
	if len(frame.Data) == 0 {
		return hwError(hardware.HW_ERR_INVALID_LENGTH)
	}
	command, data := frame.Data[0], frame.Data[1:]
	if code := radio.SharedPHYCommandErrorCode(command, data); code != 0 {
		return hwError(code)
	}
	reply := func(data []byte) ([]byte, bool) {
		return hwReply(hardware.HwResp(command), data), true
	}
	switch command {
	case hardware.HW_CMD_GET_IDENTITY:
		return reply(s.identity.PublicKeyBytes())
	case hardware.HW_CMD_GET_RANDOM:
		if len(data) < 1 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		if data[0] < 1 || data[0] > 64 {
			return hwError(hardware.HW_ERR_INVALID_PARAM)
		}
		random := make([]byte, data[0])
		if _, err := rand.Read(random); err != nil {
			s.config.Logger.Error("bot random generation failed", "error", err)
			return hwError(hardware.HW_ERR_INVALID_PARAM)
		}
		return reply(random)
	case hardware.HW_CMD_VERIFY_SIGNATURE:
		if len(data) < 97 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		peer, err := meshcore.NewIdentityFromBytes(data[:32])
		valid := byte(0)
		if err == nil && peer.Verify(data[96:], data[32:96]) {
			valid = 1
		}
		return reply([]byte{valid})
	case hardware.HW_CMD_SIGN_DATA:
		if len(data) == 0 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		return reply(s.identity.Sign(data))
	case hardware.HW_CMD_ENCRYPT_DATA:
		if len(data) <= 32 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		encrypted, err := meshcore.EncryptThenMAC(data[:32], data[32:])
		if err != nil {
			s.config.Logger.Error("bot encryption failed", "error", err)
			return hwError(hardware.HW_ERR_ENCRYPT_FAILED)
		}
		return reply(encrypted)
	case hardware.HW_CMD_DECRYPT_DATA:
		if len(data) < 50 || (len(data)-34)%16 != 0 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		plaintext, err := meshcore.MACThenDecrypt(data[:32], data[32:])
		if err != nil {
			return hwError(hardware.HW_ERR_MAC_FAILED)
		}
		return reply(plaintext)
	case hardware.HW_CMD_KEY_EXCHANGE:
		if len(data) < 32 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		peer, err := meshcore.NewIdentityFromBytes(data[:32])
		if err != nil {
			return hwError(hardware.HW_ERR_INVALID_PARAM)
		}
		shared, err := s.identity.SharedSecret(peer)
		if err != nil {
			return hwError(hardware.HW_ERR_INVALID_PARAM)
		}
		return reply(shared)
	case hardware.HW_CMD_HASH:
		if len(data) == 0 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		digest := sha256.Sum256(data)
		return reply(digest[:])
	case hardware.HW_CMD_SET_RADIO:
		if len(data) < 10 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		live, _, ok := s.phy()
		if !ok || !bytes.Equal(data[:10], live.ToBytes()) {
			return hwError(hardware.HW_ERR_INVALID_PARAM)
		}
		if s.config.RequireParity {
			return nil, false
		}
		return hwReply(hardware.HW_RESP_OK, nil), true
	case hardware.HW_CMD_SET_TX_POWER:
		if len(data) < 1 {
			return hwError(hardware.HW_ERR_INVALID_LENGTH)
		}
		if _, power, ok := s.phy(); !ok || data[0] != power {
			return hwError(hardware.HW_ERR_INVALID_PARAM)
		}
		if s.config.RequireParity {
			return nil, false
		}
		return hwReply(hardware.HW_RESP_OK, nil), true
	case hardware.HW_CMD_REBOOT:
		return hwError(hardware.HW_ERR_INVALID_PARAM)
	default:
		return nil, false
	}
}

// phy returns the shared profile legacy setters must match.
func (s *Server) phy() (hardware.RadioConfig, uint8, bool) {
	if s.config.EffectivePHY != nil {
		return s.config.EffectivePHY()
	}
	return s.config.Radio, s.config.TxPower, true
}
