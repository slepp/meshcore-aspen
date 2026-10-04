# Native MeshCore chat

Send short direct messages from a companion radio and keep listening for one
contact's replies on the same TCP connection. The radio remains on its existing
PHY, identity, name and roles. Run from the repository root:

```sh
make -s -C cmd/meshcore-chat test
make -s -C cmd/meshcore-chat run ARGS='send --host COMPANION_HOST --port 5000 \
  --expected-sender FULL_SENDER_PUBLIC_KEY --peer FULL_CONTACT_PUBLIC_KEY \
  --advertise --message "Hello; reply here when ready." --listen \
  --log ../../.tmp/meshcore-chat/dialogue.jsonl'
```

Replace `COMPANION_HOST` with your companion service's address and both keys
with complete 64-character public keys. Repeat `--message`
for a batch. Each message must be 1..120 printable ASCII bytes. `--advertise`
sends one normal native **flood advert** before the messages. Each DM uses
**direct zero-hop routing**, with no discovery or fallback flood. Sending adds
a missing contact with a `peer-KEYPREFIX` label or updates its route length;
an existing name, key, flags, coordinates, advert timestamp and unused path bytes
are preserved and read back. The server may advance the contact's last-modified
timestamp for the route update. No other contacts are changed.

For a receive-only session, with the contact already present:

```sh
make -s -C cmd/meshcore-chat run ARGS='listen --host COMPANION_HOST \
  --expected-sender FULL_SENDER_PUBLIC_KEY --peer FULL_CONTACT_PUBLIC_KEY \
  --log ../../.tmp/meshcore-chat/replies.jsonl'
```

The foreground process emits JSON lines to stdout and, optionally, a private
append-only log under repository `.tmp`. Every event includes its UTC
`observed_at` and actual Python `pid`. The log permits only one writer. Stop
with Ctrl-C. Python 3.9+ and its standard library are sufficient.

## Wait for one reply without another radio connection

An automation process can keep its own foreground wait on the existing
listener's private log. This command prints exactly the next authenticated,
full-key-verified DM JSON object for the selected contact, then exits:

```sh
make -s -C cmd/meshcore-chat wait-reply ARGS='\
  --log ../../.tmp/meshcore-chat/dialogue.jsonl \
  --peer FULL_CONTACT_PUBLIC_KEY --after-id LAST_PROCESSED_MESSAGE_ID \
  --timeout 43200'
```

`--timeout` is in seconds and defaults to 12 hours. `--after-id` must identify
an authenticated DM for that peer already in the initial log scan; a missing
ID is an immediate error. Omit it to start with the first matching record.
The command skips other events, unverified messages and previously scanned
message IDs, then waits for appended rows, retaining an incomplete final line
until its newline arrives. A matching record already after the cursor returns
immediately. The returned object's `backlog` flag is preserved; queued history
is not relabeled as a fresh message.

The file is opened read-only without taking the listener's writer lock or
opening any TCP connection. It must already exist under repository `.tmp`,
be a regular file, and have no group/other permissions. Missing, replaced,
truncated or malformed logs and timeouts produce a JSON error on stderr and
a nonzero exit (CLI exit 1; Make reports target failure). Rearm with the
returned `message_id` after processing the reply. Neither this command nor
the running listener sends an automatic response or executes received text.

## Delivery and replies

`send_requested` records the permitted text before submission. `sent` means the
companion admitted it, **not** that the recipient received it. `ack` matches
the native send-confirmed tag to this process's permitted sends and includes
the native RTT. `delivery.outcome` is `acknowledged` or `unknown`;
`--ack-wait` defaults to 30 seconds. Late matching ACKs are still reported.
The default spacing after the advert and between DMs is three seconds.
An admitted advert has no delivery ACK; its RF reception remains unknown.

`dm` contains the requested sender's full key, six-byte prefix, sender timestamp,
text, native text type, route and available SNR. Authentication is performed
by the companion's native encrypted-DM receiver, not by this TCP client.
The client verifies the full contact key and rejects ambiguous six-byte
prefixes. Native queue forms 7 and 16 are supported, including signed-plain
messages' four-byte author prefix (which is not a separately verified identity).
No received text is executed or applied as a name or setting.

Only this contact's DMs and this process's sent-message ACKs are logged.
Other contacts, channel messages and unrelated pushes are discarded without
logging their contents. Native queue pulls advance the client's receive cursor;
on a single-consumer companion they consume queued messages, including those
filtered out. The on-chip multi-client companion gives each connection its
own journal cursor.

The initial queue and each reconnect's queue are drained with `backlog: true`;
these messages are **not evidence of a fresh reply**. Later messages use
`backlog: false`. Identical sender/timestamp/type/text messages are suppressed
within a 2,048-entry in-memory window, including across reconnects. A process
restart begins a new window, so consumers should also retain `message_id`.

Message-waiting pushes trigger actual queue pulls. A blocking receive loop and
a local clock query/queue pull every 30 seconds (`--heartbeat`) provide liveness
without busy-waiting or RF probes. Fragmented TCP frames retain their bytes
across idle deadlines. Connection loss is logged and reconnects with a
2..30-second backoff **for receiving only**: the advert, interrupted message
and any unsent remainder are never automatically replayed. Reconnecting
re-verifies the sender and contact. Protocol/identity errors stop the process
rather than silently accepting a different radio or malformed stream.

The implementation shares TCP framing with the hardware tools through
`tools/companion.py`. Queue layouts follow the native companion and existing
`internal/companion/mesh.go` implementation. All CLI commands and tests run
through this directory's Makefile; temporary files belong in repository `.tmp`.
