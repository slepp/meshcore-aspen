# Willow production supervision

Run the actual service fault tests before installing a new build:

```sh
cd experiments/hew-roles
make supervision-test
```

The seven groups run debug and release services through a loopback modem. They
panic real production actor handlers while processing authenticated packets;
they are separate from the small generic runtime fixtures.

## Independent branches and ownership

| Branch | Supervised child | Recovery boundary |
| --- | --- | --- |
| Relay | `service.RoleWorker` | Its owned role-state actor and committed relay snapshot |
| Room | `service.RoleWorker` | Its owned role-state actor and committed room snapshot |
| All-Hew bot | `service.RoleWorker` | Its owned role-state actor and committed notes/state |
| Native bot | `process.WorkerOwner` | The actual IPC child actor, descriptor and native subprocess |
| Modem | `supervised_io.Endpoint` | The actual TCP connection actor and descriptor |
| Observer queue | `observer_queue.Outbox` | Bounded in-memory RF admissions and one pending encoded event |
| Observer publisher | `observer_client.Publisher` | MQTT connection; replacement retries the outbox's pending bytes |

These are production declarative `one_for_one` supervisors, each with
`intensity: 2 within 60s`. The service holds typed stable `ChildRef`s, including
inside the MKISS session. The branches do not share a root restart budget.
The all-Hew and native bot paths remain different implementations; native
traffic passes through `WorkerOwner`, not through an unused all-Hew supervisor.

A role worker owns its state actor through a resource lease. Panicking the
worker stops that owned actor; the replacement creates another and loads the
committed snapshot. Actor-RPC failures in normal processing escalate to the
supervised owner rather than silently continuing with a dead state actor.
Per-role environment actors retain the current airtime table, flood reservation,
quarantine flag and incarnation counter across owner replacement. They are
metadata actors, not a second copy of the role's durable application state.

The service coordinator, ephemeral MKISS session/decoders and metadata actors
are not independently supervised. Their own programming panics are outside
these recovery budgets; process restart remains the fallback. The modem branch
supervises socket ownership, not an automatic replay of the negotiation routine.

The observer driver is a scoped native task alongside the modem coordinator.
It continues stepping the supervised publisher during modem negotiation and
reconnect waits. Admission receives only logical port 3; the other copies still
reach relay/room/bot. Neither the outbox nor publisher can submit RF packets.
The driver is joined before its supervisors stop. Its lifetime actor and the
driver itself are outside the child restart budgets, like the coordinator.
Publisher replacement preserves pending bytes in the separate outbox. Outbox
replacement or process restart loses its in-memory queue and starts a new
observation session; `observer.seed` remains unchanged.

Restart budgets cover **Hew actor failures**. Ordinary EOF, socket errors and
native subprocess exit are typed transport events. Existing bounded
reconnect/worker backoffs still coordinate those events; declarative supervision
does not replace MKISS negotiation or invent successful native TX outcomes.

## Persistence and quarantine

Fresh state is committed before a role accepts packets. A private
`roles.initialized` record contains `RLS1` followed by the relay, room and bot
public keys, in that order. It prevents subsequent startup from treating a
missing initialized identity or role snapshot as a new node.
Existing snapshots also prevent missing-seed regeneration when upgrading an
older Willow directory that does not yet have this marker.

On a role incarnation's first operation:

1. Check the role's quarantine flag and require its committed snapshot.
2. Create a state actor using the existing seed and configuration.
3. Validate/load the snapshot, including identity and structural bounds.
4. Restore current airtime and advert scheduling without sending a startup
   flood just because an actor restarted.
5. Publish `ROLE_RESTORED state=... generation=N`.

Missing/corrupt snapshots quarantine that role. Failed commits also quarantine
it and suppress the affected replies. Neither path promotes a partial staging
file or substitutes empty application state. A repaired file is not silently
adopted by an already-quarantined role: restart the process to reload it.
Missing initialized seeds remain a process-startup error to prevent identity
replacement. Initial incomplete imports are still refused.

Active sessions, pending room deliveries, dedup windows and deferred all-Hew
commands are reconstructed according to the existing restart contract, not
resumed as if nothing happened. Clients log in again. Durable history, ACLs,
replay stamps, cursors and notes survive. An operation committed before the
actor crashed may have no transmitted reply; its retry must obey the persisted
replay/idempotence contract.

## Modem and native worker

A modem actor restart closes its old socket. The new incarnation starts
**without a connection**: the coordinator first retires the old session's jobs
as unknown, then opens and negotiates another epoch through the same ChildRef.
No background redial sends old-generation frames on a new connection. Epoch
decoder, receiver and session actors are explicitly stopped.

A native owner restart closes/reaps its old subprocess. The coordinator fences
old bot TX, verifies the bound worker, launches it and performs HELLO before
accepting new bot work. Native NVS/SPIFFS remain authoritative. The old PID
does not survive replacement; signed contacts must be rediscovered. A modem
reconnect alone can retain a healthy native owner/process and contacts.

Exhausting one role's supervisor budget leaves that role unavailable, not
replaced with a new supervisor. Healthy branches continue. Runtime exit status
is nevertheless 1 at eventual shutdown because the exhausted fault is
unrecovered. Exhausting the modem budget leaves RF offline until process
restart; it does not spend another role's budget.

Native hardware faults inside Hew remain process-fatal. Lua/Wasm stays in a
subprocess; no signal-catching or `longjmp` recovery is introduced.

## Actual service regressions

* Room panic before commit: no reply or mutation reaches the committed
  snapshot; replacement loads durable history and accepts the uncommitted retry.
* Room panic after commit: the reply is absent, but the saved replay stamp
  survives and rejects the retry. A later valid login succeeds, without a
  new startup flood.
* Missing/corrupt room state: relay and bot remain usable, no empty room
  snapshot is created, and quarantine persists across restart until repaired.
  The migration suite also removes a room snapshot after an imported node's
  first start: the ordinary launcher preserves the identity marker, quarantines
  the room and keeps the imported relay and native bot data usable.
* Room budget exhaustion: relay, bot and modem continue, including another
  modem epoch; the room does not reappear with a reset budget.
* Modem panic with an accepted pending job: that job becomes unknown once,
  the replacement negotiates another epoch, and room state/native contacts
  remain usable.
* Native IPC-owner panic: old child is reaped, relay/room continue in the same
  modem epoch, and the replacement bot reads its previously saved private note.
* Normal service builds ignore the fixture injection files.

The test executable differs only in the compiled `MC_SUPERVISION_TEST` gate.
It enables local private fixture files to trigger selected actor panics;
ordinary service binaries never read those files for fault injection.
The generic resource fixture additionally checks cleanup and stable ChildRef
passing independently of MeshCore state.

Continuity checks introduce a contact with a zero-hop direct signed advert
before sending its DM. The pinned native dispatcher delays incoming floods
according to packet length, SNR and airtime; a shorter DM can overtake its first
flood advert and arrive before the contact exists. An isolated back-to-back
exchange reproduced that behavior: the first DM was ignored, the next DM
after contact processing succeeded. A direct advert preserved processing order.
The same native dispatcher runs in Birch; no worker deadlines or permissions
are relaxed to make these tests pass.

## RF settings across replacement

Authenticated RF settings are committed in the same transaction as member
replay stamps. The HEW5 extension stores supported preferences alongside the
existing member-attempt and optional-history representation. A replacement
role validates the complete record before adopting any saved setting.

The production regression sets a room name through authenticated packets,
panics its actual owner before a later query commits, and resolves the stable
ChildRef replacement. The same request then reads the committed name.
Relay forwarding, native bot replies and the modem connection continue.

[RF management](MANAGEMENT.md) lists the supported binary, discovery,
anonymous and CLI requests, and the remaining remote-management surfaces.
Go serialized differential checks cover these contracts separately from
actor/process fault tests. Neither set of host tests replaces physical RF
acceptance of the configured node.
