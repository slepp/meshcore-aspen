# Native service hooks

Add an on-device service without giving it the command bot's VM, source
installer or identity. The room frontend now takes a `NativeNetworkHost`
from [`NativeServices.h`](NativeServices.h), rather than `CommandBot`.
Aspen's command bot supplies that host through its existing HTTPS worker.
The host can start with the bot disabled; enabling a room does not enable
commands or change the bot's saved configuration.

This is a compile-time service boundary, not a runtime plugin installer.
The current host accepts **one** network service; a second registration
returns false. The service and its immutable configuration must outlive the
host. An unconfigured room frontend stays disabled.

## Hooks and ownership

| Hook | Caller | Contract |
| --- | --- | --- |
| `ensureNativeHttps()` | Dispatch task at startup | Start/reuse the native HTTPS worker; failure leaves the service disabled |
| `attachNetworkService(service)` | Dispatch task after initialization | Publish one fully initialized service; reject occupied or stopping hosts |
| `NativeNetworkService::poll()` | HTTPS task, after pending HTTP work | Bounded network work; no direct shared-radio calls |
| `NativeNetworkService::close()` | HTTPS task at shutdown | Invalidate connections and prevent new admission; no automatic replay |
| `beginCloudRoom(mux, host)` | Dispatch task at startup | Allocate queues/radio, load the private provider and register network hooks |
| `loopCloudRoom()` | Dispatch task each iteration | Receive radio packets, consume final TX results and admit bounded queued operations |

The room frontend's sockets, opaque driver and network callbacks live on the
HTTPS task. Its `LocalRadio`, native TX jobs and radio callbacks live on the
dispatch task. SPSC queues cross that boundary. Queue payloads use PSRAM;
atomics, admission control and the worker stack remain in internal memory.
Disconnect invalidates the connection generation under the same admission
mutex used by physical transmission. A reconnect does not replay a popped
operation whose transmission is uncertain.

The Worker owns room identities, membership, history and native ACK cursors.
The frontend owns no room private keys and retains no replacement history.
`cloudRoomConfiguration()` and `createCloudRoomDriver()` remain the private
build-provider seams; default configuration starts no identities or sockets.
`cloudroom status`, `cloudroom error` and explicit `cloudroom advertise ALIAS`
retain their existing command names.

## Adding another module

Keep protocol/state-machine code in `firmware/shared/<service>` and board
adapters beside the board runtime. The current room split already follows
this arrangement: `shared/cloudroom` is portable, `esp32/CloudRoomService`
owns board resources, and `services/shared-room` owns the backend.

For the next native module, implement `NativeNetworkService`, initialize
all queues before registration and provide a dispatch-side begin/poll pair.
Use a build flag and explicit runtime wiring, not static constructor
registration. Report initialization failure with the service/connection
name; do not enable a partially initialized service.

**Before enabling two network modules**, add a fixed-capacity host registry
with explicit per-module work/socket budgets. Replace the single registration
only with tests for duplicate/full registration, startup rollback, ordered
shutdown and radio isolation. Each module needs its own queue and generation
domain. Count its sockets in `Capacity.h`; it must not increase the shared
radio's airtime budget or block native role polling. No extra module registry
or socket capacity is enabled by this change.

## Local checks

```sh
python3 -m unittest discover -s firmware/esp32/tests -p test_native_services.py -v
make -C firmware/shared/cloudroom test
```

The first command compiles the independent service interface and the disabled
room adapter with a fake host. The second exercises radio queues, disconnect
generations and opaque frontend operations without a radio or credentials.
An enabled board build also needs its selected public or private HTTPS
profile. This interface-only change needs no live firmware update.
