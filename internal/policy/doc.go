package policy

// Native reference: MeshCore companion-v1.17.1, commit
// d92964352441e53b93e8667b802e04f6e072b39e.
//
// Preference defaults: examples/simple_repeater/MyMesh.cpp:890-907,
// examples/simple_room_server/MyMesh.cpp:647-665 and
// examples/companion_radio/MyMesh.cpp:878-895. Timing:
// repeater:539-554, room:278-297, companion:268-280 and src/Utils.cpp:18-22.
// Priorities and flood capacity: src/Mesh.cpp:344-355,637-738.
// Reply decisions: src/helpers/RoutingPolicy.h:15-65.
// Region lookup and keys: src/helpers/RegionMap.cpp:173-205 and
// src/helpers/TransportKeyStore.cpp:4-24,37-49.
// Loop limits: examples/simple_repeater/MyMesh.cpp:396-455.
// Advertisement timers: examples/simple_room_server/MyMesh.cpp:810-820,1041-1052
// and src/Dispatcher.cpp:382-386 (strict deadline comparison).
//
// Defaults do not encode deployment overrides. Consumers first load or migrate
// persisted Preferences, then ApplyOverrides. Management commits must persist
// validated preferences before activating them. Native CLI interval commands
// have narrower input ranges/quantization than the underlying preference
// storage; command parsing remains with the role that owns its management API.
//
// Durable host preferences intentionally retain any finite nonnegative float32
// airtime factor, including native runtime dutycycle 1 -> factor 99 and factor 0.
// Native CommonCLI.cpp:450-465 accepts these at runtime but clamps to 0..9 when
// loading at line111. The host does not apply that boot-only clamp or silently
// round to thousandths: persistence retains the successfully applied runtime
// value. Nonfinite and negative factors are explicitly rejected.
//
// Native MeshCore tests cover public region derivation and transport
// authentication. Explicit private Region.Keys are a host capability
// extension, not native keystore parity: the pinned TransportKeyStore.cpp:70-76
// saveKeysFor clears its cache and returns false without storing any keys.
//
// Callers supply clocks, estimated RF airtime and an exclusive-upper-bound RNG.
// AdvertisementSchedule stores process-local monotonic deadlines, never durable
// wall-clock timestamps. ReceiveContext is captured before asynchronous work.
// Timing functions calculate eligibility only: physical arbitration, RF airtime
// budgets and transmission completion remain outside this package.
