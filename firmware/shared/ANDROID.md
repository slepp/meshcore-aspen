# Android companion connections

Use the official **MeshCore** app by Liam Cottle with a companion radio or
the host companion service. Get it from the
[official Android listing](https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android)
linked by [MeshCore](https://meshcore.co.uk/), or the publisher's
[download directory](https://files.liamcottle.net/MeshCore/v1.50.0/).

## Connect without changing the radio

1. Choose **Connect → WiFi**. Enter the companion's IP address and port **5000**;
   port 8001 carries KISS, not the app protocol.
2. Enable **Auto Reconnect** and connect. The header should show the companion's
   saved name, contacts and channels.
3. On an already configured radio, choose **Skip Setup**. The setup wizard can
   write the name and LoRa settings; do not apply those changes while other
   roles or users share the radio.
4. Use the signal icon's **Advert • Zero Hop** action to broadcast the existing
   companion identity to nearby radios. This does not rename or retune it.
5. Open a bot contact's chat for commands, a repeater contact's **Manage**
   action for native login, or a room contact for room login. Contact
   **Telemetry** sends a native request, not a bot text command.

The app owns its local UI/history. The connected companion owns the sending
identity, native contacts and channels. Switching Base changes the RF sender
even when using the same Android installation. A remote contact needs the
sender's signed advert for encrypted chat or telemetry; cached names alone
do not establish reachability.

If a previously reachable chat contact stops responding, choose **Reset Path**
from its chat menu. This clears that contact's cached route and makes the next
send use flood routing, without changing identities or shared radio settings.
Try a supported read-only command such as `!ping`. Check both **Delivered**
and the reply; a returned route can change the displayed path to **Direct**.

Companion TCP is unauthenticated and belongs on a trusted LAN. Do not expose
it to the Internet. Use [mast administration](../esp32/MAST_ADMIN.md#administrator-workflow)
for shared PHY/role controls; a Base connection does not grant mast authority.

In an Android emulator, `10.0.2.2` reaches the emulator host's loopback services:

| Connection | App address |
| --- | --- |
| Standalone ESP32 companion | `MAST_IP:5000` |
| Go host Base on the emulator host | `10.0.2.2:5000` |
| Dedicated bot companion on the emulator host | `10.0.2.2:8106` when configured |

Use a dedicated AVD without wiping or copying another application's user data.
TCP needs no Bluetooth or USB passthrough. For the nRF bot's optional secured
BLE endpoint, follow [notes and BLE](../nrf52840/STATE-BLE.md). That endpoint shares
the bot's identity; it does not create another Base. The production Lua and
native-note images have different runtime capacities.

## Native access and expected denials

The shared modem does not merge credentials or ACLs. Management has its own
identity and requires its own native login. A repeater login does not grant
shared WiFi, PHY or role-mask control. Guest status does not grant administrator
settings. App-only firmware updates retain saved role preferences/passwords;
a compiled startup password may not be the active saved password.

An empty-password room login fails when `room access` reports password-protected.
Base telemetry can be unavailable when the contact has not granted it.
Bot `discovery` and each role's native telemetry permissions are independent.
`!ping` is a bot chat command, not native Ping or telemetry. Zero-voltage
telemetry on a board without battery measurement is an unsupported-value
sentinel, not a battery reading.

The remote-management screen offers paid unlock or a short wait followed by
**Continue**. Keep **Remember Password?** off on shared test devices. Do not
publish credential-bearing screenshots or UI dumps.

### Choose the endpoint for app settings

Open your **Relay → Manage** for repeater settings and your **Room** contact for
room administration. The ESP32 **Management** contact accepts native login,
status, telemetry, owner information and the
[mast command set](../esp32/MAST_ADMIN.md#administrator-workflow).
The app can display repeater panels for Management; use the Relay for
role-local settings such as repeat, delays and regions.

| Operation | Relay / Room endpoint | ESP32 Management endpoint |
| --- | --- | --- |
| Name and owner information | `get name`, `get owner.info`; setters change only that role | `get name`, `set name TEXT`, `get owner.info`, `set owner.info TEXT`; binary owner information uses Management's saved text |
| Radio/TX readback | `get radio`, `get freq`, `get tx` | Same reads; `status` also reports shared PHY |
| Repeat, delays, path hash and advert intervals | Relay's native settings; Room has its applicable settings | Mast commands, not app repeater presets |
| Regions, neighbours and discovery | Use Relay; neighbour requests are not Room operations | Native region/neighbour/ACL panels unsupported |
| Passwords and ACLs | Changes affect that role | Separate mast credentials/trust; `role password` supports protected Relay/Room administrator recovery |
| WiFi | Unsupported on ESP32 native Relay/Room and Go host roles | `get wifi.enabled/ssid/pwd/ip/status`; `set wifi.ssid TEXT`, `set wifi.pwd TEXT`, `set wifi.enabled 0/1`; secrets/setters require encrypted RF |
| Shared tuning, temporary radio and hardware | Shared-device native setters denied even when reads work | Mast PHY controls affect every active role |
| Clock and restart | Native clock; `reboot` restarts the selected role, not the device/host | Mast `reboot` restarts the device |
| Logs and statistics | ESP32 denies `log start` and `clear stats`; Go supports role packet logging | Native app log controls are not mast commands |

Start in Management's console with:

```text
status
wifi status
role name management
role config repeater
role help
```

Bare `wifi` returns usage; `help wifi` and `wifi help` show syntax.
`wifi status` omits credentials. App-compatible WiFi setters take literal text;
`wifi ssid TEXT` / `wifi password TEXT` also take literal text; append `hex`
before the value for explicitly encoded bytes. Save credentials, then
`wifi apply` to reconnect. `set wifi.enabled 0` disconnects after responding
and remains disabled after restart; encrypted Management RF stays available.
`set wifi.enabled 1` reconnects with saved credentials. The HTTP console
refuses password reads and WiFi setters. See [WiFi commands](../esp32/MAST_ADMIN.md#common-control-commands).

A Relay's `get owner.info` reply of `> ` means its public owner text is empty.
`set owner.info TEXT` changes only that role. Relay and Management implement
binary owner-info requests with their own names/text; Management's public
lookup works without login. Go Room supports CLI owner-info read, not the
binary request used by a repeater panel. Go host roles have no separate
Management RF identity; the private bot owner socket is bot-scoped.

Management's standard `get`/`set` aliases, binary owner information and
`help wifi`, plus host `get freq`, require a compatible profile-labelled image
or host binary. Older installed versions may reject them.

### Enable administrator login on host roles

Set `admin_password_env` in the private host configuration to the service
environment variable supplying the Relay/Room administrator password:

```json
"admin_password_env": "MESHCORE_ROLE_ADMIN_PASSWORD"
```

An empty reference supplies no administrator password. Room guest credentials
use separate `room_password` or `room_password_env`. Generate a strong
15-character administrator password and supply it through the service's
private environment. Keep files at **0600** and values out of arguments,
logs and source control. Setting an interactive shell variable does not
configure a running user service. In a maintenance window:

```sh
systemctl --user restart meshcore-host.service
```

Reconnect and check for **Repeater Admin** or **Room Admin**, not **Guest Tools**.
Refresh icons read settings; checkmark buttons apply them. Saved native
password overrides take precedence over the environment password. Do not
edit identity or role-state envelopes to replace them.

### Recover administrator access on an ESP32 role

An authorized Management RF connection can use `role password` to save/apply a
new active repeater/room administrator password. Follow the
[secret-file recovery command](../esp32/MAST_ADMIN.md#recovering-a-repeater-or-room-administrator-password);
web and bot programs cannot provision this credential. Management and room
guest passwords remain unchanged; existing ACL admissions are not revoked.
Reconnect the app and read administrator status/settings with the new credential.

Inspect the installed image's `role help` first. An unanswered login must not
block a corrected login to the same role. Older Go companions reject retries
while the first login is pending, for up to 120 seconds. Update the companion
with the same-peer login replacement fix instead of resetting passwords or ACLs.

## Reconnect after network loss

Enable Auto Reconnect. A changed IP may require updating the companion address.
After reconnect, inspect the Base name/public identity and retrieve available
messages before repeating a send. Bounded replay is not exactly-once delivery;
do not automatically retry an uncertain management mutation.
See [ESP32 recovery](../esp32/README.md#wifi-loss-and-recovery) for which sessions
retire while local RF roles continue.

For an AP/IP-loss check, use a maintenance window. Confirm uptime does not
restart, RF continues, identities/PHY remain unchanged, new network connections
work and the next enabled telemetry sample is published. This interrupts
network clients; it needs no source installation, identity change or reprovisioning.
