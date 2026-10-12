# Upgrade Willow user services from source

Use `install_willow.py` to build a new Willow installation and upgrade existing
Linux systemd user services. It keeps role identities, saved traffic, guest
selections, grants and guest rollback history. It does not create identities,
configure a radio or install a compiler. Keep your existing installation for
rollback.

Install the normal build prerequisites described in
[the service guide](../experiments/hew-roles/SERVICE.md#build-and-install-from-source).
Build the current Hew toolchain with `make hew` and put its `build/bin` launcher
on `PATH`, with matching libraries and std. A debug compiler launcher can link
an unoptimized runtime even when the application uses `--release`.

Create a private JSON plan, mode 0600. List each existing unit independently:
a dashboard can run from a different installation than its host or Base.
Use absolute paths and the full source commit. Every destination must be new.
The `source` field names a Git checkout; the tool exports the specified commit,
including the archive substitutions used by `release/source.json`, directly to
`install`. Uncommitted files are excluded. Native-worker binding and builds run
at that final path.

This example updates only the dashboard and guards the existing host. Add the
two Base services and broker as unselected guards with their actual paths when
using the full stack. Names and directories are examples; inspect your units
with `systemctl --user show UNIT --property=MainPID,ExecStart` first.

```json
{
  "source": "/srv/willow/source",
  "revision": "0123456789abcdef0123456789abcdef01234567",
  "install": "/srv/willow/install/new",
  "workspace": "/srv/willow/private/upgrade-new",
  "ready_url": "http://127.0.0.1:9080/readyz",
  "status_url": "http://127.0.0.1:9080/status",
  "services": [
    {
      "unit": "willow-dashboard.service",
      "kind": "dashboard",
      "current_install": "/srv/willow/install/dashboard-old",
      "config": "/srv/willow/private/dashboard.conf",
      "next_config": "/srv/willow/private/dashboard-new.conf"
    },
    {
      "unit": "willow-host.service",
      "kind": "host",
      "current_install": "/srv/willow/install/host-old",
      "selected": false
    }
  ]
}
```

```sh
chmod 600 upgrade.json
python3 tools/install_willow.py plan upgrade.json
python3 tools/install_willow.py apply upgrade.json
```

`plan` reads the plan and checks its path relationships without building or
changing services. `apply` builds with the current PATH compiler, two CPUs,
a 4 GiB per-process address-space limit and a 15-minute build timeout. Only
selected production targets are built; a host selection also verifies the native
worker binding. Run the affected source tests before upgrading, as described in
the service guide. Build output stays in the private workspace. A build failure
stops no service. Inspect the log and use fresh install/workspace paths after
correcting a failed build.

For a host or Base upgrade, add `state` and `next_state`. The host kind uses
`willow.py rebind`; a Base uses `migrate_base.py` to retain its identity and
companion journal. A selected host launches `willow.py run --release --state
NEXT_STATE`. Other kinds use their corresponding release binary and the copied
`next_config`. `base`, `dashboard` and `broker` need `config` and `next_config`.

List services in **stop order**, with consumers before their owners; the tool
starts them in reverse order. For the usual full stack, stop dashboard, host,
main Base, secondary Base, then broker. Include every service that refers to a
moving state directory. An unselected service's listed configuration is checked
for such references and refused when it would keep using old state.

The helper requires each unit's existing `70-willow-current.conf` to contain an
empty `ExecStart=` reset and one command. It replaces that file's command and
working directory, retaining its environment and other options. Other drop-ins,
unit files and environment files remain untouched. Keep `HEW_WORKERS=4` in the
existing service configuration. No compiler path is written to a service.

Before stopping a host whose observer lacks durable checkpoint fields, the tool
requires its observed-minus-published-minus-dropped count, queue and pending item
to be empty. Wait for its broker to drain outstanding records. An older host can
still lose a record arriving between that check and shutdown. Durable observers
may retain backlog; active save/restore errors refuse the upgrade, and a stopped
backlog requires an owned private `observer.outbox` file. This checks checkpoint
presence; it does not independently validate the final flush. No checkpoint payload
is interpreted by the installer.

After the build passes, the tool verifies the selected executables, current
readiness and public identities, then stops only selected units and waits for
their children to exit. It locks and copies the latest stopped state, including
`observer.outbox`; unfinished transactions are refused by the owning migration
helper. Configuration bytes are copied privately. References to moving state
and that service's old install are rewritten to their new paths. Other
references, including credential files and environment variable names, remain.
It never prints configuration or credential values.

A failed or uncertain stop leaves its receipt for inspection and never restarts
an owner automatically. A later staging failure restores the saved overrides and starts the old owners. If a
child remains, it stops recovery and reports that condition instead. Once a
candidate may have started, failures require inspection: no uncertain switch is
replayed and no automatic rollback discards new traffic. Successful completion
requires the expected executable, readiness and unchanged public identities;
unselected service PIDs must remain unchanged. Exact prior overrides, frozen
state, build logs and `receipt.json` remain private in the workspace.

## Roll back using current saved traffic

For a completed upgrade, stop and stage the latest candidate state into fresh
rollback paths with:

```sh
python3 tools/install_willow.py rollback /srv/willow/private/upgrade-new/receipt.json \
  --workspace /srv/willow/private/rollback-new
```

This uses each saved old install's migration helpers and retains traffic saved
since the upgrade. It never restores the old frozen-state snapshot over new
traffic. Previous installations must still contain their original source,
worker and binding files. The same stop, lock, override and identity checks
apply; the rollback gets its own receipt. Inspect incomplete receipts and actual
owners before attempting recovery. Do not edit a receipt to replay a switch.

The helper updates existing user services only. Initial identity provisioning,
Go-to-Willow migration, compiler installation and removal of old installations
remain separate operations. No RF transmission is used as an upgrade check.
