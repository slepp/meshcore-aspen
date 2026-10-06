# Writing about MeshCore KISS

Write for someone building, running or extending a mesh node. Lead with what
they can do, which device or service does it, and the next useful command or
decision. In the README, put a short route to a working radio and host or
standalone setup before implementation details and test harnesses. Keep wiring,
protocol contracts and validation procedures in their focused guides.

- Call the devices what they are: a MeshCore radio, a companion radio, the
  shared modem, a host role or an on-device role. Do not frame ordinary
  over-air interaction as a special encounter with a "stock peer". Use exact
  upstream revisions and protocol versions where a build or checker requires
  them, not as qualifiers on every mention of a radio.
- Describe behavior and consequences directly. Say which messages arrive,
  which role owns an identity, what remains after a restart, and what an
  operator must configure. Keep distinctions that affect decisions: RF
  reception versus local reflection, a confirmed transmission versus an
  uncertain one, or flashing that erases an identity.
- Prefer "the companion receives the advert over RF" to "independent
  stock-peer reception provides evidence of OTA parity". Use ordinary
  technical terms when they carry meaning; do not replace precise protocol
  names, commands or units with vague reassuring language.
- Do not narrate the project's development or the author's reasoning in
  operator copy. Avoid claims of proof, certification, exhaustive coverage,
  provenance or "not X" disclaimers unless they answer a real deployment
  question. A test guide can say what a command checks and what it changes;
  it need not argue for the importance of the check.
- Keep errors and status text actionable: name the affected connection,
  packet, setting or role and the condition that failed. Preserve established
  command names and machine-readable fields when editing human-facing text.
- Review a page as a new reader: can they find the supported setup, the
  command to try, the expected result and any destructive or security warning
  without reading implementation history? Move deep mechanics to a linked
  reference rather than repeating them in the getting-started path.

These guidelines apply to README text, firmware and host guides, command help,
errors, status messages and dashboard copy. Protocol reference material still
needs exact wire formats, limits and failure behavior.

# Practical engineering and releases

Aspen is a practical mesh-node suite, not a certification program. Spend time
on working behavior, usable setup and recovery, and bugs that affect operators.

- Test changed behavior and its directly affected interfaces. Reuse established
  results for unchanged code; do not restart broad validation after a tooling or
  documentation edit. Repeat a check only when new evidence or a relevant change
  makes its result uncertain.
- Preserve identities, settings and useful data during maintenance. Back up
  valuable state, not whole flash by default. Keep credentials and private node
  backups out of public source and release assets.
- Keep release manifests factual: source revision, supported hardware,
  dependencies, layout, versions and file hashes. Do not add qualification
  labels, certification language, evidence dossiers or public test receipts.
  Keep test logs internal unless the operator asks to publish them.
- Build, test, tag and publish through a short, practical release procedure.
  Obtain necessary publication approval without repeated approval gates or
  asking the operator to approve a list of hashes. Checksums identify files;
  they are not an approval ceremony.
- State real limitations and destructive-operation warnings directly. Do not
  label tested releases as "unqualified" or perpetually "pending" merely because
  the build tool cannot know the outcome of later tests.
- Do not promote old plans, checkpoints, test matrices or speculative risks
  into new requirements. The operator's current scope controls the work.
  Unrelated products, future features and unsupported hardware do not block an
  Aspen release.
- Prefer the simplest change that fixes the problem. Before adding a gate,
  check, artifact or abstraction, identify the concrete operator outcome it
  protects. If it adds ceremony without useful coverage, leave it out.
- Stop when the requested outcome works and is saved. Do not invent further
  qualification work after the relevant checks pass.
