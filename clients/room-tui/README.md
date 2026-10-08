# Aspen Rooms in the terminal

Join the same IP/radio conversation as the web interface from a Linux terminal.
This client uses Bubble Tea v2, Bubbles and Lip Gloss, in its own Go module; it
does not run room services or require a local radio.

With Go 1.26.7 or newer:

```sh
cd clients/room-tui
go build -o room-tui .
./room-tui --service https://YOUR_ROOM_WORKER
```

Select a channel with the arrow keys, press Enter, enter your operator-created
username and account password, then press Enter to join. A service without
`WEB_USERS` instead asks for a display name and that channel's shared password.
An account needs a grant for each channel you join. The terminal masks password
entry and does not save it. Use the Worker's HTTPS origin, without a path;
HTTP is accepted only for loopback development.

Joined channels remain connected while you switch, with unread counts for
other channels. Messages show the author's name and key fingerprint. Radio names
come from signed adverts verified by the Worker; repeated names remain separate
full-key identities. A saved post confirms shared-history storage, not RF
reception. [Account setup and the shared API](../../services/shared-room/WEB.md)
also apply to this client.

| Key | Action |
| --- | --- |
| Tab / Shift+Tab | Focus channels, history or input |
| Up / Down, Enter in channels | Select a channel and focus its input |
| Ctrl+N / Ctrl+P | Next / previous channel |
| Enter in composer | Add a line break |
| Ctrl+S | Send, within 151 UTF-8 bytes including `Name: ` |
| Ctrl+R | Explicitly check/retry an uncertain post, or reconnect |
| Ctrl+O | Load older history |
| Ctrl+L | Leave the selected room; retain unsent work |
| Ctrl+Q | Save and quit |

The layout uses a sidebar at 80 columns, collapses it below that, and requires
at least 40 columns by 18 rows. It follows the terminal's reported background;
an unreported background defaults to dark. `NO_COLOR=1` disables color styles.
Radio text and names are stripped of terminal control sequences for rendering.

## Device state and reconnecting

The first run creates a local Ed25519 keypair. Account login signs the Worker's
single-use challenge; the public key is this desktop's message author. Another
terminal state directory or browser profile is another device, even for the
same account. This key is not moved to radios or advertised over RF.

State defaults to `$XDG_CONFIG_HOME/aspen-rooms/<service-hash>/state.json`
(or `$HOME/.config/...`). It holds the private seed, scoped login cookies,
username, selected channel, drafts and uncertain posts. Keep this private
directory: removing it loses that identity and unsent work. Files are `0600`,
the leaf directory is `0700`, and a lock prevents two clients sharing it at
once. The client refuses public files or symlinks at the state leaf/files.
To use another account or device:

```sh
./room-tui --service https://YOUR_ROOM_WORKER --state-dir "$HOME/.config/aspen-other-device"
```

Drafts save as you type. Before sending, the client saves a UUID, author and
text. A lost response retains this outbox entry through quit and restart;
**Ctrl+R** explicitly sends that same ID, so an already stored post is returned
without another copy. There is no automatic post retry. Rejected content keeps
the draft for editing; an expired login requires signing in again before
checking the pending post. A different author cannot resend it.

The client catches up history and reconnects its room sockets automatically.
Cookies retain their original expiry after restart. A changed account password,
name, grant or room configuration ends those sessions; log in again with the
same device state. If saving state fails, resolve the displayed file error
before sending. A second Ctrl+Q after a quit-save warning exits with an error
and does not claim the latest changes were saved.

## Local checks

```sh
go test -race ./...
# Requires the existing services/shared-room npm dependencies and Python 3.
node test/worker.mjs
```

The second command starts an isolated local Worker with public fixture keys,
checks Go signatures against its native verifier, drops a committed post's
response to check same-ID recovery, and drives login, send, resize, quit and
restart through a PTY. It also waits for a real Worker alarm to retry native
history after a missing ACK, without another client request. It uses no
production credentials and sends nothing
over RF. It removes its local Worker state and test binary on completion.
