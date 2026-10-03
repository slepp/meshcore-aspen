/*
Package observer publishes read-only observations without a MeshCore node or
any RF transmit interface. Attach Observe to a KISS modem's data handler.

# MQTT schema

The default empty/internal-v1 Format preserves the event contract below.
observer-v1/capture-v1 select the corresponding upstream public packet dialect
and status JSON with an uppercase key
and IATA topic component; only finite-signal RF receptions are published.
It excludes local reflection, malformed envelopes and unavailable UTC.
Config.Audience enables the v1_PUBLICKEY MQTT CONNECT identity exchange,
signed by Config.Sign using the existing MeshCore identity. Tokens use hex
Ed25519 signatures and renew through a fresh connection before expiry.
TLS/WSS verifies certificates; library callers may supply verified TLSConfig.
See firmware/esp32/OBSERVER.md for pinned wire contracts and configuration.

Packet messages use QoS 1, are not retained, and appear on
<topic_prefix>/<observer_public_key_hex>/packets. TopicPrefix defaults to
"meshcore". Each JSON object contains:

  - event_id: random per-observer-instance prefix and observation sequence;
    retries preserve this ID, so consumers can deduplicate QoS 1 delivery.
  - timestamp: UTC RFC 3339 observation time, not publication time.
  - observer_identity: the observer's 32-byte public identity in lowercase hex.
  - raw_packet_hex: the complete received frame in lowercase hex.
  - snr: real dB, or null when unavailable/non-finite.
  - rssi: dBm, or null when unavailable.
  - local_loopback: true for signal-tagged SNR -32/RSSI 127 local reflections.
    Both signal values are null for these frames. This is a firmware marker,
    not a cryptographic assertion of origin.
  - packet: present only when meshcore.PacketFromBytes succeeds; header,
    type, route, version and path_length are numeric wire fields; type_name
    and route_name are meshcore-go names; path_hex is lowercase hex;
    path_hash_size and path_hop_count describe the packed path_length.
  - decode_error: present instead of packet when envelope decoding fails.
    Raw bytes are still retained. Payloads are not decrypted, authenticated,
    or semantically validated; no private keys are accepted by this package.

<topic_prefix>/<observer_public_key_hex>/status contains retained plain text
"online" or "offline". A QoS 1 last will publishes "offline" after an ungraceful
disconnect; Close attempts to publish it before disconnecting. A broker outage
can prevent delivery of either status until reconnect.

# Bounds and lifecycle

Start returns without requiring a reachable MQTT broker. One worker retries
connections every second; an idle worker rechecks Paho's connection state every
five seconds.
The queue defaults to 256 frames (configurable 1..4096), with at most one
additional pending publication. Observe copies at most 4096 bytes per frame and
never waits for network I/O or queue capacity. Oversized frames, full-queue
arrivals, and calls outside the started lifecycle are dropped. It takes only a
short admission mutex; it never logs or decodes packets on the RF callback.
Stats reports counts, and the worker logs changing drop totals periodically
(including while reconnecting and on shutdown).
Connected reports Paho's open publisher connection after the broker acknowledges
the retained online publication. It is false before connecting, after detected
connection loss and during shutdown. This is a non-blocking status query, not
a network probe or a guarantee of subscriber delivery. Broker outages remain
recoverable; they do not create a permanent application fault.

Queued messages are memory-only. Close/cancellation stops admission and discards
queued and unacknowledged events, counting them as dropped. Connection and
publication timeouts are three seconds; graceful offline publication gets one
second. There is no restart after Close or context cancellation. Published
counts broker acknowledgements, not subscriber delivery; lost acknowledgements
can result in duplicates with the same event_id, or a shutdown drop count for
an event that the broker received. Stats snapshots during admission are not
transactional. The observer never subscribes or processes MQTT commands.

# Embedded broker

StartBroker runs an in-memory mochi TCP broker, defaulting to 127.0.0.1:1883.
Use 127.0.0.1:0 for an ephemeral port and Addr to retrieve it. Anonymous access
requires a literal loopback address. Non-loopback binds require both username
and password; configured credentials apply to every connection, including the
observer. Authenticated clients have full read/write topic access. Limits are
128 connected clients, 64 KiB packets, 128 inflight messages per client, and
256 pending client writes. Retained topics and subscriptions are in memory,
not a durable store or a resource quota for hostile authenticated clients.
TCP offers no encryption: use an external TLS MQTT broker on untrusted networks.
Stop observers before closing their broker so offline status can be delivered.
*/
package observer
