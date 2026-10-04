package policy

import "time"

type AdvertKind uint8

const (
	AdvertNone AdvertKind = iota
	AdvertLocal
	AdvertFlood
)

// AdvertisementSchedule is caller-owned. Use a monotonic-bearing time.Time
// (normally time.Now) in production; timestamps must not be persisted.
type AdvertisementSchedule struct {
	NextLocal time.Time
	NextFlood time.Time
}

func NewAdvertisementSchedule(now time.Time, p Preferences) AdvertisementSchedule {
	return AdvertisementSchedule{
		NextLocal: advertDeadline(now, p.LocalAdvertSeconds),
		NextFlood: advertDeadline(now, p.FloodAdvertSeconds),
	}
}

func advertDeadline(now time.Time, seconds uint32) time.Time {
	if seconds == 0 {
		return time.Time{}
	}
	return now.Add(time.Duration(seconds) * time.Second)
}

// RescheduleLocal and RescheduleFlood correspond to the two native management
// callbacks: changing one interval must not restart the other timer.
func (s AdvertisementSchedule) RescheduleLocal(now time.Time, p Preferences) AdvertisementSchedule {
	s.NextLocal = advertDeadline(now, p.LocalAdvertSeconds)
	return s
}

func (s AdvertisementSchedule) RescheduleFlood(now time.Time, p Preferences) AdvertisementSchedule {
	s.NextFlood = advertDeadline(now, p.FloodAdvertSeconds)
	return s
}

func (s AdvertisementSchedule) Poll(now time.Time, p Preferences) (AdvertisementSchedule, AdvertKind) {
	if p.LocalAdvertSeconds == 0 {
		s.NextLocal = time.Time{}
	}
	if p.FloodAdvertSeconds == 0 {
		s.NextFlood = time.Time{}
	}
	// Native millisHasNowPassed is strictly greater, not greater-or-equal.
	if !s.NextFlood.IsZero() && now.After(s.NextFlood) {
		s.NextFlood = advertDeadline(now, p.FloodAdvertSeconds)
		s.NextLocal = advertDeadline(now, p.LocalAdvertSeconds)
		return s, AdvertFlood
	}
	if !s.NextLocal.IsZero() && now.After(s.NextLocal) {
		s.NextLocal = advertDeadline(now, p.LocalAdvertSeconds)
		return s, AdvertLocal
	}
	return s, AdvertNone
}
