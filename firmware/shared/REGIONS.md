# Configure can → ab → edm regions

Set up the repeater's region tree with the exact lowercase names `can`, `ab`
and `edm`, then make `ab` its home:

```text
region def can ab edm
region home ab
region save
region
region home
```

The tree should contain:

```text
* F
 can F
  ab^ F
   edm F
```

`region home` should reply `home is ab`. `^` identifies the home entry; `F`
allows flooding for that entry. `region def` preserves existing unrelated
entries, moves any existing named entries into this hierarchy and enables
their flood permission. Inspect the full tree before saving. Repeat this
configuration for each repeater identity; roles on a shared modem do not share
their region tables.

## Use the running role's administration

- **Aspen:** connect a companion/native MeshCore app and use the repeater's
  authenticated administration console, or its native repeater CLI.
- **Pine:** use the serial repeater console or authenticated encrypted repeater
  administration. The separate bot identity is not the repeater.
- **Birch:** use its existing local role owner command surface, or the host
  admin page's **Scoped role command** field. Select the running repeater.
  The same field accepts `region`, `region home`, `region default`,
  `region put`, `region def`, `region allowf`, `region denyf`, `region remove`
  and `region save`. Interactive `region load` is not accepted by the HTTP
  owner console.

Keep HTTP administration on localhost or behind a trusted TLS tunnel. It
requires existing host-admin authentication and preserves the selected role's
native validation/persistence. Region settings do not modify radio frequency,
bandwidth, SF, CR, power, path width, identities or administrator credentials.
A lost write reply requires readback, not automatic repetition.

## Home, hierarchy and outgoing scope are different settings

Home is region-table metadata, not the outgoing flood scope and not an
inherited permission. At the pinned upstream revision, the repeater's outgoing
second transport code remains zero; selecting `ab` as home does not encode it
in that field. `region save` retains the home and tree across firmware-role
restart. A bulk replacement is a separate operation: reselect home and default
scope after loading a replacement table.

For a role that should **originate ab-scoped floods**, separately use:

```text
region default ab
region default
```

Leave the default unchanged if that role should continue originating unscoped
floods. `region default <null>` clears the outgoing scope. Replies to scoped
requests continue using the matched region according to the native protocol.

The public transport keys are derived from `#can`, `#ab` and `#edm` individually.
Names are case-sensitive: `ab` and `AB` are different names and keys.
An optional leading `#` is ignored when looking up an existing entry. `$` names
are private regions and require explicit transport keys; do not substitute
them for these public region names.

Allowing `ab` does not automatically allow `edm` or `can`. Configure each entry:

```text
region allowf can
region allowf ab
region allowf edm
region list allowed
```

The wildcard `*` controls **unscoped floods**, not all descendant regions.
Use `region denyf *` only when intentionally rejecting forwarding of unscoped
floods. `region denyf edm` blocks edm-scoped forwarding even with `ab` allowed.
These forwarding decisions do not turn a region table into a local-receipt ACL,
nor do they rescope ordinary direct packets.

## Reference and checks

The commands and semantics above follow MeshCore's pinned
[`docs/cli_commands.md`](https://github.com/meshcore-dev/MeshCore/blob/d92964352441e53b93e8667b802e04f6e072b39e/docs/cli_commands.md),
[`RegionMap.cpp`](https://github.com/meshcore-dev/MeshCore/blob/d92964352441e53b93e8667b802e04f6e072b39e/src/helpers/RegionMap.cpp)
and repeater `sendFloodScoped` implementation.

Host tests in `internal/roles/regions_home_test.go` check the owner command path,
restart persistence, exact case, hierarchy and independent scoped admission.
The native region fixture in `test_support/parity/oracle.cpp` checks the same
home/hierarchy behavior against the pinned RegionMap implementation. Neither
test changes a deployed node. After configuring a physical repeater, re-read
`region`, `region home` and `region default` after a planned restart and check
the chosen scopes over RF separately.
