// SPDX-License-Identifier: Apache-2.0
#include "OpaqueFrontend.h"
#include <stdio.h>

namespace cloudroom {
static size_t encode(const uint8_t *bytes, size_t size, char *out) {
  const char *digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t at = 0;
  for (size_t i = 0; i < size; i += 3) {
    const unsigned n = unsigned(size-i < 3 ? size-i : 3);
    const uint32_t b = (uint32_t(bytes[i]) << 16) | (n>1 ? uint32_t(bytes[i+1]) << 8 : 0) | (n>2 ? bytes[i+2] : 0);
    out[at++] = digits[b>>18]; out[at++] = digits[(b>>12)&63];
    out[at++] = n>1 ? digits[(b>>6)&63] : '='; out[at++] = n>2 ? digits[b&63] : '=';
  }
  out[at] = 0; return at;
}
bool OpaqueFrontend::enqueue(unsigned alias, const char *frame, size_t size) {
  if (alias >= count_ || alias >= AliasLimit || !states_[alias].ready || size >= 512) return false;
  auto &s = states_[alias]; if (s.size == QueueDepth) return false;
  auto &slot = buffers_.operations[alias][(s.head+s.size)%QueueDepth];
  memcpy(slot.bytes, frame, size); slot.size = uint16_t(size); ++s.size; return true;
}
void OpaqueFrontend::opened(unsigned alias, uint32_t generation) {
  disconnected(alias); if (alias < count_ && alias < AliasLimit) states_[alias].generation = generation;
}
void OpaqueFrontend::disconnected(unsigned alias) {
  if (alias >= AliasLimit) return;
  states_[alias] = {};
  for (auto &f : flights_) if (f.cookie && f.alias == alias) f = {};
}
void OpaqueFrontend::received(const Reception &rx) {
  if (!rx.size || rx.size > RadioLimit) return;
  char encoded[341], frame[512]; encode(rx.bytes, rx.size, encoded);
  const int n = snprintf(frame, sizeof(frame), "{\"id\":\"%lu\",\"operation\":{\"op\":\"rf\",\"packet\":\"%s\"}}", (unsigned long)++request_, encoded);
  if (n <= 0 || size_t(n) >= sizeof(frame)) return;
  for (unsigned a = 0; a < count_ && a < AliasLimit; ++a) enqueue(a, frame, size_t(n));
}
bool OpaqueFrontend::advertise(unsigned alias) {
  char frame[96];
  const int n=snprintf(frame,sizeof(frame),"{\"id\":\"%lu\",\"operation\":{\"op\":\"advertise\"}}",(unsigned long)++request_);
  return n>0 && size_t(n)<sizeof(frame) && enqueue(alias,frame,size_t(n));
}
size_t OpaqueFrontend::receiptFrame(const char *id, Receipt::Outcome result, char *frame, size_t capacity) {
  const char *name = result == Receipt::Sent ? "sent" : result == Receipt::Failed ? "failed" : "unknown";
  const int n = snprintf(frame, capacity, "{\"id\":\"%lu\",\"operation\":{\"op\":\"txReceipt\",\"dispatchId\":\"%s\",\"outcome\":\"%s\"}}", (unsigned long)++request_, id, name);
  return n>0 && size_t(n)<capacity ? size_t(n) : capacity+1;
}
bool OpaqueFrontend::frame(unsigned alias, const char *frame, size_t size) {
  if (alias >= count_ || alias >= AliasLimit) return false;
  Event e; if (!e.parse(frame,size)) return false;
  auto &state = states_[alias];
  if (e.type == Event::Ready) {
    uint64_t version = 0; char name[32], key[65]; size_t written = 0;
    const auto pub = Document::field(e.body, "publicKey");
    if (state.ready || !e.alias.equals(aliases_[alias].id) ||
        !Document::field(e.body,"version").unsignedNumber(version,2) || version != 2 ||
        !decodeString(Document::field(e.body,"name"),name,sizeof(name),written) || strcmp(name,aliases_[alias].name)) return false;
    const char *hex = "0123456789abcdef";
    for (unsigned i=0;i<32;++i) {key[i*2]=hex[aliases_[alias].publicKey[i]>>4];key[i*2+1]=hex[aliases_[alias].publicKey[i]&15];} key[64]=0;
    if (!pub.equals(key)) return false;
    state.ready = true; return true;
  }
  if (!state.ready) return false;
  if (e.type == Event::Result || e.type == Event::Error) return true;
  if (e.type != Event::Transmit || !e.alias.equals(aliases_[alias].id)) return false;
  char id[37]; memcpy(id,e.dispatchId.data,36); id[36]=0;
  for (const auto &f : flights_) if (f.cookie && f.alias == alias && !strcmp(f.id,id)) return true;
  Transmission tx; size_t n = 0;
  if (!decodeBytes(e.packet,tx.bytes,sizeof(tx.bytes),n) || !n) return false;
  Flight *slot = nullptr; for (auto &f : flights_) if (!f.cookie) {slot=&f;break;}
  if (!slot) {
    if (state.failedId[0]) return false;
    memcpy(state.failedId,id,sizeof(id));return true;
  }
  tx.size=uint16_t(n);tx.alias=uint8_t(alias);tx.generation=state.generation;tx.delayMs=e.delayMs;tx.priority=e.priority;
  if (!++cookie_) ++cookie_;
  tx.cookie=cookie_;
  if (!radio_.submit(tx)) {
    if (state.failedId[0]) return false;
    memcpy(state.failedId,id,sizeof(id));return true;
  }
  slot->cookie=tx.cookie;slot->generation=tx.generation;slot->alias=tx.alias;memcpy(slot->id,id,sizeof(id));
  return true;
}
void OpaqueFrontend::receipt(const Receipt &r) {
  for (auto &f : flights_) if (f.cookie == r.cookie && f.alias == r.alias && f.generation == r.generation) {
    if (!f.terminal) { f.terminal=true;f.outcome=r.outcome; } return;
  }
}
size_t OpaqueFrontend::operation(unsigned alias, char *out, size_t capacity) {
  if (alias >= count_ || alias >= AliasLimit) return 0;
  auto &s=states_[alias]; if (!s.ready) return 0;
  // Final receipts have priority over best-effort RX. Keep their bindings
  // until serialized, even when the RX operation queue is full.
  if (s.failedId[0]) {
    const size_t n=receiptFrame(s.failedId,Receipt::Failed,out,capacity);
    if (n<=capacity) s.failedId[0]=0;
    return n;
  }
  for (auto &f:flights_) if (f.cookie && f.alias==alias && f.terminal) {
    const size_t n=receiptFrame(f.id,f.outcome,out,capacity);
    if (n<=capacity) f={};
    return n;
  }
  if (!s.size) return 0;
  auto &slot=buffers_.operations[alias][s.head];if(slot.size>capacity)return capacity+1;
  memcpy(out,slot.bytes,slot.size);s.head=(s.head+1)%QueueDepth;--s.size;return slot.size;
}
} // namespace cloudroom
