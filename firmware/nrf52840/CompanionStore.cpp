// SPDX-License-Identifier: Apache-2.0
#include "CompanionStore.h"
#include <Utils.h>
#include <new>
#include <cstddef>
#include <algorithm>
#if NRFMAST_PRODUCTION_LUA
#include "platform/nvs.h"
#endif

namespace nrfmast {
namespace {
void encode(CompanionStore::Contact &out, const ContactInfo &in) {
  memcpy(out.key, in.id.pub_key, 32); memcpy(out.name, in.name, 32);
  memcpy(out.path, in.out_path, 64);
  out.type = in.type; out.flags = in.flags & 1; out.pathLength = in.out_path_len;
  out.advertTime = in.last_advert_timestamp; out.modified = in.lastmod;
  out.latitude = in.gps_lat; out.longitude = in.gps_lon; out.syncSince = in.sync_since;
}
ContactInfo decode(const CompanionStore::Contact &in) {
  ContactInfo out{};
  memcpy(out.id.pub_key, in.key, 32); memcpy(out.name, in.name, 32);
  memcpy(out.out_path, in.path, 64);
  out.type = in.type; out.flags = in.flags; out.out_path_len = in.pathLength;
  out.last_advert_timestamp = in.advertTime; out.lastmod = in.modified;
  out.gps_lat = in.latitude; out.gps_lon = in.longitude; out.sync_since = in.syncSince;
  return out;
}
void digest(const CompanionStore::Record &record, uint8_t out[32]) {
  mesh::Utils::sha256(out, 32, reinterpret_cast<const uint8_t *>(&record),
                      offsetof(CompanionStore::Record, digest));
}
}
std::unique_ptr<CompanionStore::Record> CompanionStore::allocateSnapshot() {
#ifdef NRF52_PLATFORM
  if (dbgHeapFree() < int(sizeof(Record) + 8192)) {
    error_ = "companion snapshot needs 2888 bytes above the 8KiB heap reserve";
    return {};
  }
#endif
  std::unique_ptr<Record> result(new (std::nothrow) Record{});
  if (!result) error_ = "companion snapshot allocation failed";
  return result;
}
bool CompanionStore::validate(const Record &record) const {
  uint8_t hash[32]; digest(record, hash);
  if (memcmp(record.magic, "PCS\1", 4) || memcmp(record.owner, bot_.self_id.pub_key, 32) ||
      record.count > Contacts || memcmp(hash, record.digest, 32)) return false;
  for (unsigned i = 0; i < record.count; ++i) {
    const auto &c = record.contacts[i];
    if (!memchr(c.name, 0, 32) || c.flags > 1 || c.reserved ||
        (c.pathLength != OUT_PATH_UNKNOWN && !mesh::Packet::isValidPathLen(c.pathLength)) ||
        !memcmp(c.key, record.owner, 32)) return false;
    for (unsigned j = 0; j < i; ++j)
      if (!memcmp(c.key, record.contacts[j].key, 32)) return false;
    if (c.advertLength > sizeof(c.advert)) return false;
    if (c.advertLength) {
      mesh::Packet packet{};
      if (!packet.readFrom(c.advert, c.advertLength) ||
          packet.getPayloadType() != PAYLOAD_TYPE_ADVERT || packet.payload_len < 100 ||
          memcmp(packet.payload, c.key, 32)) return false;
    }
  }
  for (const auto &channel : record.channels)
    if (!memchr(channel.name, 0, sizeof(channel.name))) return false;
  return true;
}
bool CompanionStore::begin(uint32_t now) {
  ready_ = dirty_ = false;
  state_ = allocateSnapshot();
  if (!state_) return false;
  memcpy(state_->owner, bot_.self_id.pub_key, 32);
#if NRFMAST_PRODUCTION_LUA
  nvs_handle_t handle = 0;
  auto status = nvs_open("pine-companion", NVS_READONLY, &handle);
  if (status == ESP_OK) {
    size_t size = sizeof(Record);
    status = nvs_get_blob(handle, "state", state_.get(), &size);
    nvs_close(handle);
    if (status == ESP_OK && (size != sizeof(Record) || !validate(*state_))) {
      error_ = "companion journal invalid; saved files retained"; return false;
    }
  }
  if (status != ESP_OK && status != ESP_ERR_NVS_NOT_FOUND) {
    error_ = "companion journal unreadable; saved files retained"; return false;
  }
  if (status == ESP_ERR_NVS_NOT_FOUND) {
    *state_ = Record{}; memcpy(state_->owner, bot_.self_id.pub_key, 32);
  }
  for (unsigned i = 0; i < state_->count; ++i)
    if (!bot_.addContact(decode(state_->contacts[i]))) {
      error_ = "companion contact restore failed"; return false;
    }
  for (unsigned i = 0; i < Channels; ++i) {
    ChannelDetails channel{};
    memcpy(channel.name, state_->channels[i].name, 32);
    memcpy(channel.channel.secret, state_->channels[i].secret, 16);
    if (!bot_.setChannel(i, channel)) { error_ = "companion channel capacity mismatch"; return false; }
  }
  refillAt_ = now; credits_ = 8; ready_ = true; error_ = "ok";
  return true;
#else
  error_ = "companion persistence requires production Lua QSPI storage";
  return false;
#endif
}
void CompanionStore::capture(Record &out) const {
  out = Record{}; memcpy(out.owner, bot_.self_id.pub_key, 32);
  memcpy(out.channels, state_->channels, sizeof(out.channels));
  auto iterator = bot_.startContactsIterator();
  ContactInfo contact{};
  while (out.count < Contacts && iterator.hasNext(&bot_, contact)) {
    auto &dest = out.contacts[out.count++];
    for (unsigned i = 0; i < state_->count; ++i)
      if (!memcmp(state_->contacts[i].key, contact.id.pub_key, 32)) {
        dest.advertLength = state_->contacts[i].advertLength;
        memcpy(dest.advert, state_->contacts[i].advert, sizeof(dest.advert)); break;
      }
    encode(dest, contact);
  }
}
bool CompanionStore::save(Record &record, uint32_t now) {
  if (!ready_) return false;
  const uint32_t refill = uint32_t(now - refillAt_) / RefillMs;
  if (refill) { credits_ = std::min(uint32_t(8), uint32_t(credits_) + refill); refillAt_ += refill * RefillMs; }
  digest(record, record.digest);
  if (!memcmp(state_.get(), &record, sizeof(record)) && !dirty_) return true;
  if (!credits_) { error_ = "companion write budget exhausted; retry after 15 seconds"; return false; }
  --credits_;
#if NRFMAST_PRODUCTION_LUA
  nvs_handle_t handle = 0;
  auto status = nvs_open("pine-companion", NVS_READWRITE, &handle);
  if (status == ESP_OK) status = nvs_set_blob(handle, "state", &record, sizeof(record));
  if (status != ESP_OK) {
    if (handle) nvs_close(handle);
    error_ = "companion journal staging failed; previous selection retained"; return false;
  }
  status = nvs_commit(handle);
  nvs_close(handle);
  if (status != ESP_OK) {
    ready_ = false; error_ = "companion commit uncertain; restart before changing state"; return false;
  }
  *state_ = record; dirty_ = false; error_ = "ok"; return true;
#else
  return false;
#endif
}
void CompanionStore::changed(uint32_t now) {
  if (!ready_ || dirty_) return;
  dirty_ = true; dirtyAt_ = now;
}
void CompanionStore::loop(uint32_t now) {
  if (!ready_ || !dirty_ || uint32_t(now - dirtyAt_) < FlushMs) return;
  auto next = allocateSnapshot();
  if (next) { capture(*next); if (save(*next, now)) return; }
  dirtyAt_ = now;
  Serial.printf("Companion storage: %s\n", error_);
}
bool CompanionStore::contact(const ContactInfo &contact, bool remove, uint32_t now) {
  if (!ready_) return false;
  auto next = allocateSnapshot();
  if (!next) return false;
  capture(*next);
  unsigned i = 0;
  while (i < next->count && memcmp(next->contacts[i].key, contact.id.pub_key, 32)) ++i;
  if (remove) {
    if (i == next->count) return false;
    for (unsigned j = i + 1; j < next->count; ++j) next->contacts[j - 1] = next->contacts[j];
    next->contacts[--next->count] = Contact{};
  } else {
    if (i == next->count) {
      if (i == Contacts) { error_ = "companion contact table full"; return false; }
      ++next->count;
    }
    encode(next->contacts[i], contact);
  }
  return save(*next, now);
}
bool CompanionStore::channel(unsigned slot, const ChannelDetails &channel, uint32_t now) {
  if (!ready_ || slot >= Channels) return false;
  auto next = allocateSnapshot();
  if (!next) return false;
  capture(*next);
  memcpy(next->channels[slot].name, channel.name, 32);
  memcpy(next->channels[slot].secret, channel.channel.secret, 16);
  return save(*next, now);
}
bool CompanionStore::putAdvert(const uint8_t *key, const uint8_t *bytes, int size, uint32_t now) {
  if (!ready_ || size <= 0 || size > MAX_TRANS_UNIT) return false;
  mesh::Packet packet{};
  if (!packet.readFrom(bytes, size) || packet.getPayloadType() != PAYLOAD_TYPE_ADVERT ||
      packet.payload_len < 100 || memcmp(packet.payload, key, 32)) return false;
  // An advert's signature covers its payload, not the received route.
  packet.path_len = 0;
  uint8_t raw[MAX_TRANS_UNIT];
  size = packet.writeTo(raw);
  if (size > int(sizeof(Contact::advert))) return false;
  unsigned i = 0;
  while (i < state_->count && memcmp(state_->contacts[i].key, key, 32)) ++i;
  if (i == state_->count) {
    for (i = 0; i < state_->count; ++i)
      if (!bot_.lookupContactByPubKey(state_->contacts[i].key, 32)) break;
    if (i == state_->count) {
      if (i == Contacts) return false;
      ++state_->count;
    }
    state_->contacts[i] = Contact{};
    memcpy(state_->contacts[i].key, key, 32);
  }
  auto &dest = state_->contacts[i];
  dest.advertLength = size;
  memcpy(dest.advert, raw, size);
  memset(dest.advert + size, 0, sizeof(dest.advert) - size);
  changed(now); return true;
}
int CompanionStore::getAdvert(const uint8_t *key, uint8_t *bytes) const {
  if (!ready_) return 0;
  for (unsigned i = 0; i < state_->count; ++i)
    if (!memcmp(state_->contacts[i].key, key, 32)) {
      memcpy(bytes, state_->contacts[i].advert, state_->contacts[i].advertLength);
      return state_->contacts[i].advertLength;
    }
  return 0;
}
}
