# Security

## Reporting a problem

Email **[slepp@slepp.ca](mailto:slepp@slepp.ca)** for private security reports.
You can also use GitHub's private vulnerability reporting when it is enabled
for this repository. Keep vulnerability details out of public issues.

Include the affected version/profile, setup, reproduction steps and observed
impact. Redact passwords, WiFi credentials, private keys, signing seeds,
private messages and caller backups. Do not test against someone else's
radio, broker or host service.

Fixes target the latest maintained source and release. Reports about older
versions are welcome, but this hobby project does not provide a response-time
or long-term-support commitment.

## Deployment boundaries

Companion TCP, KISS, the ESP32 dashboard and browser administration belong on
a trusted LAN. ESP32 browser administration uses plaintext HTTP. Do not
expose these ports directly to the Internet. Use encrypted authenticated RF
or a protected local owner socket for the secret imports that those transports
support.

The hosted shared-room service (`aspen.ve6slp.ca`) is in scope for security
reports, including code redemption, key custody and quota bypass. A Worker you
deploy yourself is yours to operate; report flaws in the shared Worker source
here. The service operator can read and send as every room it hosts
([room-key custody](services/shared-room/TRUST.md#hosted-rooms)). Do not probe
the hosted service beyond a single `cloudroom probe` and your own rooms.

Configure independent administrator credentials and owner keys before
enabling roles. Preserve identity storage during updates. Treat full-flash
backups, private setup files and scoped caller exports as secrets.

Use the matching update guide for your board:
[ESP32](firmware/esp32/ESP_FIELD_UPDATES.md) or
[nRF52840](firmware/nrf52840/BLE-FIELD-UPDATE.md). Pine's BLE bootloader has no
application rollback, and an interrupted update can require USB recovery;
keep physical recovery available.
