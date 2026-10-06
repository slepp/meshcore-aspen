# Download an encrypted node backup

Aspen generates a compressed backup of its identities, settings, programs and
saved data. Birch exports the host's configuration, configured credentials and
role state, not the shared modem's state. Pine exports InternalFS identities and
configuration together with its QSPI Lua files and metadata.

| Node | Available in | Download through |
| --- | --- | --- |
| Aspen | Aspen 0.1.2; current `public_aspen`, beta and HTTPS source builds | Authenticated Management RF or the node's WiFi administration page |
| Birch | Current Go host source | Authenticated repeater/room RF administration or the enabled host administration page |
| Pine | Current `nrfmast_fleet_lua` and `nrfmast_solar_lua` source builds | Authenticated repeater RF administration |

Older website images do not gain these commands without an application update.
The existing scoped `data` export/restore commands remain separate: they transfer
one bot scope, not a whole node. See the platform's update guide before changing
firmware; do not erase a filesystem or identity to add backups.

## Choose your recipient key

Backups are encrypted to an operator's **Ed25519 public key**. You must retain
its matching **32-byte seed** on your workstation to inspect or extract them.
Use the same private operator seed you already maintain, or generate a dedicated
backup key:

```sh
umask 077
mkdir -p "$HOME/.config/meshcore-private"
python3 tools/hardware/rf.py init-key \
  --key-file "$HOME/.config/meshcore-private/backup.seed"
```

The printed 64-hex-digit public key is safe to give the node. Never paste the
seed into a command, browser input or setup profile. A backup recipient does
not receive administration permissions; authenticate separately using the
node's existing owner key or administrator password.

**These exports include private identities and credentials.** Keep both the
encrypted file and operator seed private and in separate retained storage.
Extracted records are plaintext secrets. Encryption of the file does not
protect a plaintext HTTP login: use a trusted LAN, localhost or a protected TLS
tunnel for web administration.

## Download over WiFi

In Aspen's `/admin` page or Birch's enabled host `/admin` page, enter the
recipient public key under **Node backup**, then choose **Create and download
node backup**. The page waits for preparation and checks the downloaded file's
size and SHA-256. **Download saved node backup** retrieves the previous snapshot
without creating another one.

For a private file-based workflow, put the existing admin password in a
0600 file, then run:

```sh
python3 tools/node_backup.py download "$HOME/node.mcb" \
  --url http://NODE_IP \
  --key-file "$HOME/.config/meshcore-private/backup.seed" \
  --password-file "$HOME/.config/meshcore-private/admin.password"
```

For Birch, use the host status URL, normally `http://127.0.0.1:9080`, and its
configured administrator password or separate HTTP token. Enable the
[host administration page](HOST_GUIDE.md#host-browser-administration) first.
Backup controls work without a native bot; they export all retained host role
state under its configured state directory.

Expected: `Saved encrypted node backup: ...` with a byte and record count.
The destination is created privately and never overwritten. Add `--saved`
to download the existing snapshot to a new filename.

## Download through a companion radio

Connect the workstation to a companion radio on the node's LoRa profile. Use
Aspen's Management public key, Pine's repeater public key or a running Birch
repeater/room public key as `TARGET_PUBLIC_KEY`. This is the addressed role's
full key, not the shared modem's key or the backup recipient's key.

```sh
python3 tools/node_backup.py download "$HOME/node-rf.mcb" \
  --gateway COMPANION_IP --port 8001 \
  --target TARGET_PUBLIC_KEY \
  --key-file "$HOME/.config/meshcore-private/backup.seed" \
  --password-file "$HOME/.config/meshcore-private/admin.password"
```

If that seed is already an authorized owner, omit `--password-file` for
trusted-key login. A dedicated recipient seed instead needs the existing
administrator password. Reception and replies use encrypted native admin DMs.
The backup's own encryption remains the same as the WiFi download.

Current source builds use unpadded Base64 replies sized to fill the native
162-byte admin text limit, including the request tag, backup ID and byte offset.
With the downloader's 16-digit request tag, each reply usually carries 84--88
archive bytes. Budget about 10 minutes for 10 KiB, plus radio transmission/reply
time. Reads still wait at least five seconds between chunks.
Aspen 0.1.2 and other older backup builds use 48-byte hex replies; the downloader
detects that command version and falls back, taking at least 18 minutes per 10 KiB.
Keep the workstation and companion connected; large contact inventories can
take over an hour. Other roles continue sharing the radio. This is a paced
pull transfer, not an unsolicited flood.

An interrupted download keeps private `.part` and `.part.json` files beside
the requested destination. Run the same command with **`--resume`** to continue.
It loads the saved snapshot, checks the ID, size, checksum and recipient, and
requests the next byte offset. It never starts a replacement backup. WiFi
resume verifies the saved prefix against a fresh stream.

The node retains its encrypted snapshot across a restart. `backup start`
replaces it only after the new file is written and published; a failed export
leaves the previous published snapshot available through `backup load`.
If the saved ID has changed, use a new destination; do not join different
snapshots. If the initial request's reply was lost before local metadata was
saved, inspect `backup status` and use **`--saved`** rather than blindly
requesting another snapshot.

## Inspect or extract

The tool checks the complete file's authenticated encryption before exposing
records. It rejects a different recipient seed, truncation, changed bytes and
unsafe archive paths.

```sh
python3 tools/node_backup.py inspect "$HOME/node.mcb" \
  --key-file "$HOME/.config/meshcore-private/backup.seed"
python3 tools/node_backup.py extract "$HOME/node.mcb" \
  --key-file "$HOME/.config/meshcore-private/backup.seed" \
  --directory "$HOME/private-node-records"
```

Inspection prints the manifest and inventory counts, not secret values.
Extraction requires a new directory, creates it with mode 0700 and writes
records with mode 0600. It never overwrites existing files. Use the extracted
records with the platform's identity/configuration recovery procedures; this
workflow does not provide a whole-node network restore command.

## Commands, limits and archive layout

Authenticated administration accepts `backup help`, `backup start KEY64`,
`backup status`, `backup load`, `backup read64 ID16 OFFSET`,
`backup read ID16 OFFSET`, `backup cancel` and
`backup clear`. `clear` removes saved backup files, not the node's settings.
Lua jobs cannot request whole-node exports. WiFi downloads use
`GET /admin/backup?id=ID16` with the existing authenticated session.

`READY ID16 bytes=N sha=SHA256` describes the encrypted file. ID16 is the
first 16 lowercase hex digits of its SHA-256. RF replies are
`CHUNK64 ID16 OFFSET BASE64`, `WAIT ms=N` or `EOF`. Base64 is the standard
alphabet without `=` padding. Its decoded chunk size is
`floor((162 - tag_bytes - header_bytes) * 3 / 4)`, capped by the bytes remaining
in the file; the header includes its trailing space. Untagged reads can carry
up to 101 archive bytes. Advance by the actual decoded length, not a fixed
chunk size. Legacy `read` remains `CHUNK ID16 OFFSET HEX` with at most 48 bytes
for older clients. A `WAIT` is a pacing response,
not a failed read. Preparation and downloads cannot replace the same file
while an active download holds it.

Format v1 permits at most 512 records and a 2 MiB decoded tar archive. Embedded
exports also need filesystem room for the previous and new encrypted snapshots
plus an 8192-byte free-space reserve. Export fails explicitly when storage,
inventory, readback or consistency checks fail; pause configuration/data changes
and request a new snapshot when the node reports concurrent changes.
Logs, sockets, transfer staging and saved backup files are not archive records.

The file header is `MCB 01 01 00 00 00`, followed by the ephemeral Ed25519 public
key (32 bytes), recipient public key (32 bytes) and AES-CTR nonce (16 bytes).
The remaining payload is RLE-compressed USTAR encrypted with AES-128-CTR,
followed by a 32-byte HMAC-SHA256 over the header and ciphertext. Key derivation
uses the MeshCore Ed25519 shared secret and distinct
`meshcore-backup-v1 encryption` / `meshcore-backup-v1 authentication` domains.
The RLE control byte encodes 1..128 literal bytes when bit 7 is clear, or
3..128 repetitions of the next byte when set.

`manifest.json` records the product, firmware and format version. `files/`
holds the platform's retained filesystem records; ESP NVS blobs are under
`nvs/NAMESPACE/KEY.blob`; Pine InternalFS records are under `config/internal/`;
Birch includes `config/host.json` and `config/environment.json`. Identity
representations retain the platform's original seed/scalar/envelope formats.
Do not interpret every identity record as the operator's 32-byte seed.
