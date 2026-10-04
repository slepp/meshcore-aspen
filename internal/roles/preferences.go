package roles

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"

	"meshcore.local/meshcore/internal/policy"
)

const sharedPHYOwnerRequired = "Error: mast-admin required for shared radio"

func (s *Service) preferenceCommand(command string) string {
	parts := strings.SplitN(command, " ", 3)
	if len(parts) < 2 {
		if command == "get" || command == "set" {
			return "Error: usage: " + s.commandHelp(command)
		}
		return "Err - unsupported by host role"
	}
	p := s.state.Preferences
	if parts[0] == "get" {
		if len(parts) != 2 {
			return "Error: usage: get SETTING; use help get"
		}
		value := ""
		switch parts[1] {
		case "guest.password":
			value = s.cfg.Password
			if s.state.GuestPasswordOverride != nil {
				value = *s.state.GuestPasswordOverride
			}
		case "multi.acks":
			value = fmt.Sprint(s.state.MultiACKs)
		case "allow.read.only":
			value = "off"
			if s.state.AllowReadOnly {
				value = "on"
			}
		case "lat":
			value = nativeFloat(float32(s.state.Latitude))
		case "lon":
			value = nativeFloat(float32(s.state.Longitude))
		case "public.key":
			value = strings.ToUpper(s.id.Identity.String())
		case "role":
			value = "repeater"
			if s.room {
				value = "room_server"
			}
		case "radio", "freq":
			if s.cfg.CurrentRadio == nil {
				return "Error: shared radio readback unavailable"
			}
			radio, ok := s.cfg.CurrentRadio()
			if !ok {
				return "Error: shared radio readback unavailable"
			}
			bw := strings.TrimRight(strings.TrimRight(strconv.FormatFloat(float64(radio.BwHz)/1000, 'f', 3, 64), "0"), ".")
			frequency := nativeFloat(float32(float64(radio.FreqHz) / 1e6))
			value = frequency
			if parts[1] == "radio" {
				value = fmt.Sprintf("%s,%s,%d,%d", frequency, bw, radio.SF, radio.CR)
			}
		case "tx":
			if s.cfg.CurrentPower == nil {
				return "Error: shared radio readback unavailable"
			}
			power, ok := s.cfg.CurrentPower()
			if !ok {
				return "Error: shared radio readback unavailable"
			}
			value = fmt.Sprint(power)
		case "rxdelay":
			value = nativeFloat(p.RXDelay)
		case "txdelay":
			value = nativeFloat(p.TXDelay)
		case "direct.txdelay":
			value = nativeFloat(p.DirectTXDelay)
		case "af":
			value = nativeFloat(p.AirtimeFactor)
		case "dutycycle":
			value = nativeDuty(p.AirtimeFactor)
		case "repeat":
			value = "off"
			if p.Repeat {
				value = "on"
			}
		case "path.hash.mode":
			value = fmt.Sprint(p.PathHashMode)
		case "advert.interval":
			value = fmt.Sprint(p.LocalAdvertSeconds / 60)
		case "flood.advert.interval":
			value = fmt.Sprint(p.FloodAdvertSeconds / 3600)
		case "flood.max":
			value = fmt.Sprint(p.FloodMaxHops)
		case "flood.max.unscoped":
			value = fmt.Sprint(p.UnscopedMaxHops)
		case "flood.max.advert":
			value = fmt.Sprint(p.AdvertMaxHops)
		case "loop.detect":
			value = []string{"off", "minimal", "moderate", "strict"}[p.Loop]
		case "owner.info":
			value = strings.ReplaceAll(p.OwnerInfo, "\n", "|")
		case "bridge.type":
			value = "none"
		default:
			if strings.HasPrefix(parts[1], "wifi.") {
				return s.commandHelp("wifi")
			}
			return "Err - unsupported by host role"
		}
		return "> " + value
	}
	if parts[0] == "set" && (parts[1] == "radio" || parts[1] == "tx" || parts[1] == "freq") {
		return sharedPHYOwnerRequired
	}
	if parts[0] == "set" && strings.HasPrefix(parts[1], "wifi.") {
		return s.commandHelp("wifi")
	}
	if parts[0] == "set" && len(parts) != 3 {
		return "Error: usage: set SETTING VALUE; use help set"
	}
	if parts[0] != "set" {
		return "Err - unsupported by host role"
	}
	value := parts[2]
	switch parts[1] {
	case "guest.password":
		if len(value) > 15 {
			value = value[:15]
		}
		s.state.GuestPasswordOverride = &value
	case "name":
		if len(value) == 0 || len(value) > 31 || !utf8.ValidString(value) || strings.ContainsAny(value, "\x00[]\\:,?*") {
			return "Error, bad chars or name length"
		}
		s.state.Name = value
	case "multi.acks":
		n, err := strconv.ParseUint(value, 10, 8)
		if err != nil || n > 1 {
			return "Error, multi.acks must be 0 or 1"
		}
		s.state.MultiACKs = uint8(n)
	case "allow.read.only":
		if value != "on" && value != "off" {
			return "Error, must be on or off"
		}
		s.state.AllowReadOnly = value == "on"
	case "lat", "lon":
		n, err := strconv.ParseFloat(value, 64)
		limit := float64(180)
		if parts[1] == "lat" {
			limit = 90
		}
		if err != nil || math.IsNaN(n) || math.IsInf(n, 0) || n < -limit || n > limit {
			return "Error, invalid coordinate"
		}
		if parts[1] == "lat" {
			s.state.Latitude = n
		} else {
			s.state.Longitude = n
		}
	case "rxdelay", "txdelay", "direct.txdelay":
		n, err := strconv.ParseFloat(value, 32)
		if err != nil {
			return "Error, invalid numeric value"
		}
		switch parts[1] {
		case "rxdelay":
			if n != 0 && s.cfg.RXScore == nil {
				return "Error, native radio score unavailable"
			}
			p.RXDelay = float32(n)
		case "txdelay":
			p.TXDelay = float32(n)
		case "direct.txdelay":
			p.DirectTXDelay = float32(n)
		}
	case "af", "dutycycle":
		if s.sourcePolicy == nil {
			return "Error, physical source-policy management is not connected"
		}
		n, err := strconv.ParseFloat(value, 32)
		if err != nil || math.IsNaN(n) || math.IsInf(n, 0) || n < 0 {
			return "Error, invalid airtime value"
		}
		if parts[1] == "dutycycle" {
			if n < 1 || n > 100 {
				return "ERROR: dutycycle must be 1-100"
			}
			n = float64(float32(100)/float32(n) - 1)
		}
		p.AirtimeFactor = float32(n)
	case "repeat":
		if value != "on" && value != "off" {
			return "Error, must be on or off"
		}
		p.Repeat = value == "on"
	case "owner.info":
		p.OwnerInfo = strings.ReplaceAll(value, "|", "\n")
	case "loop.detect":
		found := false
		for i, v := range []string{"off", "minimal", "moderate", "strict"} {
			if value == v {
				p.Loop, found = policy.LoopPolicy(i), true
			}
		}
		if !found {
			return "Error, must be: off, minimal, moderate, or strict"
		}
	case "path.hash.mode", "flood.max", "flood.max.unscoped", "flood.max.advert", "advert.interval", "flood.advert.interval":
		n, err := strconv.ParseUint(value, 10, 8)
		if err != nil {
			return "Error, invalid integer"
		}
		switch parts[1] {
		case "path.hash.mode":
			p.PathHashMode = policy.PathHashMode(n)
		case "flood.max":
			p.FloodMaxHops = uint8(n)
		case "flood.max.unscoped":
			p.UnscopedMaxHops = uint8(n)
		case "flood.max.advert":
			p.AdvertMaxHops = uint8(n)
		case "advert.interval":
			if n != 0 && (n < 60 || n > 240) {
				return "Error: interval range is 60-240 minutes"
			}
			p.LocalAdvertSeconds = uint32(n/2) * 120
		case "flood.advert.interval":
			if n != 0 && (n < 3 || n > 168) {
				return "Error: interval range is 3-168 hours"
			}
			p.FloodAdvertSeconds = uint32(n) * 3600
		}
	default:
		return "Err - unsupported by host role"
	}
	disableInitialAdverts(&p)
	validated, err := policy.ApplyOverrides(s.profile, p, policy.Overrides{})
	if err != nil {
		return "Error, " + err.Error()
	}
	if err := s.validatePolicyInputs(validated); err != nil {
		return "Error, " + err.Error()
	}
	s.state.Preferences = validated
	if parts[1] == "repeat" {
		return "OK - repeat is now " + strings.ToUpper(value)
	}
	if parts[1] == "dutycycle" {
		return "OK - " + nativeDuty(validated.AirtimeFactor)
	}
	return "OK"
}

// CommonCLI::savePrefs turns off the initial fast interval on manual setup.
func disableInitialAdverts(p *policy.Preferences) {
	if p.LocalAdvertSeconds < 3600 {
		p.LocalAdvertSeconds = 0
	}
}

func nativeDuty(factor float32) string {
	duty := float32(100) / (factor + 1)
	whole := int(duty)
	fraction := int((duty-float32(whole))*10 + .5)
	return fmt.Sprintf("%d.%d%%", whole, fraction)
}

// StrHelper::ftoa uses a 24-bit fixed fraction and seven truncated digits,
// not printf rounding. Preserve its textual management representation.
func nativeFloat(value float32) string {
	if value == 0 {
		return "0.0"
	}
	bits := math.Float32bits(value)
	exponent := int(bits>>23&255) - 127
	if exponent >= 31 || exponent < -23 {
		return "0"
	}
	mantissa := (bits & 0xffffff) | 0x800000
	var whole, fraction uint32
	switch {
	case exponent >= 23:
		whole = mantissa << uint(exponent-23)
	case exponent >= 0:
		whole = mantissa >> uint(23-exponent)
		fraction = (mantissa << uint(exponent+1)) & 0xffffff
	default:
		fraction = mantissa >> uint(-(exponent + 1))
	}
	sign := ""
	if bits>>31 != 0 {
		sign = "-"
	}
	text := sign + strconv.FormatUint(uint64(whole), 10) + "."
	if fraction == 0 {
		return text + "0"
	}
	var digits [7]byte
	for i := range digits {
		fraction *= 10
		digits[i] = byte(fraction>>24) + '0'
		fraction &= 0xffffff
	}
	tail := strings.TrimRight(string(digits[:]), "0")
	if tail == "" {
		tail = "0"
	}
	return text + tail
}
