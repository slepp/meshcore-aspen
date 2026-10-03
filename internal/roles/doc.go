package roles

// Protocol and persistence notes
//
// The supported application contracts target MeshCore companion-v1.17.1.
// The node library owns crypto, deduplication and routing. internal/policy owns
// forwarding admission, scopes, path limits, timing and originated priorities.
// These Go handlers translate the upstream MyMesh/CommonCLI wire contract;
// they do not execute the native C++ application handlers. Native CommonCLI's
// board-local "set radio" persists tuning for reboot; a colocated role cannot
// independently retune the host's single shared PHY. The physical owner
// configures and verifies that profile; role-local "set radio"/"set tx"/
// "set freq"/"tempradio" return a mast-admin-required error rather than
// promoting role administrator credentials to mast owner.
// "get radio" reports the verified shared readback or an explicit unavailable
// error, never a stale per-role preference. "get tx" uses the same verified
// physical readback without granting role-local power control.
// Repeater forwarding defaults on; room forwarding defaults off. Synthetic
// local reflections are delivered locally but never retransmitted.
// Enabling room repetition retains its native hop-only admission, not the
// repeater's additional region/loop gate. Local reflections do not increment
// role-local RF receive counters or replace the last measured RSSI/SNR.
//
// Config.Policy applies explicit overrides over saved preferences. Omitted
// settings retain saved management changes, including explicit zero/disabled
// advert intervals. Deprecated AdvertInterval is an explicit flood-seconds
// override when positive and Policy.FloodAdvertSeconds is absent. Native defaults
// use one-byte originated paths, two-minute local adverts and 47-hour flood
// adverts. Startup emits a zero-hop advert eligible after 16 seconds. Learned
// and reply path widths remain independent of the originated path mode.
// Config.PreferenceProfile is independent of ACL/history retention. New state
// defaults to NativePreferences: saved AF values above 9 clamp to 9 on load,
// before explicit startup overrides. Runtime dutycycle 1 still applies and saves
// AF 99 without truncation. DurableHostPreferences is the named extension that
// preserves those runtime values on reload. Older documents without a preference
// profile migrate to this extension, preserving existing host behaviour. Both
// profiles preserve explicit factor zero and reject invalid runtime values.
//
// Supported application surfaces include password/ACL login, PATH responses,
// native ACL filtering and least-active non-admin eviction, status and basic
// telemetry, repeater level-2 owner/neighbour queries, discovery, rate-limited
// anonymous clock/owner/region queries, room signed posts, per-member cursors,
// keep-alive backlog ACKs, optional extra direct ACKs and three-failure retry
// suspension. Room attempts use random low-two-bit values, not a persistent
// retry tail. Native response/ACK/CLI delays and originated priorities reach
// TxRadio.Enqueue unchanged.
// Eviction replaces the original ACL table slot, preserving native ordering
// for subsequent equal-activity admission, enumeration and room polling.
// AccessList exposes the complete nonzero-permission local-console view.
// OwnerAdmin exposes scoped host-owner configuration through the same command
// engine and role worker: public help/version/board/clock, nonsecret preference
// and statistics reads, and name/repeat/path-width/advert-interval setters.
// ConfigureName narrows this to set name; Name supplies readback. The caller
// authenticates and authorizes the owner. Mutations retain the native command's
// persistence and policy-publication path; reads leave persisted role state unchanged.
// These adapters do not grant RF administrators or expose credentials, identity
// changes, regions, shared RF setters, explicit adverts or restart.
// Unsupported stats topics return the native error, not fabricated measurements.
// Cancellation after mutation admission may leave the outcome unknown; inspect
// affected settings before repeating. Closed, restarting and fail-closed roles
// reject owner commands. Neither adapter adds a host-wide Unix socket.
// Native binary ACL replies retain their size limits: a direct reply holds
// at most 23 entries; larger replies fail instead of inventing pagination or
// silently reporting a smaller ACL. Flood replies have less path-dependent space.
// New posts use the native storePost/strncpy limit of 150 bytes. Existing
// 151-byte history is retained during migration; byte-truncated UTF-8 is stored
// losslessly rather than rewritten by JSON string encoding.
//
// Management implements name/password/location preferences, clocks, ACLs,
// forwarding/path/delay/hop/loop preferences, advert timers, owner information,
// neighbour discovery/removal, room posting, and region configuration including
// default scope and bounded transactional loading. Region-load ownership is
// administrator-local. Explicit policy.Region.Keys are a host capability
// extension, not native private-keystore parity: the pinned saveKeysFor is
// unimplemented and rejects private-key persistence.
// Source airtime changes use the radio's
// SetSourceAirtimeFactor(context.Context, float64) method after committing the
// preference; a failed device update stops processing until reconciliation.
// Packet-log start/stop/erase and ReadPacketLog use bounded private JSONL, a named
// host storage-format extension. Capture stops with a reported error at the
// limit rather than overwriting records; local reflections retain null signals.
// Authenticated reboot uses Config.RequestRestart after state commit, outside
// the role mutex but on its worker, with a five-second admission context.
// The callback must enqueue a role-only supervisor action and return promptly:
// synchronous Close or waiting for completion would deadlock the worker.
// Success has no CLI reply or artificial reply delay (CommonCLI.cpp:185-186).
// Repeater legacy plain-text commands retain their 200 ms transport ACK queued
// before admission; the callback does not wait for RF completion. Missing or
// rejected admission gets an explicit error at the normal CLI reply delay.
// Accepted restart quiesces application writes, forwarding and periodic sends;
// the parent must complete Close before reconstructing or replacing role state.
// Administrator set prv.key accepts exactly 128 hex digits for a validated
// native scalar-prefix key. Optional Config.StageIdentity persists only the
// pending key, after command-state commit and outside the role mutex, with a
// five-second context. Nil disables import. Success is sent only after confirmed
// staging: "OK, reboot to apply! New pubkey: " followed by the uppercase public
// key. Active identity, adverts and crypto remain unchanged; no restart is
// requested. The parent activates through RebindStateIdentity only after Close
// and source retirement (or before initial service construction). The callback
// borrows read-only key bytes until return; they are then cleared. Provider
// errors are redacted, retaining indeterminate classification without their
// potentially sensitive text. Shared-store writes preserve the pending key.
// Physical-radio mutations, private-key export and erase remain unsupported;
// in particular this does not enable native local-only operations over RF.
// Nondefault airtime factors are rejected on radios without that source-policy
// method. Active RX/relay delay policies require Config.AirtimeEstimator;
// nonzero rxdelay additionally requires Config.RXScore. Positive RX durations
// pass through WithRxDelay; the node owns the 50 ms minimum and 32-second cap,
// not a second role-level hold queue. Native flood/direct relay priorities are
// preserved from the upstream router.
//
// Config.Telemetry supplies cached physical readings outside application locks.
// Native TX/RX airtime fields contain only supplied measured RF occupancy, never
// estimated airtime or queue/network latency. No reserved native fault bit is
// used as an availability marker. ErrorEvents is the native sticky error bitmask,
// not a TX failure count; leave HasErrorEvents false without equivalent native
// fault flag. Unavailable battery/counters/airtime are zero;
// unavailable noise/RSSI/SNR are -32768. Counters supplied by the physical modem
// are aggregate; routing counters are role-local. Base Cayenne LPP telemetry
// supports battery and MCU temperature, not external sensor-management APIs.
// Parent wiring can attach Service.LogTXResult with Link.AddTXResultHandler.
// Rejected/failed/unknown outcomes retain their packet and job correlation,
// separate queue/RF/estimated durations and null signal fields in TX_RESULT
// records. Admission is not logged as success; successful TX packets still use
// outbound DATA callbacks. The callback queues logging without physical RX I/O.
//
// Mutations commit before successful RF responses. NativeRetention persists
// room administrators/nonzero repeater ACL entries, with volatile replay state
// and room history. DurableReplay persists all membership, replay timestamps
// and bounded history as a named extension. Version-1 files migrate to that
// extension with a private state.v1.json rollback copy; migration never silently
// deletes history. Explicit native retention is refused while saved history
// would be lost. Post-rename directory-sync failures stop application processing
// until restart/reconciliation instead of pretending to roll back the disk.
// State reads/writes use internal/state.ReadRoleJSON/WriteRoleJSON exclusively;
// an identity/state envelope takes precedence over retained legacy files, and
// explicit null reset state never revives those files. The rollback copy is made
// from the selected version-1 document. state.ErrCommitIndeterminate remains
// discoverable through errors.Is, and Close cannot retry the failed commit.
// A known-aborted write remains reported until a later successful commit
// clears it, allowing a recovered role to complete a requested restart.
//
// Provisioned passwords and shared secrets are not copied to state. Explicit
// native password-management changes are persisted in the private state file.
// Byte-truncated non-UTF-8 passwords use explicit binary override fields rather
// than lossy JSON strings; existing string overrides remain compatible.
// Empty administrator passwords remain disabled. Room ACL roles 0 (guest) and
// 1 (READ_ONLY) cannot post or receive a post ACK; roles 2 and 3 can. Role 1 is
// intentionally stricter than companion-v1.17.1 simple_room_server/MyMesh.cpp,
// whose plain-text handler denies only role 0. Assign role 2 to allow posting.
// CLI commands still require role 3. An all-admin ACL cannot evict an
// administrator; ordinary path overflow is rejected.
// StateDir must be exclusive to this role. Identity remains fixed for a running
// service. ErrorHandler must not block or call Close.
//
// Native fixture tests compare originated priorities and MeshCore helper outputs
// with the saved vectors or a fresh MESHCORE_POLICY_ORACLE run.
