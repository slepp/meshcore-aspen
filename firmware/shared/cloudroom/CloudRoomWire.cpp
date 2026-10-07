// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomWire.h"
#include <string.h>

namespace cloudroom {
bool View::equals(const char *literal) const {
  return type == JSONString && strlen(literal) == size && !memcmp(data, literal, size);
}
bool View::unsignedNumber(uint64_t &out, uint64_t limit) const {
  if (type != JSONNumber || !size || (size > 1 && data[0] == '0')) return false;
  out = 0;
  for (size_t i = 0; i < size; ++i) {
    const unsigned digit = unsigned(data[i] - '0');
    if (digit > 9 || digit > limit || out > (limit - digit) / 10) return false;
    out = out * 10 + digit;
  }
  return true;
}
bool View::hex(size_t bytes) const {
  if (type != JSONString || size != bytes * 2) return false;
  for (size_t i = 0; i < size; ++i)
    if (!((data[i] >= '0' && data[i] <= '9') || (data[i] >= 'a' && data[i] <= 'f'))) return false;
  return true;
}
bool Document::parse(const char *frame, size_t size) {
  root_ = {};
  if (!frame || !size || size > FrameLimit || JSON_Validate(frame, size) != JSONSuccess) return false;
  size_t i = 0;
  while (i < size && (frame[i] == ' ' || frame[i] == '\r' || frame[i] == '\n' || frame[i] == '\t')) ++i;
  if (i == size || frame[i] != '{') return false;
  root_ = {frame, size, JSONObject};
  return true;
}
View Document::field(View object, const char *key) {
  View out;
  if (object.type != JSONObject || !key ||
      JSON_SearchConst(object.data, object.size, key, strlen(key), &out.data, &out.size, &out.type) != JSONSuccess) return {};
  return out;
}
bool Event::parse(const char *frame, size_t size) {
  *this = {};
  Document document;
  if (!document.parse(frame, size)) return false;
  const auto root = document.root();
  const auto kind = Document::field(root, "type");
  if (kind.equals("ready")) {
    uint64_t version = 0;
    if (!Document::field(root, "version").unsignedNumber(version, 1) || version != 1 ||
        !Document::field(root, "publicKey").hex(32)) return false;
    type = Ready; alias = Document::field(root, "alias"); body = root;
  } else if (kind.equals("result") || kind.equals("error")) {
    type = kind.equals("result") ? Result : Error;
    id = Document::field(root, "id");
    if (id.type != JSONString || !id.size || id.size > 32) return false;
    body = Document::field(root, type == Result ? "result" : "error");
    if (body.type != (type == Result ? JSONObject : JSONString)) return false;
  } else if (kind.equals("delivery")) {
    type = Delivery; alias = Document::field(root, "alias");
    client = Document::field(root, "client");
    deliveryId = Document::field(root, "deliveryId"); route = Document::field(root, "route");
    body = Document::field(root, "message");
    if (!client.hex(32) || deliveryId.type != JSONString || deliveryId.size != 36 ||
        route.type != JSONString || route.size > 340 || body.type != JSONObject) return false;
  } else return false;
  if ((type == Ready || type == Delivery) &&
      (alias.type != JSONString || !alias.size || alias.size > 64)) return false;
  return true;
}
static int base64Digit(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}
bool decodeBytes(View value, uint8_t *out, size_t capacity, size_t &written) {
  written = 0;
  if (value.type != JSONString || value.size % 4 || (!out && capacity)) return false;
  for (size_t i = 0; i < value.size; i += 4) {
    const char *p = value.data + i;
    const int a = base64Digit(p[0]), b = base64Digit(p[1]);
    const int c = p[2] == '=' ? 0 : base64Digit(p[2]);
    const int d = p[3] == '=' ? 0 : base64Digit(p[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    const size_t n = p[2] == '=' ? 1 : p[3] == '=' ? 2 : 3;
    if ((n != 3 && i + 4 != value.size) || (n == 1 && (p[3] != '=' || (b & 15))) ||
        (n == 2 && (c & 3)) || n > capacity - written) return false;
    const uint32_t bits = (uint32_t(a) << 18) | (uint32_t(b) << 12) | (uint32_t(c) << 6) | uint32_t(d);
    for (size_t j = 0; j < n; ++j) out[written++] = uint8_t(bits >> (16 - 8 * j));
  }
  return true;
}
static bool hex4(const char *data, size_t size, size_t &i, uint32_t &out) {
  if (size - i < 4) return false;
  out = 0;
  for (unsigned n = 0; n < 4; ++n) {
    const char c = data[i++];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    out = (out << 4) | unsigned(v);
  }
  return true;
}
bool decodeString(View value, char *out, size_t capacity, size_t &written) {
  written = 0;
  if (value.type != JSONString || !out || !capacity) return false;
  auto append = [&](uint8_t byte) { if (written >= capacity - 1) return false; out[written++] = char(byte); return true; };
  for (size_t i = 0; i < value.size;) {
    const char c = value.data[i++];
    if (c != '\\') { if (!append(uint8_t(c))) return false; continue; }
    if (i == value.size) return false;
    const char escape = value.data[i++];
    if (escape == 'u') {
      uint32_t code = 0;
      if (!hex4(value.data, value.size, i, code)) return false;
      if (code >= 0xd800 && code <= 0xdbff) {
        if (value.size - i < 6 || value.data[i++] != '\\' || value.data[i++] != 'u') return false;
        uint32_t low = 0;
        if (!hex4(value.data, value.size, i, low) || low < 0xdc00 || low > 0xdfff) return false;
        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
      } else if (code >= 0xdc00 && code <= 0xdfff) return false;
      if (code < 0x80) { if (!append(uint8_t(code))) return false; }
      else if (code < 0x800) { if (!append(0xc0 | (code >> 6)) || !append(0x80 | (code & 63))) return false; }
      else if (code < 0x10000) { if (!append(0xe0 | (code >> 12)) || !append(0x80 | ((code >> 6) & 63)) || !append(0x80 | (code & 63))) return false; }
      else if (!append(0xf0 | (code >> 18)) || !append(0x80 | ((code >> 12) & 63)) || !append(0x80 | ((code >> 6) & 63)) || !append(0x80 | (code & 63))) return false;
    } else {
      const char *codes = "\"\\/bfnrt", *decoded = "\"\\/\b\f\n\r\t";
      const char *match = strchr(codes, escape);
      if (!match || !append(uint8_t(decoded[match - codes]))) return false;
    }
  }
  out[written] = 0;
  return true;
}
} // namespace cloudroom
