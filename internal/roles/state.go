package roles

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/policy"
	hoststate "meshcore.local/meshcore/internal/state"
)

const maxPostBytes = 151
const maxStateBytes = 8 << 20

// Native storePost passes a 151-byte buffer size to StrHelper::strncpy.
const nativePostBytes = 150

type RetentionProfile string

const (
	NativeRetention RetentionProfile = "native"
	DurableReplay   RetentionProfile = "durable-replay"
)

type PreferenceProfile string

const (
	NativePreferences      PreferenceProfile = "native-preferences"
	DurableHostPreferences PreferenceProfile = "durable-host-preferences"
)

var ErrCommitUncertain = errors.New("role commit outcome indeterminate; restart and reconcile state")

type member struct {
	Key           [32]byte
	Permissions   byte
	LastTimestamp uint32
	SyncSince     uint32
	Path          []byte
	PathLength    byte
	KnownPath     bool
	Attempt       byte
	LastActivity  uint32 `json:"-"`

	Active      bool      `json:"-"`
	PendingACK  uint32    `json:"-"`
	Pending     bool      `json:"-"`
	PendingPost uint32    `json:"-"`
	Deadline    time.Time `json:"-"`
	Failures    int       `json:"-"`
}

type post struct {
	Author    [32]byte
	Timestamp uint32
	Text      string
	RawText   []byte `json:",omitempty"`
}

func (p post) text() string {
	if p.RawText != nil {
		return string(p.RawText)
	}
	return p.Text
}

type diskState struct {
	Version               int
	Retention             RetentionProfile   `json:",omitempty"`
	PreferenceProfile     PreferenceProfile  `json:",omitempty"`
	Preferences           policy.Preferences `json:",omitempty"`
	Name                  string             `json:",omitempty"`
	AllowReadOnly         bool               `json:",omitempty"`
	MultiACKs             uint8              `json:",omitempty"`
	Latitude              float64            `json:",omitempty"`
	Longitude             float64            `json:",omitempty"`
	AdvertLocation        uint8              `json:",omitempty"`
	RTCOffset             int64              `json:",omitempty"`
	HomeRegion            uint16             `json:",omitempty"`
	NextRegionID          uint32             `json:",omitempty"`
	DefaultRegion         uint16             `json:",omitempty"`
	ManagedDefaultRegion  bool               `json:",omitempty"`
	DiscoveryModified     uint32             `json:",omitempty"`
	AdminPasswordOverride *string            `json:",omitempty"`
	GuestPasswordOverride *string            `json:",omitempty"`
	AdminPasswordBytes    []byte             `json:",omitempty"`
	GuestPasswordBytes    []byte             `json:",omitempty"`
	Logging               bool               `json:"-"`
	Identity              string
	Room                  bool
	Clock                 uint32
	Members               map[string]*member
	MemberOrder           []string `json:",omitempty"`
	History               []post
	Posted                uint64
	Pushed                uint64
}

func (d diskState) clone() diskState {
	out := d
	out.AdminPasswordBytes = bytes.Clone(d.AdminPasswordBytes)
	out.GuestPasswordBytes = bytes.Clone(d.GuestPasswordBytes)
	out.MemberOrder = append([]string(nil), d.MemberOrder...)
	out.History = append([]post(nil), d.History...)
	for i := range out.History {
		out.History[i].RawText = bytes.Clone(out.History[i].RawText)
	}
	out.Members = make(map[string]*member, len(d.Members))
	for k, v := range d.Members {
		c := *v
		c.Path = bytes.Clone(v.Path)
		out.Members[k] = &c
	}
	return out
}

func (s *Service) load() error {
	if err := os.MkdirAll(s.cfg.StateDir, 0700); err != nil {
		return err
	}
	dir, err := os.Lstat(s.cfg.StateDir)
	if err != nil || !dir.IsDir() || dir.Mode()&os.ModeSymlink != 0 {
		return errors.New("role state directory must be a real directory")
	}
	if err := os.Chmod(s.cfg.StateDir, 0700); err != nil {
		return err
	}
	s.state = diskState{Version: 2, Retention: s.cfg.Retention, Identity: s.id.Identity.String(), Room: s.room, Members: make(map[string]*member)}
	s.state.Preferences = policy.Defaults(s.profile)
	s.state.Name = s.cfg.Name
	path := filepath.Join(s.cfg.StateDir, "state.json")
	data, err := hoststate.ReadRoleJSON(path)
	if errors.Is(err, os.ErrNotExist) {
		if s.state.Retention == "" {
			s.state.Retention = NativeRetention
		}
		s.state.PreferenceProfile = s.cfg.PreferenceProfile
		if s.state.PreferenceProfile == "" {
			s.state.PreferenceProfile = NativePreferences
		}
		return nil
	}
	if err != nil {
		return err
	}
	if len(data) > maxStateBytes {
		return errors.New("role state exceeds 8 MiB")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&s.state); err != nil {
		return fmt.Errorf("reading role state: %w", err)
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return errors.New("trailing data in role state")
	}
	d := &s.state
	if len(d.Name) == 0 || len(d.Name) > 31 || !utf8.ValidString(d.Name) || strings.ContainsRune(d.Name, 0) ||
		d.MultiACKs > 1 || (d.AdvertLocation != 0 && d.AdvertLocation != 2) || math.IsNaN(d.Latitude) || math.IsNaN(d.Longitude) ||
		math.Abs(d.Latitude) > 90 || math.Abs(d.Longitude) > 180 || d.RTCOffset < -int64(^uint32(0)) || d.RTCOffset > int64(^uint32(0)) {
		return errors.New("invalid persisted application preferences")
	}
	for _, stored := range []struct {
		text **string
		raw  *[]byte
	}{
		{&d.AdminPasswordOverride, &d.AdminPasswordBytes},
		{&d.GuestPasswordOverride, &d.GuestPasswordBytes},
	} {
		if *stored.raw != nil {
			if *stored.text != nil {
				return errors.New("ambiguous persisted management password")
			}
			value := string(*stored.raw)
			*stored.text, *stored.raw = &value, nil
		}
		password := *stored.text
		if password != nil && (len(*password) > 15 || strings.ContainsRune(*password, 0)) {
			return errors.New("invalid persisted management password")
		}
	}
	if (d.Version != 1 && d.Version != 2) || d.Identity != s.id.Identity.String() || d.Room != s.room {
		return errors.New("role state version, identity or role mismatch")
	}
	if d.Version == 1 {
		if err := preserveLegacyState(data, s.cfg.StateDir); err != nil {
			return err
		}
		d.Version, d.Retention = 2, DurableReplay
	}
	// Older host documents preserved runtime preferences across restarts.
	if d.PreferenceProfile == "" {
		d.PreferenceProfile = DurableHostPreferences
	}
	if d.PreferenceProfile != NativePreferences && d.PreferenceProfile != DurableHostPreferences {
		return errors.New("invalid persisted preference profile")
	}
	if s.cfg.PreferenceProfile != "" {
		d.PreferenceProfile = s.cfg.PreferenceProfile
	}
	if d.PreferenceProfile == NativePreferences && d.Preferences.AirtimeFactor > 9 {
		d.Preferences.AirtimeFactor = 9
	}
	if d.Retention != NativeRetention && d.Retention != DurableReplay {
		return errors.New("invalid persisted retention profile")
	}
	if s.cfg.Retention != "" && s.cfg.Retention != d.Retention {
		if s.cfg.Retention == NativeRetention && len(d.History) > 0 {
			return errors.New("native retention would discard saved history; retain durable-replay or explicitly archive it first")
		}
		d.Retention = s.cfg.Retention
	}
	if len(d.Members) > s.cfg.MaxMembers || len(d.History) > s.cfg.MaxHistory {
		return errors.New("persisted role state exceeds configured bounds")
	}
	if d.Members == nil {
		d.Members = make(map[string]*member)
	}
	for key, m := range d.Members {
		if m == nil || meshcore.NewIdentity(m.Key).String() != key ||
			!meshcore.IsValidPathLen(m.PathLength) ||
			len(m.Path) != int(m.PathLength&63)*int((m.PathLength>>6)+1) {
			return errors.New("invalid member in role state")
		}
		if _, err := s.id.SharedSecret(meshcore.NewIdentity(m.Key)); err != nil {
			return fmt.Errorf("invalid member public key: %w", err)
		}
	}
	seen := make(map[string]bool)
	for _, key := range d.MemberOrder {
		if seen[key] || d.Members[key] == nil {
			return errors.New("invalid persisted member ordering")
		}
		seen[key] = true
	}
	d.MemberOrder = s.memberKeys()
	var previous uint32
	for _, p := range d.History {
		if validPost(p.text()) != nil || p.Timestamp <= previous ||
			p.Timestamp > d.Clock || bytes.IndexByte([]byte(p.text()), 0) >= 0 ||
			(p.RawText != nil && p.Text != "") {
			return errors.New("invalid post history in role state")
		}
		previous = p.Timestamp
	}
	return nil
}

func preserveLegacyState(data []byte, dir string) error {
	backup := filepath.Join(dir, "state.v1.json")
	file, err := os.OpenFile(backup, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if errors.Is(err, os.ErrExist) {
		existing, err := os.ReadFile(backup)
		if err != nil {
			return err
		}
		if !bytes.Equal(existing, data) {
			return errors.New("legacy role backup differs; archive it before repeating migration")
		}
		return nil
	}
	if err != nil {
		return err
	}
	_, writeErr := file.Write(data)
	if err := errors.Join(writeErr, file.Sync(), file.Close()); err != nil {
		return errors.Join(err, os.Remove(backup))
	}
	d, err := os.Open(dir)
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}

func (s *Service) save() error {
	if s.commitUncertain {
		return errors.Join(ErrCommitUncertain, s.lastErr)
	}
	disk := s.state.clone()
	if disk.AdminPasswordOverride != nil && !utf8.ValidString(*disk.AdminPasswordOverride) {
		disk.AdminPasswordBytes = []byte(*disk.AdminPasswordOverride)
		disk.AdminPasswordOverride = nil
	}
	if disk.GuestPasswordOverride != nil && !utf8.ValidString(*disk.GuestPasswordOverride) {
		disk.GuestPasswordBytes = []byte(*disk.GuestPasswordOverride)
		disk.GuestPasswordOverride = nil
	}
	if disk.Retention == NativeRetention {
		disk.History = nil
		for key, m := range disk.Members {
			if m.Permissions == 0 || (s.room && m.Permissions&3 != 3) {
				delete(disk.Members, key)
				continue
			}
			m.LastTimestamp, m.Attempt = 0, 0
		}
		order := disk.MemberOrder[:0]
		for _, key := range disk.MemberOrder {
			if disk.Members[key] != nil {
				order = append(order, key)
			}
		}
		disk.MemberOrder = order
	}
	data, err := json.Marshal(disk)
	if err != nil {
		return err
	}
	if len(data) > maxStateBytes {
		return errors.New("role state exceeds 8 MiB persistence limit")
	}
	err = s.writeJSON(filepath.Join(s.cfg.StateDir, "state.json"), json.RawMessage(data))
	if errors.Is(err, hoststate.ErrCommitIndeterminate) {
		s.failClosed(errors.Join(ErrCommitUncertain, err))
		return s.lastErr
	}
	if err == nil {
		s.lastErr = nil
	}
	return err
}
