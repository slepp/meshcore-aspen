// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "third_party/coreJSON/core_json.h"

namespace cloudroom {
constexpr size_t FrameLimit = 4096;
constexpr size_t RadioLimit = 255;
constexpr const char *Subprotocol = "aspen-room.v1.json";
// Views remain valid only while the caller owns the input frame. No DOM,
// retained history, malloc or whole-frame copy is needed by this parser.
struct View {
  const char *data = nullptr;
  size_t size = 0;
  JSONTypes_t type = JSONInvalid;
  bool equals(const char *literal) const;
  bool unsignedNumber(uint64_t &out, uint64_t limit = UINT32_MAX) const;
  bool hex(size_t bytes) const;
};
class Document {
  View root_;
public:
  bool parse(const char *frame, size_t size);
  View root() const { return root_; }
  static View field(View object, const char *key);
};
struct Event {
  enum Type { Invalid, Ready, Result, Error, Delivery } type = Invalid;
  View id, alias, client, deliveryId, route, body;
  bool parse(const char *frame, size_t size);
};
// Decode base64 directly into a caller-owned bounded destination, preserving
// every possible byte. Strings containing JSON escapes are rejected here:
// production base64 is canonical plain ASCII (including '/' and '+').
bool decodeBytes(View base64, uint8_t *out, size_t capacity, size_t &written);
// Decodes a validated JSON string into its bounded final destination. This is
// the only copy for native post text; Unicode escapes, including pairs, work.
bool decodeString(View string, char *out, size_t capacity, size_t &written);
} // namespace cloudroom
