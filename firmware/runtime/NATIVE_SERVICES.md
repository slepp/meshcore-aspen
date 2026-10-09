# Native service hooks

Add an on-device service without giving it the command bot's VM, source
installer or identity. The room frontend now takes a `NativeNetworkHost`
from [`NativeServices.h`](NativeServices.h), rather than `CommandBot`.
Aspen's command bot supplies that host through its existing HTTPS worker.
The host can start with the bot disabled; enabling a room does not enable
commands or change the bot's saved configuration.

This is a compile-time service boundary, not a runtime plugin installer.
The host accepts **two** network services through a fixed-capacity registry.
Each registration declares a unique name, a socket reservation and a cooperative
work limit. Services, their configuration and their name strings must outlive
the host. An unconfigured room frontend stays disabled.

For dispatch-task raw packet modules, use the separate
[native packet engine interface](PACKET_ENGINES.md). Network callbacks must
not call those hooks or the physical radio.

## Hooks and ownership

| Hook | Caller | Contract |
| --- | --- | --- |
| `ensureNativeHttps()` | Dispatch task at startup | Start/reuse the native HTTPS worker; failure leaves the service disabled |
| `attachNetworkService(service, budget)` | Dispatch task after initialization | Publish a fully initialized service or return a specific registration result; rejected resources remain caller-owned |
| `NativeNetworkService::poll(work)` | HTTPS task, after pending HTTP work | Consume at most the supplied work units; no direct shared-radio calls |
| `NativeNetworkService::close()` | HTTPS task at shutdown, reverse registration order | Invalidate connections and prevent new admission; called once per accepted registration; no automatic replay |
| `beginCloudRoom(mux, host)` | Dispatch task at startup | Allocate queues/radio, load saved configuration or the build provider and register network hooks |
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
The generic image loads [saved frontend settings](../../services/shared-room/NATIVE.md#configure-the-generic-image)
through this same boundary; a private build provider remains available to
custom applications. Configuration changes apply after restart.

## Registering a module

Keep protocol/state-machine code in `firmware/shared/<service>` and board
adapters beside the board runtime. The current room split already follows
this arrangement: `shared/cloudroom` is portable, `esp32/CloudRoomService`
owns board resources, and `services/shared-room` owns the backend.

Implement `NativeNetworkService`, initialize all queues before registration
and provide a dispatch-side begin/poll pair. Use a build flag and explicit
runtime wiring, not static constructor registration. Each module needs its
own queue and connection-generation domain.

Register from the dispatch task with a `NativeServiceBudget`:

```cpp
const auto result = host.attachNetworkService(
    service, {"my-service", 1, 4});
if (result != NativeServiceRegistration::Attached) {
  // The module releases its own unregistered startup resources here.
  Serial.printf("My service registration failed: %s\n",
                nativeServiceRegistrationText(result));
}
```

The name is 1..31 printable ASCII bytes without spaces. Names and service
objects must both be unique. Work is 1..65535 units per network pass; define
what a unit does in the module and honor the supplied quota in `poll(work)`.
The room module counts each received packet, final TX result and alias visit
as one unit. Its quota is `2 * QueueDepth + active aliases`, preserving its
existing queue and socket work limits.

Registration reports `Unavailable`, `Stopping`, `InvalidBudget`, `Duplicate`,
`Full` or `SocketBudget` without publishing the rejected service. On failure,
undo any admission flag, detach unregistered radio resources and free startup
buffers/sockets. Do not replace an existing module, leave partial initialization
active or silently retry registration with smaller reservations.

One dispatch task owns registration. The HTTPS task sees only fully published,
immutable entries. Each pass visits every registered service once and rotates
the starting service. These are cooperative work limits, not wall-clock
preemption: socket operations still need bounded timeouts, and a synchronous
TLS connection attempt can delay other services on this task. Physical radio
polling/admission remains on the dispatch task.

## Socket reservations and shutdown

`ONCHIP_NATIVE_SERVICE_SOCKETS` reserves the registry's aggregate socket
capacity. Its default is the room profile's `ONCHIP_CLOUD_ROOM_CONNECTIONS`,
or zero when the room is not built. Accepted module reservations cannot exceed
that total. The current profiles keep their existing reservations; adding
registry slots does not create sockets or enable another service.

For a room using one socket and a second module using one socket, build with
`ONCHIP_NATIVE_SERVICE_SOCKETS=2` and `KISS_MAX_TCP_CLIENTS=1`. The room's own
connection count stays one. `Capacity.h` rejects insufficient room reservations,
more than two service sockets, missing HTTPS support or an inconsistent KISS
client count. It retains the combined SDK limit of 16 sockets. Do not expand
the shared radio's airtime budget to accommodate a module.

Stopping seals registration before waiting for the network task. That task
closes accepted services in reverse registration order, once each, before its
network resources are destroyed. A registration racing shutdown is either
published and closed or rejected and left to its caller; it cannot be skipped
during cleanup. There is no live unload/replacement. Keep service objects alive
until the host has stopped, and use explicit startup order for dependencies.
Reconnects and host restarts do not authorize automatic replay of uncertain
operations.

## Local checks

```sh
python3 -m unittest discover -s firmware/esp32/tests -p test_native_services.py -v
python3 -m unittest discover -s firmware/esp32/tests -p test_prepare.py -v
make -C firmware/esp32 bot-native-services-test
make -C firmware/shared/cloudroom test
```

These commands check registration refusals, per-pass quotas, round-robin order,
shutdown races, caller-owned rollback, socket profile compilation, the actual
HTTPS worker's callback ownership, and room queues/disconnect generations.
They use local fixtures without a radio or credentials. An enabled board build
also needs its selected public or private HTTPS profile. Existing radios need
no update merely to keep their current room service running; install a registry
build when adding a native module.
