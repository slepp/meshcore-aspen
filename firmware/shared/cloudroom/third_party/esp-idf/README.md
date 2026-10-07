# ESP-IDF transport foundation header

`esp_transport_internal.h` is unchanged from ESP-IDF v4.4.7, the SDK in the
pinned Arduino 2.0.17 / PlatformIO espressif32 6.11.0 native profile.

Source: https://github.com/espressif/esp-idf/blob/v4.4.7/components/tcp_transport/esp_transport_internal.h

SHA-256: `cb365a5d7ad3da42053363cf5cfa35c235724ded091912b8634c5dfda147d199`.
License: Apache-2.0, retained in the header.

The SDK requires a foundation/error container on an adapter parent before its
WebSocket transport can initialize. The public adapter API does not expose that
field; this header lets `CloudRoomSocket` allocate/release the SDK's container
while reusing Aspen's verified TLS transport. Framing and the container's
implementation remain the unmodified SDK library. A compile guard requires this
SDK version; recheck the internal layout when upgrading it.
