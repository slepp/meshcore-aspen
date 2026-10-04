// SPDX-License-Identifier: Apache-2.0
#include "ObserverWire.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

// Provided once by upstream BaseChatMesh.cpp (densaugeo/base64).
unsigned int encode_base64_length(unsigned int);
unsigned int encode_base64(const unsigned char[], unsigned int, unsigned char[]);

namespace onchip::observerWire {
namespace {
bool quoted(const char *input, char *output, size_t capacity) {
  size_t at = 0;
  if (!input || capacity < 3) return false;
  output[at++] = '"';
  for (auto p = reinterpret_cast<const unsigned char *>(input); *p; ++p) {
    if (at + 7 >= capacity) return false;
    if (*p == '"' || *p == '\\') {
      output[at++] = '\\';
      output[at++] = *p;
    } else if (*p < 32) {
      at += snprintf(output + at, capacity - at, "\\u%04x", *p);
    } else output[at++] = *p;
  }
  output[at++] = '"';
  output[at] = 0;
  return true;
}
void hex(const uint8_t *data, size_t length, char *output) {
  for (size_t i = 0; i < length; ++i)
    snprintf(output + i * 2, 3, "%02X", data[i]);
  output[length * 2] = 0;
}
bool utc(uint32_t epoch, char *timestamp, char *clock, char *date) {
  if (epoch < 1735689600u) return false;
  time_t seconds = epoch;
  tm value{};
  return gmtime_r(&seconds, &value) &&
         strftime(timestamp, 40, "%Y-%m-%dT%H:%M:%S.000000+00:00", &value) &&
         strftime(clock, 9, "%H:%M:%S", &value) &&
         strftime(date, 11, "%d/%m/%Y", &value);
}
size_t complete(int length, size_t capacity) {
  return length > 0 && size_t(length) < capacity ? size_t(length) : 0;
}
size_t base64url(const char *input, char *output, size_t capacity) {
  const size_t length = strlen(input);
  if (encode_base64_length(length) + 1 > capacity) return 0;
  size_t n = encode_base64(reinterpret_cast<const unsigned char *>(input),
                          length, reinterpret_cast<unsigned char *>(output));
  for (size_t i = 0; i < n; ++i) {
    if (output[i] == '+') output[i] = '-';
    if (output[i] == '/') output[i] = '_';
  }
  while (n && output[n - 1] == '=') --n;
  output[n] = 0;
  return n;
}
} // namespace

bool decode(const uint8_t *raw, size_t length, mesh::Packet &packet) {
  // Packet::readFrom assumes its caller has checked all wire offsets.
  if (!raw || length < 3 || length > 255 || raw[0] >> 6) return false;
  size_t offset = (raw[0] & 3) == 0 || (raw[0] & 3) == 3 ? 5 : 1;
  if (offset >= length) return false;
  const uint8_t packed = raw[offset++];
  if (!mesh::Packet::isValidPathLen(packed)) return false;
  offset += (packed & 63) * ((packed >> 6) + 1);
  if (offset >= length || length - offset > MAX_PACKET_PAYLOAD) return false;
  return packet.readFrom(raw, length);
}
bool token(const mesh::LocalIdentity &identity, const char *audience,
           uint32_t epoch, char *output, size_t capacity, uint32_t lifetime) {
  if (!audience || !*audience || epoch < 1735689600u || !lifetime ||
      lifetime > UINT32_MAX - epoch) return false;
  char key[65], aud[256], payload[512], encoded[704];
  hex(identity.pub_key, 32, key);
  if (!quoted(audience, aud, sizeof(aud))) return false;
  const int n = snprintf(payload, sizeof(payload),
      "{\"publicKey\":\"%s\",\"aud\":%s,\"iat\":%u,\"exp\":%u}",
      key, aud, epoch, epoch + lifetime);
  if (!complete(n, sizeof(payload)) ||
      !base64url(payload, encoded, sizeof(encoded))) return false;
  // Same JWT framing as upstream JWTHelper, using LocalIdentity::sign directly:
  // the MeshCore key is scalar||prefix, not a seed or a libsodium secret key.
  char input[768], signatureHex[129];
  const int size = snprintf(input, sizeof(input),
      "eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0.%s", encoded);
  if (!complete(size, sizeof(input))) return false;
  uint8_t signature[64];
  identity.sign(signature, reinterpret_cast<const uint8_t *>(input), size);
  hex(signature, sizeof(signature), signatureHex);
  return complete(snprintf(output, capacity, "%s.%s", input, signatureHex),
                  capacity) != 0;
}
size_t packet(const uint8_t *raw, size_t length, const char *origin,
              const char *key, uint32_t epoch, float rssi, float snr,
              uint16_t filter, char *output, size_t capacity, bool captureFormat) {
  mesh::Packet decoded;
  char timestamp[40], clock[9], date[11], name[200], rawHex[511], hash[17];
  if (!decode(raw, length, decoded) ||
      !(filter & (uint16_t(1) << decoded.getPayloadType())) ||
      !std::isfinite(rssi) || !std::isfinite(snr) || rssi < -128 || rssi > 127 ||
      (rssi == 127 && snr == -32) ||
      !utc(epoch, timestamp, clock, date) || !quoted(origin, name, sizeof(name)))
    return 0;
  hex(raw, length, rawHex);
  uint8_t digest[MAX_HASH_SIZE];
  decoded.calculatePacketHash(digest);
  hex(digest, sizeof(digest), hash);
  const char *route = captureFormat && decoded.getRouteType() == 3 ? "T" :
                      decoded.isRouteFlood() ? "F" : "D";
  char signal[64];
  const int signalSize = snprintf(signal, sizeof(signal),
                                 captureFormat ? "%g" : "%.1f", snr);
  if (!complete(signalSize, sizeof(signal))) return 0;
  if (captureFormat && !strpbrk(signal, ".eE")) {
    if (size_t(signalSize) + 3 > sizeof(signal)) return 0;
    strcat(signal, ".0");
  }
  char path[512]{};
  size_t at = 0;
  const unsigned hops = captureFormat && strcmp(route, "D") ?
                        0 : decoded.getPathHashCount();
  for (unsigned i = 0; i < hops; ++i) {
    char hop[7];
    hex(decoded.path + i * decoded.getPathHashSize(),
        decoded.getPathHashSize(), hop);
    // The firmware's path array is lowercase, unlike raw/hash/origin_id.
    for (char *p = hop; *p; ++p) if (*p >= 'A' && *p <= 'F') *p += 'a' - 'A';
    at += snprintf(path + at, sizeof(path) - at, captureFormat ?
                   "%s%s" : "%s\"%s\"", i ? "," : "", hop);
  }
  const bool includePath = captureFormat ? !strcmp(route, "D") : at > 0;
  return complete(snprintf(output, capacity,
      "{\"origin\":%s,\"origin_id\":\"%s\",\"timestamp\":\"%s\","
      "\"type\":\"PACKET\",\"direction\":\"rx\",\"time\":\"%s\","
      "\"date\":\"%s\",\"len\":\"%u\",\"packet_type\":\"%u\","
      "\"route\":\"%s\",\"payload_len\":\"%u\",\"raw\":\"%s\","
      "\"SNR\":\"%s\",\"RSSI\":\"%d\",\"hash\":\"%s\"%s%s%s}",
      name, key, timestamp, clock, date, unsigned(length),
      decoded.getPayloadType(), route, decoded.payload_len, rawHex,
      signal, int(std::lround(rssi)), hash,
      includePath ? (captureFormat ? ",\"path\":\"" : ",\"path\":[") : "",
      includePath ? path : "",
      includePath ? (captureFormat ? "\"" : "]") : ""), capacity);
}
size_t status(const char *state, const char *origin, const char *key,
              const char *model, const char *version, const char *radio,
              uint32_t epoch, char *output, size_t capacity) {
  char timestamp[40], clock[9], date[11], name[200], hardware[200],
       firmware[200], phy[200];
  if (!utc(epoch, timestamp, clock, date) || !quoted(origin, name, sizeof(name)) ||
      !quoted(model, hardware, sizeof(hardware)) ||
      !quoted(version, firmware, sizeof(firmware)) ||
      !quoted(radio, phy, sizeof(phy))) return 0;
  return complete(snprintf(output, capacity,
      "{\"status\":\"%s\",\"origin\":%s,\"origin_id\":\"%s\","
      "\"timestamp\":\"%s\",\"model\":%s,\"firmware_version\":%s,"
      "\"radio\":%s,\"client_version\":\"meshcore-kiss/observer-v1\"}",
      state, name, key, timestamp, hardware, firmware, phy), capacity);
}
} // namespace onchip::observerWire
