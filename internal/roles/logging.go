package roles

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/radio"
)

type packetLogEntry struct {
	Time      time.Time       `json:"time"`
	Direction string          `json:"direction"`
	Local     bool            `json:"local_loopback"`
	Raw       string          `json:"raw_packet_hex"`
	SNR       *float32        `json:"snr"`
	RSSI      *int8           `json:"rssi"`
	TXResult  *radio.TXResult `json:"tx_result,omitempty"`
}

// LogTXResult can be attached with Link.AddTXResultHandler. It queues terminal
// rejection/failure/unknown records without I/O on the physical receive path.
// Successful packets are already captured by AddOutboundHandler; admission is
// not RF success. Logging must be enabled with the normal log start command.
func (s *Service) LogTXResult(result radio.TXResult) {
	if !s.logging.Load() || result.State == radio.TXAccepted || result.State == radio.TXSucceeded {
		return
	}
	s.enqueuePacketLog(packetLogEntry{
		Time: time.Now().UTC(), Direction: "TX_RESULT",
		Raw: hex.EncodeToString(result.PacketBytes()), TXResult: &result,
	})
}

func (s *Service) captureLog(direction string, p *meshcore.Packet) {
	if !s.logging.Load() {
		return
	}
	raw, err := p.ToBytes()
	if err != nil {
		s.report(fmt.Errorf("logging packet: %w", err))
		return
	}
	entry := packetLogEntry{Time: time.Now().UTC(), Direction: direction, Local: localReflection(p), Raw: hex.EncodeToString(raw)}
	if p.HasSignalInfo && !entry.Local {
		snr, rssi := p.SNR, p.RSSI
		entry.SNR, entry.RSSI = &snr, &rssi
	}
	s.enqueuePacketLog(entry)
}

func (s *Service) enqueuePacketLog(entry packetLogEntry) {
	s.ingress.RLock()
	defer s.ingress.RUnlock()
	select {
	case <-s.done:
		return
	default:
	}
	select {
	case s.queue <- event{log: &entry}:
	default:
		s.droppedLogs.Add(1)
	}
}

func (s *Service) checkPacketLog() error {
	info, err := os.Lstat(filepath.Join(s.cfg.StateDir, "packet.log"))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
		return errors.New("packet log must be a private regular file")
	}
	if info.Size() >= s.cfg.MaxLogBytes {
		return errors.New("packet log is full; erase or archive it before restarting capture")
	}
	return nil
}

func (s *Service) preparePacketLog() error {
	if err := s.checkPacketLog(); err != nil {
		return err
	}
	file, err := os.OpenFile(filepath.Join(s.cfg.StateDir, "packet.log"), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	if err := errors.Join(file.Sync(), file.Close()); err != nil {
		return err
	}
	dir, err := os.Open(s.cfg.StateDir)
	if err != nil {
		return errors.Join(ErrCommitUncertain, err)
	}
	if err := errors.Join(dir.Sync(), dir.Close()); err != nil {
		return errors.Join(ErrCommitUncertain, err)
	}
	return nil
}

func (s *Service) appendPacketLog(entry packetLogEntry) error {
	data, err := json.Marshal(entry)
	if err != nil {
		return err
	}
	if err := s.checkPacketLog(); err != nil {
		return err
	}
	file, err := os.OpenFile(filepath.Join(s.cfg.StateDir, "packet.log"), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	info, err := file.Stat()
	if err != nil {
		return errors.Join(err, file.Close())
	}
	if info.Size()+int64(len(data)+1) > s.cfg.MaxLogBytes {
		return errors.Join(errors.New("packet log limit reached; capture stopped without overwriting history"), file.Close())
	}
	_, err = file.Write(append(data, '\n'))
	return errors.Join(err, file.Sync(), file.Close())
}

func (s *Service) erasePacketLog() error {
	path := filepath.Join(s.cfg.StateDir, "packet.log")
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return errors.New("refusing to erase nonregular packet log")
	}
	if err := os.Remove(path); err != nil {
		return err
	}
	dir, err := os.Open(s.cfg.StateDir)
	if err != nil {
		return errors.Join(ErrCommitUncertain, err)
	}
	if err := errors.Join(dir.Sync(), dir.Close()); err != nil {
		return errors.Join(ErrCommitUncertain, err)
	}
	return nil
}

// ReadPacketLog exposes the local-console log surface without sending log
// contents over RF. JSONL is a named host storage-format extension; capture
// stops at MaxLogBytes rather than silently overwriting packets.
func (s *Service) ReadPacketLog() ([]byte, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	path := filepath.Join(s.cfg.StateDir, "packet.log")
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > s.cfg.MaxLogBytes {
		return nil, errors.New("invalid or oversized private packet log")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return io.ReadAll(io.LimitReader(file, s.cfg.MaxLogBytes))
}
