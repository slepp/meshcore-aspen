/*
Package companion provides a shared-identity MeshCore companion endpoint, not a
KISS endpoint. The caller supplies a radio owned by this role (normally a RadioMux
port), the stable identity and a TCP listener. Close stops the node and closes that
radio port and listener. Serve cancellation also closes the server. Close waits
for owned workers, flushes state and returns any persistence error. TCP provides
no authentication or encryption; expose the listener only to trusted clients.

The supported command numbers and operations are:

  - 1, 22: app/device handshake, including repeat/path mode, telemetry permission
    and multi-ACK settings. Advertised protocol is 13.
  - 5, 6: read/set a persistent logical clock offset; never change the host clock.
  - 7, 8, 14: self advertisement, shared name and explicitly configured location.
  - 4, 9, 13, 15–18, 30, 42: contact listing/incremental sync, add/update, reset
    path, removal, zero-hop sharing, signed advert export/import, lookup and the
    last received advert path. A manually added contact cannot be exported until
    its signed advert has been received or imported. Manual add/update returns
    OK without an advert push; signed imports and received adverts still notify
    connected clients. Received adverts require a nonempty parsed name, including
    when the name flag is set. A missing location flag preserves saved coordinates;
    a present flag with 0,0 replaces them. Nameless imports return IllegalArg
    without changing contacts (native import queues them, then ignores them).
  - 2, 3, 10: private plain/CLI text, public/private channel text, message sync.
    Sender identity, encryption, radio routing and the transmit queue use the Go
    MeshCore runtime. Private attempts come from the client; group sends are
    single transmissions. Plain and room signed messages are acknowledged.
  - 11, 12: acknowledge only the shared physical tuning/TX power; conflicting
    changes return the native two-byte UnsupportedCmd error (the wire has no
    text error payload). Use the separately addressed authenticated mast-admin
    Management role to change a modem-owned PHY, or the private host config
    and restart when the host is physical owner. Companion credentials do not
    confer physical-owner authority. With Config.EffectivePHY that reference,
    like SELF_INFO and RX scoring, is the live verified PHY, falling back to
    RadioConfig/TxPower while unverified. The optional repeat byte changes real forwarding policy.
    Command 60 reports the native default permitted repeat frequencies.
    Advert names and channel keys persist before a successful mutation reply;
    invalid UTF-8 or NUL-containing names fail rather than changing across a
    later JSON save/reload. The supplied Config.Name provisions only new state.
  - 21, 43: persistent RX delay and source airtime tuning. Changed airtime factors
    require the supplied TxRadio's SetSourceAirtimeFactor method. The PHY owns the
    budget; no host airtime budget is added. An uncertain source-policy apply
    after the durable commit fails closed and requires role recovery.
    Startup reapplies the selected factor, including default 1, whenever the
    radio supports source policy, so reused adapters cannot retain stale tuning.
    Startup fails if that application cannot be confirmed.
    Runtime factors such as 99 (native dutycycle 1 percent), fractional factors
    and explicit zero are preserved as float32 values and reapplied on restart.
    Keeping these values across restart is a durable-host preference extension:
    it deliberately does not reproduce the native boot/load clamp to 0..9.
    Negative and nonfinite factors are rejected, not clamped. The companion
    command wire remains native uint32 thousandths; PHY encoding is owned by the
    radio adapter and must preserve the supplied float32 value.
    GET_TUNING_PARAMS returns BadState if a valid host preference exceeds that
    command's uint32-thousandths representation; it never reports a wrapped zero
    factor or changes the stored/runtime value to make it fit.
  - 26–29, 39, 50, 52, 57: room/repeater login, remote status, connection state,
    local logout, local/remote telemetry, binary requests, path discovery and
    anonymous requests, including eight nonpersistent unknown-contact slots.
    Login supports the firmware's 15-byte password
    limit. Room connections maintain keepalives and a persistent sync timestamp.
    Replies prefer request-tag matching. Login replies with server-time tags
    and older status replies without matching tags fall back to contact
    matching only when exactly one request to that peer is outstanding and
    its kind matches the fallback. Ambiguous replies are ignored rather than
    attributed to the wrong client.
    A newly admitted login supersedes only the pending login to that same full
    peer key, even when another client submitted it; other pending operations
    remain intact. Failed TX admission preserves the prior request and deadline.
    Superseded request tags are fenced, with at most 128 live fences, until the
    replacement deadline; exhausting that capacity rejects rather than sends
    an unfenced replacement. Native server-time login replies cannot identify
    an attempt and match only the current unambiguous peer request. A server
    timestamp equal to a superseded request tag is not evidence of a stale reply.
  - 31, 32: read/write 40 shared channel slots; an empty name disables a slot.
  - 33–35: identity signing with independent per-client, 8192-byte buffers.
  - 38, 58, 59: manual/type/hop-filtered contact addition, non-favourite eviction,
    location sharing, packed telemetry permissions and multi-ACK settings.
  - 20, 56: battery and core/radio/packet statistics when the corresponding
    Config.Battery or Config.Stats callback supplies real measurements. Battery
    replies include storage fields when Config.Storage supplies them: response
    code 12, battery uint16, used KB uint32, total KB uint32, all little-endian
    (11 bytes). The host state.Storage adapter reports shared filesystem usage
    and capacity, not a per-role quota or free-space count. Unrepresentable
    capacity must return an error, not truncated values. Storage remains optional;
    without a battery provider this command is unsupported, even with storage.
    Stats providers can leave a subtype pointer nil to report it unsupported.
    Missing measurement callbacks fail explicitly.
  - 25, 36, 55, 62, 65: direct raw data, requester-owned traces, zero-hop control,
    encrypted channel datagrams and serialized raw packets with explicit priority.
    Raw/control/trace/RX-log pushes are live events, not replay-journal messages.
  - 54, 61, 63, 64: session-local scope overrides, originated ordinary path width
    1/2/3, and persistent default scope. Learned direct paths retain their width.
    Trace widths use their separate native 1/2/4/8-byte encoding: TRACE flags'
    low two bits 0/1/2/3 select those widths, not ordinary path mode 0/1/2.
    A three-byte originated or learned route cannot be requested as a native
    three-byte TRACE; clients must reject that request rather than silently
    downgrading to two bytes. The host preserves raw native TRACE flags.
  - 40, 41: the no-custom-sensor profile returns an empty settings list and rejects
    unknown settings. Command 37 returns Disabled: this endpoint has no BLE.
  - 23: native 64-byte scalar-prefix identity export through Config.ExportIdentity.
    A nil callback returns Disabled. Invalid keys and keys not matching the active
    companion identity fail explicitly.
  - 24: native scalar-prefix identity import through Config.ImportIdentity.
    Nil disables import; enabling it requires persistent role state. The callback
    commits the key and complete replacement JSON atomically, then returns the
    committed identity. It must not re-enter or close the server. Companion
    activates that identity and refreshes cached secrets before replying OK.
    Contacts, channels, configuration and queued messages survive; pending
    requests, ACK tracking, room connections, transient contacts and signing
    sessions are invalidated. Already-enqueued radio packets are not cancelled.
    A failed commit leaves the current identity intact; an indeterminate commit
    fails closed until recovery.
  - 19, 51: reboot/reset admission through Config.RequestRestart and
    Config.RequestFactoryReset. The native "reboot"/"reset" markers are required.
    Nil callbacks return UnsupportedCmd; rejected admission reports BadState
    and the provider error. Accepted requests send no completion ACK.
    Callbacks only enqueue work to an independent parent worker and must not
    call Close or re-enter the server. Their three-second contexts bound
    admission only; the worker must use its own lifetime context.

The parent worker runs logical restart/reset, closes and flushes the old role
before reset, and reports failures in application status. Admission only schedules
the operation; neither command reboots the shared physical radio.
No battery, GPS fix or hardware status is fabricated. Config.SensorTelemetry
supplies optional permission-filtered Cayenne LPP sensor bytes. Location settings
are user configuration, not a measured GPS fix. Native defaults originate one-byte
paths, disable forwarding and periodic adverts, and use RX delay zero. Explicit
Config.Policy overrides replace persisted policy fields; omitted overrides preserve
them, including explicit zero. A positive legacy AdvertInterval overrides the
flood interval unless Policy.FloodAdvertSeconds is present. Configured periodic
adverts also produce one initial advert as a host startup extension.
Local modem reflections (signal metadata SNR -32, RSSI 127) remain eligible for
application delivery even when a direct packet still contains its outbound path.
Ordinary in-transit RF packets are not accepted through that exception.

Contacts (350), channels, logical clock, name/location and the latest 256 incoming
messages use the companion.json role document. Before identity import it is an
ordinary JSON file; afterward parent ReadRoleJSON/WriteRoleJSON keep it inside
the single authoritative identity-state.json envelope with the native key.
The retained legacy files are not a fallback for a corrupt envelope. Writes
use atomic, fsynced replacement.
Directory-sync failure after rename is indeterminate and fails closed, rather
than falsely restoring an older in-memory snapshot. ACKs follow message commit;
signed room messages and their resume cursor share that commit. Direct text ACKs
request 200 ms delay; multi-ACK mode adds one copy then the ACK at 500 ms. Native
originated priorities and request-response delays reach the TxRadio unchanged.
StateDir empty explicitly selects nonpersistent operation. Contacts are added
automatically unless manual-add is enabled. Signed discovery, path changes,
contact deletion, ACKs and message-waiting events are broadcast to all connected
clients. Successful login is a shared connection-state event broadcast to all
clients. Failed login and other remote request responses go only to the requester.
Channel/name/configuration changes have no protocol push type; clients must
refresh them. Known-contact discovery events do not claim that an advert's
reverse path is a learned outbound path.

Each of at most 32 connected clients has an independent message cursor, protocol
version, signing buffer, scope override and bounded writer queue. Reading messages
never consumes another client's unread messages. NativeQueue removes messages
once all currently connected clients have read them; DurableReplay retains read
history. Both profiles persist pending data as a host extension. Existing saved
history migrates to DurableReplay; new state defaults to NativeQueue. At capacity,
the oldest channel text/datagram is evicted first. If no channel record remains,
DurableReplay reclaims the oldest record already fetched by every connected
client, allowing new direct messages and CLI replies after the replay inbox has
been read. With no connected reader, or when all remaining direct messages are
unread by at least one client, the queue rejects new arrivals and withholds their
ACK instead of silently discarding unread direct messages. This rejection is an
explicit safety deviation from native's ACK-even-if-the-offline-queue-is-full behaviour.
There is no durable client identifier or application acknowledgement: reconnects
replay whatever remains, not a lossless or exactly-once per-client history.
A live client falling behind
that retention window, overflowing its writer queue, or failing a write deadline
is disconnected, rather than silently losing its stream. A full radio-event queue
reports an error and disconnects clients. Radio callbacks never wait for TCP or
disk writes. ErrorHandler must be nonblocking and must not call Server methods.
Telemetry providers run on a serialized TCP command or receive worker, outside radio ingress, with
a three-second deadline and shutdown cancellation. They must honour their context
and must not call Server methods. Cached modem snapshots are preferable to live
hardware requests.
Without an ErrorHandler, companion reports operation errors to the default
structured logger; a failed persistence or radio operation is never silent.

Tests check companion wire replies, multi-client delivery and TX timing with
upstream Go clients and synthetic peers. Native MeshCore vectors in
testdata/parity cover encrypted text, ACKs, three-byte paths and airtime-factor
handling. Set MESHCORE_COMPANION_ORACLE to a newly generated events.jsonl to
compare against a fresh native run.
*/
package companion
