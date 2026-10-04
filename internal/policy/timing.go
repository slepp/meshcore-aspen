package policy

import (
	"errors"
	"math"
)

// RXDelayMillis preserves the native signed arithmetic result. The dispatcher's
// separate hold gate is >=50ms, capped at 32000ms; node.WithRxDelay already applies
// it. Native pow(float,float) rounds the exponent and result to float32 before
// double-precision subtraction.
func RXDelayMillis(p Preferences, score float32, airtimeMS uint32) (int32, error) {
	if math.IsNaN(float64(p.RXDelay)) || p.RXDelay < 0 || p.RXDelay > 20 ||
		math.IsNaN(float64(score)) || math.IsInf(float64(score), 0) {
		return 0, errors.New("invalid receive delay input")
	}
	if p.RXDelay == 0 {
		return 0, nil
	}
	exponent := float32(.85) - score
	power := float32(math.Pow(float64(p.RXDelay), float64(exponent)))
	delay := (float64(power) - 1) * float64(airtimeMS)
	if math.IsNaN(delay) || delay >= math.MaxInt32+1 || delay <= math.MinInt32-1 {
		return 0, errors.New("receive delay exceeds signed millisecond range")
	}
	return int32(delay), nil
}

// RetransmitDelayMillis calls native-style nextInt with an inclusive minimum
// and exclusive maximum. Float32 airtime multiplication is truncated BEFORE
// constructing the RNG interval, not after sampling.
func RetransmitDelayMillis(profile Profile, p Preferences, direct bool, airtimeMS uint32, nextInt func(min, max uint32) uint32) (uint32, error) {
	if err := Validate(profile, p); err != nil {
		return 0, err
	}
	if nextInt == nil {
		return 0, errors.New("retransmit delay requires an RNG")
	}
	factor := p.TXDelay
	if direct {
		factor = p.DirectTXDelay
	}
	product := float32(airtimeMS) * factor
	if float64(product) > float64((uint64(math.MaxUint32)-1)/5) {
		return 0, errors.New("retransmit delay exceeds unsigned millisecond range")
	}
	t := uint32(product)
	max := 5*t + 1
	delay := nextInt(0, max)
	if delay >= max {
		return 0, errors.New("RNG result is outside the requested interval")
	}
	return delay, nil
}

const (
	payloadAdvert = 4
	payloadPath   = 8
	payloadTrace  = 9
)

// OriginatedPriority follows Mesh::sendFlood/sendDirect/sendZeroHop. Raw packet
// commands bypass this decision and retain the caller's explicit priority.
func OriginatedPriority(route Route, payloadType uint8, zeroHop bool) (uint8, error) {
	if route > TransportDirect || payloadType > 15 {
		return 0, errors.New("invalid route or payload type")
	}
	if zeroHop {
		if route.IsFlood() {
			return 0, errors.New("zero-hop origin must use a direct route")
		}
		return 0, nil
	}
	if route.IsFlood() {
		switch payloadType {
		case payloadTrace:
			return 0, errors.New("TRACE cannot be flooded")
		case payloadPath:
			return 2, nil
		case payloadAdvert:
			return 3, nil
		default:
			return 1, nil
		}
	}
	switch payloadType {
	case payloadTrace:
		return 5, nil
	case payloadPath:
		return 1, nil
	default:
		return 0, nil
	}
}

// RelayedPriority receives the flood path AFTER appending the forwarding node.
func RelayedPriority(route Route, payloadType uint8, pathAfterAppend Path) (uint8, error) {
	if route > TransportDirect || payloadType > 15 {
		return 0, errors.New("invalid route or payload type")
	}
	if route.IsFlood() {
		if payloadType == payloadTrace || !pathAfterAppend.Known() || pathAfterAppend.Count() == 0 {
			return 0, errors.New("invalid relayed flood path")
		}
		return pathAfterAppend.Count(), nil
	}
	if payloadType == payloadTrace {
		return 5, nil
	}
	return 0, nil
}
