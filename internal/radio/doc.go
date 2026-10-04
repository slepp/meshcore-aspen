// Package radio connects host roles to a physical KISS modem.
//
// Cancelling a link closes its active connection even when a control request
// has stalled. Pending queued transmissions become unknown, never successful
// or replayed. A configured mast role sends its exact role key during every
// queued connection negotiation, before RX or TX is exposed to applications.
// SubmitWithReceipt submits at a caller-selected priority and delay and returns
// the mast's generation and job ID with a one-shot terminal result. Acceptance
// is not an RF transmission; final results include actual airtime and remain
// unknown after a lost connection without retrying the packet.
// Overlap is advisory: the 8-byte response includes independent flags for
// same role/key on mast/another host. A same-role flag does not imply a
// duplicated identity or impose a role interlock across KISS clients. The
// beta mast runs at most one on-chip instance per role, but raw KISS clients
// need no role announcement. The flags do not pair a matching role with a
// matching public key. The mask is the boot-applied selection; only the
// matching-role warning flag reports whether that native role runs.
// The saved next-boot profile is not present. Every connected role sends its
// announcement again after a TCP reconnect.
// Missing ROLE_PRESENCE support warns and continues under either PHY authority.
// Presence is warning-only: an error reply, nonzero reason, malformed or stale
// response, unrecognized mask/flag bits, or a failed fallback CAPACITY query
// is reported as state "unknown" (overlap unknown) with a warning and log, and
// the connection continues. If the exchange times out, leaving control-reply
// correlation uncertain, that stream is closed and one fresh connection is made
// without discovery; later reconnects announce again. HELLO and CONFIG checks
// remain strict. Neither presence nor CAPACITY replies control where roles
// run; older masts may report less detail.
// Explicit legacy non-queued mode remains available.
//
// Config.PHYTracking selects how a nonowner queued link treats the mast's
// CONFIG readback. PHYFixed (the zero value, and always the configuration
// owner's mode) admits only the configured profile; a mismatch at connection
// or later retires the connection and reconnects with backoff. PHYFollow
// adopts any valid effective profile the mast reports: a persistent change,
// a temporary tempradio switch and its automatic return. Invalid or malformed
// profiles are rejected in both modes. A follower never sends CONFIG SET.
//
// The link re-reads CONFIG on every connection, every heartbeat, and
// immediately after a submission is rejected STALE; rejected jobs are final and
// never replayed, and jobs lost with a connection resolve unknown. A CONFIG
// GET clears the mast's per-connection stale fence, so the host keeps its own:
// the readback is judged under the submission lock, and a changed, mismatched
// or failed readback closes that connection's admission gate until the new
// profile and its airtime model are adopted, or the connection is retired.
// Submissions meanwhile fail fast with ErrPHYSettling (an ErrOffline) before
// anything is sent.
// Links sharing a PHYGroup prompt each other to re-read when one observes a
// new (generation, profile) pair, and share the modem's airtime table per
// modulation; each fetch is bracketed by CONFIG readbacks so a table is never
// attributed to a modulation it was not measured under. AirtimeEstimator is
// live and follows the current effective modulation. EffectivePHY and
// PHYStatus report the verified profile, its configuration generation, and a
// bounded history of retune/reconnect transitions.
package radio
