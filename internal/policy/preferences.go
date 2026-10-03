// Package policy contains native MeshCore role decisions without owning radios,
// identities, persistence, sessions or clocks.
package policy

import (
	"errors"
	"fmt"
	"math"
	"strings"

	meshcore "github.com/meshcore-go/meshcore-go"
)

const PreferencesVersion = 1

type Profile string

const (
	Repeater  Profile = "repeater"
	Room      Profile = "room"
	Companion Profile = "companion"
)

type PathHashMode uint8

func (m PathHashMode) Width() (uint8, error) {
	if m > 2 {
		return 0, errors.New("path hash mode must be 0, 1 or 2")
	}
	return uint8(m) + 1, nil
}

type LoopPolicy uint8

const (
	LoopOff LoopPolicy = iota
	LoopMinimal
	LoopModerate
	LoopStrict
)

type Permission uint8

const (
	Guest Permission = iota
	ReadOnly
	ReadWrite
	Admin
	RoleMask Permission = 3
)

func (p Permission) Role() Permission { return p & RoleMask }

type Scope struct {
	Name string   `json:"name"`
	Key  [16]byte `json:"key"`
}

func (s Scope) IsNull() bool { return s.Key == [16]byte{} }

const (
	RegionDenyFlood  uint8 = 1
	RegionDenyDirect uint8 = 2 // Reserved by native firmware; not an admission rule.
)

// Regions retain native insertion order: the first matching permitted entry wins.
// Keys apply only to private '$' regions; public names use auto-derived keys.
// Supplying private keys is a host extension: the pinned native keystore writer
// is unimplemented. It must not be reported as native-executed storage parity.
type Region struct {
	ID     uint16     `json:"id"`
	Parent uint16     `json:"parent"`
	Flags  uint8      `json:"flags"`
	Name   string     `json:"name"`
	Keys   [][16]byte `json:"keys,omitempty"`
}

type Preferences struct {
	Version            int          `json:"version"`
	PathHashMode       PathHashMode `json:"path_hash_mode"`
	RXDelay            float32      `json:"rxdelay"`
	TXDelay            float32      `json:"txdelay"`
	DirectTXDelay      float32      `json:"direct_txdelay"`
	AirtimeFactor      float32      `json:"airtime_factor"`
	Repeat             bool         `json:"repeat"`
	LocalAdvertSeconds uint32       `json:"local_advert_seconds"`
	FloodAdvertSeconds uint32       `json:"flood_advert_seconds"`
	FloodMaxHops       uint8        `json:"flood_max_hops"`
	UnscopedMaxHops    uint8        `json:"unscoped_max_hops"`
	AdvertMaxHops      uint8        `json:"advert_max_hops"`
	Loop               LoopPolicy   `json:"loop"`
	DefaultScope       Scope        `json:"default_scope"`
	WildcardFlags      uint8        `json:"wildcard_flags"`
	Regions            []Region     `json:"regions,omitempty"`
	OwnerInfo          string       `json:"owner_info"`
}

// Overrides are operator settings, applied over persisted preferences. Nil means
// absent; a pointer to zero deliberately disables or resets the corresponding value.
type Overrides struct {
	PathHashMode       *PathHashMode `json:"path_hash_mode,omitempty"`
	RXDelay            *float32      `json:"rxdelay,omitempty"`
	TXDelay            *float32      `json:"txdelay,omitempty"`
	DirectTXDelay      *float32      `json:"direct_txdelay,omitempty"`
	AirtimeFactor      *float32      `json:"airtime_factor,omitempty"`
	Repeat             *bool         `json:"repeat,omitempty"`
	LocalAdvertSeconds *uint32       `json:"local_advert_seconds,omitempty"`
	FloodAdvertSeconds *uint32       `json:"flood_advert_seconds,omitempty"`
	FloodMaxHops       *uint8        `json:"flood_max_hops,omitempty"`
	UnscopedMaxHops    *uint8        `json:"unscoped_max_hops,omitempty"`
	AdvertMaxHops      *uint8        `json:"advert_max_hops,omitempty"`
	Loop               *LoopPolicy   `json:"loop,omitempty"`
	DefaultScope       *Scope        `json:"default_scope,omitempty"`
	WildcardFlags      *uint8        `json:"wildcard_flags,omitempty"`
	Regions            *[]Region     `json:"regions,omitempty"`
	OwnerInfo          *string       `json:"owner_info,omitempty"`
}

// Defaults follow the pinned application constructors, not deployment choices.
// An unknown profile returns an invalid value which Validate rejects.
func Defaults(profile Profile) Preferences {
	if !validProfile(profile) {
		return Preferences{}
	}
	p := Preferences{
		Version: PreferencesVersion, TXDelay: .5, DirectTXDelay: .2,
		AirtimeFactor: 1, FloodMaxHops: 64, UnscopedMaxHops: 64, AdvertMaxHops: 8,
	}
	if profile != Companion {
		p.LocalAdvertSeconds, p.FloodAdvertSeconds = 120, 47*60*60
	}
	if profile == Repeater {
		p.Repeat, p.DirectTXDelay = true, .3
	}
	return p
}

func validProfile(p Profile) bool { return p == Repeater || p == Room || p == Companion }

func Validate(profile Profile, p Preferences) error {
	if !validProfile(profile) || p.Version != PreferencesVersion {
		return errors.New("unsupported policy profile or preferences version")
	}
	if _, err := p.PathHashMode.Width(); err != nil {
		return err
	}
	for _, f := range []struct {
		name   string
		v, max float32
	}{{"rxdelay", p.RXDelay, 20}, {"txdelay", p.TXDelay, 2},
		{"direct_txdelay", p.DirectTXDelay, 2}} {
		if math.IsNaN(float64(f.v)) || f.v < 0 || f.v > f.max {
			return fmt.Errorf("%s must be finite and in 0..%g", f.name, f.max)
		}
	}
	if math.IsNaN(float64(p.AirtimeFactor)) || math.IsInf(float64(p.AirtimeFactor), 0) || p.AirtimeFactor < 0 {
		return errors.New("airtime_factor must be finite and nonnegative")
	}
	if profile == Companion && (p.TXDelay != .5 || p.DirectTXDelay != .2) {
		return errors.New("native companion transmit factors are fixed at 0.5 and 0.2")
	}
	if p.FloodMaxHops > 64 || p.UnscopedMaxHops > 64 || p.AdvertMaxHops > 64 || p.Loop > LoopStrict {
		return errors.New("invalid flood hop limit or loop policy")
	}
	if p.LocalAdvertSeconds > 255*120 || p.FloodAdvertSeconds > 255*3600 {
		return errors.New("advert interval exceeds native storage range")
	}
	if len(p.OwnerInfo) > 119 || strings.ContainsRune(p.OwnerInfo, 0) {
		return errors.New("owner information must be at most 119 bytes without NUL")
	}
	if len(p.DefaultScope.Name) > 30 || strings.ContainsRune(p.DefaultScope.Name, 0) {
		return errors.New("default scope name must be at most 30 bytes without NUL")
	}
	if len(p.Regions) > 32 {
		return errors.New("region table exceeds native capacity 32")
	}
	ids := make(map[uint16]Region, len(p.Regions))
	names := make(map[string]bool, len(p.Regions))
	for _, r := range p.Regions {
		if r.ID == 0 || r.Name == "" || len(r.Name) > 30 || strings.ContainsRune(r.Name, 0) || len(r.Keys) > 4 {
			return errors.New("invalid region identity, name or key count")
		}
		for i := range len(r.Name) {
			c := r.Name[i]
			if !meshcore.IsValidRegionNameChar(c) {
				return errors.New("invalid native region name character")
			}
		}
		name := strings.TrimPrefix(r.Name, "#")
		if _, exists := ids[r.ID]; exists || names[name] {
			return errors.New("duplicate region identity or name")
		}
		ids[r.ID], names[name] = r, true
	}
	for _, r := range p.Regions {
		seen := map[uint16]bool{r.ID: true}
		for parent := r.Parent; parent != 0; {
			ancestor, ok := ids[parent]
			if !ok || seen[parent] {
				return errors.New("region parent is missing or cyclic")
			}
			seen[parent], parent = true, ancestor.Parent
		}
	}
	return nil
}

func ApplyOverrides(profile Profile, base Preferences, o Overrides) (Preferences, error) {
	p := base
	assign(&p.PathHashMode, o.PathHashMode)
	assign(&p.RXDelay, o.RXDelay)
	assign(&p.TXDelay, o.TXDelay)
	assign(&p.DirectTXDelay, o.DirectTXDelay)
	assign(&p.AirtimeFactor, o.AirtimeFactor)
	assign(&p.Repeat, o.Repeat)
	assign(&p.LocalAdvertSeconds, o.LocalAdvertSeconds)
	assign(&p.FloodAdvertSeconds, o.FloodAdvertSeconds)
	assign(&p.FloodMaxHops, o.FloodMaxHops)
	assign(&p.UnscopedMaxHops, o.UnscopedMaxHops)
	assign(&p.AdvertMaxHops, o.AdvertMaxHops)
	assign(&p.Loop, o.Loop)
	assign(&p.DefaultScope, o.DefaultScope)
	assign(&p.WildcardFlags, o.WildcardFlags)
	assign(&p.Regions, o.Regions)
	assign(&p.OwnerInfo, o.OwnerInfo)
	if err := Validate(profile, p); err != nil {
		return Preferences{}, err
	}
	return p.Clone(), nil
}

// Clone isolates the mutable region/key slices for a persistence transaction.
func (p Preferences) Clone() Preferences {
	p.Regions = append([]Region(nil), p.Regions...)
	for i := range p.Regions {
		p.Regions[i].Keys = append([][16]byte(nil), p.Regions[i].Keys...)
	}
	return p
}

func assign[T any](dest *T, value *T) {
	if value != nil {
		*dest = *value
	}
}
