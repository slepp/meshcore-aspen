// Package kissproxy gives each local client its own upstream KISS connection
// while retaining virtual identity and shared-PHY configuration protection.
// Legacy SET_RADIO and SET_TX_POWER succeed only when they match the shared
// profile: the configured one, or with Config.EffectivePHY (modem authority
// following the mast) the live verified profile, rejecting while unverified.
// They never retune the mast.
//
// Incomplete frames in either direction have five seconds to finish after
// their first body bytes arrive. Additional fragments do not extend that
// deadline. A stalled session closes independently of other clients; idle
// connections and inter-frame FEND bytes have no read timeout.
package kissproxy
