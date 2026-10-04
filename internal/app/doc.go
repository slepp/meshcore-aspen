// Package app runs independent host roles over a shared physical radio.
// Name fields in the startup config provision new role state. Encrypted
// administrator/companion protocol mutations subsequently own saved names and
// channel keys; editing those initial fields does not override managed state.
// The shared PHY is changed only by its designated owner, never per role.
//
// Companion CMD19 schedules a logical restart of only the companion server and
// its source link. In multiplexed mode that virtual port remains quarantined
// until requested again; that request retires the owner's socket once and all
// sources rejoin a fresh epoch, fencing late uncorrelated replies. Closing an
// idle child keeps sibling roles online. CMD51 uses the same worker when the root
// configuration explicitly sets companion_factory_reset to true (default false).
// Reset waits for the old server's final save and source retirement, then
// atomically replaces its identity/state authority and reapplies operator
// configuration. Native requests acknowledge admission only by disconnecting;
// they never receive a fabricated reset-completion reply.
//
// The worker reuses the endpoint and its source link's live airtime estimator.
// It never restarts siblings or assumes PHY configuration ownership.
// Lifecycle admission requires require_parity because legacy radio connections
// configure the shared PHY rather than performing readback-only attachment.
// Key import/export remain independently opt-in and import does not restart.
//
// The companion /status entry exposes state and application_error alongside its
// current public_key, endpoint and radio_connected. A failed operation leaves
// that role stopped without retrying destructive work or stopping other roles.
//
// Physical TX callbacks maintain a coherent role_tx status snapshot: since,
// available, accepted/rejected/succeeded/failed/unknown event counts,
// reported_rf_airtime_ns, airtime_complete, diagnostic_drops and last_problem.
// Only terminal succeeded/failed RFAirtime contributes to the reported sum.
// QueueWait and EstimatedAirtime never contribute. An unknown completion leaves
// the known partial sum visible but permanently clears completeness for that
// role lifetime. Reconnect generations do not reset it; companion logical
// restart creates fresh accounting. Bot proxy sources have no TX callback and
// omit role_tx.
//
// Room/repeater telemetry receives TXAirtime only as valid when complete.
// RXAirtime remains unavailable. Native ErrorEvents is a firmware fault-bit
// field, not an outcome count. HasErrorEvents remains false until an exact
// native flag mapping exists; outcome counts stay in role_tx. No reserved
// fault bit is used. Physical Snapshot readings and errors retain
// their original validity: role-local accounting does not replace missing
// core modem measurements. Core readings require Link.Snapshot to succeed:
// the link is connected and its sample is no older than 45 seconds. Individual
// Has flags remain authoritative; a successful sample need not contain every
// optional reading. Snapshot errors are returned unchanged alongside independent
// role-local fields. Existing telemetry counters remain shared physical
// modem readings, labelled telemetry_counter_scope, not role-lifetime counts.
//
// Callbacks publish atomic snapshots and attempt non-blocking diagnostic
// admission to one bounded logger worker. Dropped diagnostics remain observable
// per role even though counters and the latest problem are never dropped.
// Each room/repeater link also binds its single service's LogTXResult callback
// for optional packet logging. Logical role restart replaces both service and
// source; TCP reconnect and sibling restarts do not register handlers again.
//
// Room/repeater RequestRestart callbacks enqueue work to independent workers.
// Authenticated native reboot admission follows the role's durable command
// commit. The callback's five-second context bounds admission only. Accepted
// work retires asynchronously with no CLI success reply or added restart delay.
// Repeater legacy plain-text CLI can enqueue its native 200 ms transport ACK
// before admission; this does not confirm restart or wait for RF completion.
// Rejection replies and their native delays remain owned by roles.
// Each worker waits for its service's Close (including drained persistence),
// stops its adapter and closes the physical source before loading the same
// identity/state and reapplying operator configuration. A failed operation
// leaves only that role stopped with application_error. Their /status entries
// select the current role lifetime, never a retired link. Each role source's
// airtime estimator and RX score follow that link's verified effective PHY,
// across restarts and mast retunes, and join readback-only without changing
// the physical configuration owner's link. Legacy
// non-parity admission is rejected. No role reset, rekey or shared reboot is
// implemented by these workers.
//
// Every /status role entry includes application_started_at: a UTC RFC3339Nano
// JSON string for its active application lifetime, or null when no application
// is active. Room/repeater/companion publish a fresh timestamp only after
// successful activation, not on restart admission or TCP source reconnect.
// Retiring/stopped instances expose null while retaining explicit state,
// radio_connected and application_error fields. Observer and bot timestamps
// remain fixed for their host-managed application lifetimes.
// The observer or controller shared_phy shows the requested physical profile
// and an effective value only while its queued connection is online. The
// queued CONFIG response is read at connection, every heartbeat and after a
// STALE submission rejection, not on each HTTP request. phy_authority defaults
// to host, where the observer or controller applies the profile. With modem
// authority the host link only joins with HELLO nonowner and reads CONFIG GET;
// configuration_owner is "modem". phy_tracking (modem authority only) defaults
// to follow: every host link adopts the mast's valid effective profile through
// authenticated persistent changes, bounded tempradio switches and their
// automatic return, and reconnects, without restarting roles or changing their
// identities. radio/tx_power then act as the requested reference, and
// shared_phy reports matches_requested, configuration_generation,
// effective_since, the transition count and recent_transitions; each role
// entry reports its source's phy_configuration_generation. Companion SELF_INFO,
// its radio setting checks and RX scoring read the source's live effective PHY
// (the value verified at instance start while offline). The bot KISS proxy
// admits legacy SET_RADIO/SET_TX_POWER only when they match the configuration
// link's live readback, rejecting them while it is unverified. phy_tracking "fixed" keeps deliberate mismatch detection: a differing
// readback fails startup or takes that link offline until the mast returns to
// the configured profile. Host authority always behaves as fixed. owner_connected describes the verifying link, not the MCU's owner
// slot. radio_client_capacity defaults to eight for host authority. Modem
// authority caps external connections at four. Startup queries CAPACITY on
// its negotiated configuration link before opening role sources; a supported
// response validates the configured/reserved limit. Older firmware permits an
// eight-slot fallback with host authority, or a warning and four-slot fallback
// (or configured limit) with modem authority. Legacy non-queued connections
// cannot negotiate CAPACITY; they use the configured limit without a query.
// The connection budget counts the persistent controller link, enabled role
// sources, optional bot companion source, and reserved bot KISS sessions.
// radio_session defaults to "per_role", retaining independent TCP connections.
// "auto" first tries an extended queued CAPACITY request on the owner's
// port-0 connection; unsupported older firmware causes that socket to be
// discarded and the normal per-role connection plan to start afresh. "required"
// fails startup if subports are unavailable. On success, port 0 carries the
// observer/controller, and up to three enabled roles (repeater, room,
// companion, then bot companion) take available ports 1..3 with independent HELLO,
// CONFIG readback, role identity and TX ownership. Extra host sources and
// every bot KISS proxy client retain direct TCP connections, counted against
// the mast's reported external slot capacity. Port 0 counts toward the modem's
// advertised logical-port count; a three-port modem provides only ports 1 and 2
// for roles, and each remaining role needs its own direct TCP slot. Required
// subports are computed from selected roles and the configured socket budget;
// negotiated capacity is checked again before opening sources. The role plan
// reserves its assigned virtual ports across reconnects; a smaller temporary
// port report retries without moving a role to an unplanned socket. A per-role
// fallback that cannot accommodate all reserved sources fails startup.
// The session needs a firmware CAPACITY response with five payload bytes:
// version, TCP slots, local slots, ports including port 0, and nonzero
// sessions. Ordinary HELLO and the legacy three-byte CAPACITY response remain
// unchanged.
// Closing port 0 retires the physical socket; closing a child drops only that
// port's RX/TX and quarantines it until the next owner epoch. Existing sibling
// links continue without renegotiation until the quarantined port requests a
// reconnect. That request retires the shared socket once; all links then join
// a fresh owner epoch with their own HELLO and readback. This briefly interrupts
// siblings on a child restart or heartbeat failure, not on an idle child Close.
// Each child may request one owner epoch refresh per failure streak. Only
// completing HELLO, CONFIG, PHY settle and telemetry on that child rearms it;
// repeated failures before it is fully online leave it offline with an
// explicit error instead of periodically flapping every sibling. A natural
// owner reconnect allows the child to try joining again; a failed logical
// restart remains stopped until the host is restarted.
// A refresh does not wait for accepted RF jobs to complete: they become
// unknown and are not replayed.
// Outstanding TX jobs become unknown and are never replayed.
// At initial startup auto can fall back to per_role after an unsupported or
// silent three-second probe, or a report of fewer ports than the selected roles
// and direct-client socket budget require (subject to the usual direct-client
// capacity check). On reconnect, only
// explicit legacy responses cause a terminal downgrade; probe timeouts,
// malformed responses and temporarily insufficient ports retry on fresh sockets.
// If a reconnect finds explicit old-firmware evidence, the host exits with an
// explicit error rather than remaining ready or retrying indefinitely; an
// external supervisor must restart it so auto can select per_role on a fresh
// connection. Required mode fails startup instead of falling back.
// KISS clients are logical sources sharing one PHY and its airtime budget, not
// exclusive radio owners. Beta runs at most one on-chip instance of each native
// role; the host config selects at most one base room, repeater, companion and
// observer worker each (plus a separately configured bot companion). Operators
// select placement through the mast profile and enabled_roles, including
// intentional overlap. Generic KISS applications, including direct mast
// clients, need no role announcement.
// Repeater, room, companion and observer sources present the public key of
// their loaded state identity to the mast with advisory queued ROLE_PRESENCE on
// every TCP connection, before RX/advertising/forwarding. Overlapping roles
// and identities produce warnings, not placement restrictions. The 0x26
// ROLE_PRESENCE response is 8 bytes: version, reason, source generation,
// boot native mask and warning flags. Flags identify an active matching role
// on mast/another host or the same key on mast/another host, respectively.
// These are independent aggregate observations: a same-role flag alone does
// not assert a duplicate identity or identify which instance shares a key.
// A native room and a host room with distinct identities can both use the PHY;
// their presence warnings never deny RX/TX. No dynamic multi-room provisioning
// is provided in this beta.
// A selected-but-inactive native role in the boot mask alone is not an overlap.
// Valid overlap returns reason NONE and does not affect the connection. An
// error, nonzero reason, malformed response, stale generation or unrecognized
// bit reports role_presence state "unknown" with a warning and never denies
// admission; a timed-out exchange retires that stream and reconnects once
// without discovery. TCP reconnects announce again. Source opening receives the exact identity loaded
// for each worker lifetime, including after logical rekey/reset; TCP reconnects
// retry with that same identity.
// The observer passively publishes received traffic to MQTT; its presence on
// the configuration link is informational, not mesh role ownership. A
// controller-only link and bot companion/bot KISS proxy do not impersonate a
// base companion. Bot proxy clients cannot forge ROLE_PRESENCE, but can forward
// data without announcing a role. If the extension is absent, both host and
// modem authority log and continue, even when CAPACITY reports local sources
// or is itself unavailable. Missing CAPACITY leaves the actual external
// connection limit unverified, so operators should set radio_client_capacity
// to the physical modem's limit. Presence reports are advisory snapshots,
// including on older combined masts. A connected role's /status entry repeats
// the overlap as an optional top-level warning without changing state or
// readiness. role_presence includes the source generation and warning flags,
// applied_native_roles decoded from the mast's boot-loaded mask, and
// native_role_running for this role from the active-role flag. A selected but
// stopped mast role is applied, not running. The saved next-boot role profile
// is not in this KISS response; inspect the mast's signed RF status instead.
// A disconnected source keeps its last role_presence snapshot marked offline
// but has no current top-level warning. Presence is an unauthenticated report,
// not identity evidence or an RF delivery guarantee. Another host may connect
// after a snapshot, and unannounced hosts cannot be identified.
// Companion companion_retention similarly separates the requested mode from
// the effective active application mode. Both entries expose explicit
// validity and error, rather than stale values after shutdown.
// The optional bot_companion uses the same worker with an independent identity,
// state directory, readback-only radio source and lifecycle. Its key management
// import is separately opt-in and uses the same atomic identity/document
// transaction as base; export and factory reset stay disabled. The existing
// KISS proxy remains separate.
//
// enabled_roles selects repeater, room, companion, observer and bot independently.
// An omitted selection retains all five; an explicit empty list runs only the
// configuration controller and status endpoint. Disabled roles have state
// "disabled" and no identity or source. When observer is disabled, controller
// owns the configuration link, airtime model and shared_phy status without
// starting MQTT or allocating an observer identity. bot_companion_listen alone
// enables the separate bot companion, including when companion is disabled.
//
// GET /readyz returns HTTP 200 only when the configuration owner and enabled
// applications are running with online sources and cached physical telemetry,
// the enabled observer publisher is connected to MQTT, and the enabled bot
// listener is active. Otherwise it returns 503.
// When bot_companion is configured, it must independently satisfy the same
// application, radio and telemetry readiness checks as the base companion.
// An admitted restart/reset is immediately unready, before retirement starts.
// Host cancellation is unready throughout cleanup. No RF or broker probe runs
// in the HTTP handler. Both health endpoints send Cache-Control: no-store.
//
// The readiness JSON is {"ready":bool,"not_ready":{role:reason},
// "mqtt_connection_checked":bool}. The reasons are not_started,
// application_fault, application_not_running, radio_disconnected,
// telemetry_unavailable, mqtt_disconnected, mqtt_connection_unavailable and
// listener_inactive; host shutdown adds
// "host":"shutting_down". A healthy response has an empty not_ready object.
// When enabled, production checks observer.Connected using Paho's open
// connection state after the broker acknowledges the online publication.
// A disconnected publisher is unready. Broker disconnects recover without a
// permanent application fault or a changed lifetime timestamp.
// Observer status includes mqtt_connected (boolean) and observer_stats with
// numeric observed, published, dropped and queued fields. These are the existing
// best-effort counters, not transactional snapshots or subscriber-delivery
// guarantees.
//
// roles.Service.ApplicationError and companion.Server.ApplicationError expose
// the sticky terminal fail-closed cause through lock-free reads. Status reports
// these instances as state:"faulted" with application_error, while retaining
// their loaded lifetime timestamp and independent radio_connected value.
// Recoverable packet/command errors do not become terminal faults. Temporary
// source/telemetry failures make readiness fail until recovery without rekeying
// or restarting the role. A terminal fault never initiates hidden recovery.
// Companion and bot status include listener_active while loaded; it becomes
// false on Close or Accept failure, without waiting for session cleanup.
// A stopped companion retains its last public key and explicit stopped state.
//
// Root role_key_import (default false) enables authenticated room/repeater
// Config.StageIdentity callbacks independently of companion_key_import.
// Native "set prv.key" stages a strict 64-byte scalar-prefix key in the existing
// private envelope; its reboot-to-apply reply does not change the running key.
// Normal saves retain pending identity. Logical restart activates only after
// successful old-service/source retirement; full-host startup activates before
// selecting or logging identities. Previously staged intent applies even when
// new staging is disabled. Activation uses roles.RebindStateIdentity and one
// atomic identity/document replacement, then clears pending. No pending key is
// a no-write no-op. Commit uncertainty leaves the affected startup stopped.
//
// State coordinates current/pending uniqueness across repeater, room, companion,
// observer, bot and the optional bot_companion from canonical storage, including
// both companion active imports, while holding the role transaction lock.
// No startup identity map is used for collision decisions. This local state
// does not detect duplicate role identities on separate hosts without modem
// overlap reporting. No remote role key export, erase, reset or
// shared-PHY operation is added.
package app
