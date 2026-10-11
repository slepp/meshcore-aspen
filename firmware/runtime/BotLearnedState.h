// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <SPIFFS.h>
#include <Utils.h>
#include <Packet.h>
#include <cstring>

namespace onchip {
// Dispatcher-owned cache. Operator policies and flood cooldowns have separate authorities.
class BotLearnedState {
public:
  static constexpr uint32_t CheckpointMs = 300000, ContactSeconds = 7 * 86400,
                            RouteSeconds = ContactSeconds, FreshRouteSeconds = 600;
  struct Entry {
    uint8_t key[32]{}, advert[255]{}, advertSize = 0;
    uint8_t route[MAX_PATH_SIZE]{}, routeLength = 0xff, scope = 0;
    uint32_t acceptedUtc = 0, heardUtc = 0, routeUtc = 0;
  };
  struct Snapshot {
    uint32_t magic = 0x424c5331, version = 1, sequence = 0;
    uint8_t owner[32]{};
    uint32_t frequency = 0, bandwidth = 0;
    uint8_t sf = 0, cr = 0;
    Entry entries[16]{};
    uint8_t digest[32]{};
  } data{};
  bool dirty = false, failed = false;
  uint32_t changedAt = 0, attemptedAt = 0, writes = 0, restored = 0, rejected = 0, saveMs = 0;
private:
  unsigned slot_ = 1;
  uint32_t sequence_ = 0;
  uint8_t owner_[32]{};
  bool attempted_ = false;
  static const char *path(unsigned slot) {
    return slot ? "/command-bot/learned-1" : "/command-bot/learned-0";
  }
  static void digest(Snapshot &snapshot) {
    mesh::Utils::sha256(snapshot.digest, 32, reinterpret_cast<const uint8_t *>(&snapshot),
                       offsetof(Snapshot, digest));
  }
  static bool valid(const Snapshot &snapshot, const uint8_t owner[32]) {
    uint8_t hash[32];
    mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(&snapshot),
                       offsetof(Snapshot, digest));
    if (snapshot.magic != 0x424c5331 || snapshot.version != 1 ||
        memcmp(snapshot.owner, owner, 32) || memcmp(snapshot.digest, hash, 32)) return false;
    for (const auto &entry : snapshot.entries)
      if ((entry.routeLength != 0xff && !mesh::Packet::isValidPathLen(entry.routeLength)) ||
          entry.scope > 2) return false;
    return true;
  }
  bool read(const char *name, Snapshot &snapshot, const uint8_t owner[32]) {
    auto file = SPIFFS.open(name, "r");
    return file && file.size() == sizeof(snapshot) &&
        file.read(reinterpret_cast<uint8_t *>(&snapshot), sizeof(snapshot)) == sizeof(snapshot) &&
        valid(snapshot, owner);
  }
public:
  void load(const uint8_t owner[32]) {
    memcpy(owner_, owner, 32);
    data = {};
    const bool first = read(path(0), data, owner_);
    const uint32_t firstSequence = first ? data.sequence : 0;
    const bool second = read(path(1), data, owner_);
    rejected = (SPIFFS.exists(path(0)) && !first) + (SPIFFS.exists(path(1)) && !second);
    if (second && (!first || int32_t(data.sequence - firstSequence) > 0)) slot_ = 1;
    else if (first && read(path(0), data, owner_)) slot_ = 0;
    else data = {};
    sequence_ = data.sequence; memcpy(data.owner, owner_, 32);
    failed = false;
  }
  void changed(uint32_t now) {
    if (!dirty) { dirty = true; changedAt = now; }
  }
  uint32_t wait(uint32_t now) const {
    if (!dirty) return UINT32_MAX;
    const uint32_t elapsed = now - (attempted_ ? attemptedAt : changedAt);
    return elapsed >= CheckpointMs ? 0 : CheckpointMs - elapsed;
  }
  bool save(uint32_t now, bool force) {
    if (!dirty || (!force && wait(now))) return true;
    attempted_ = true; attemptedAt = now;
    const uint32_t started = millis();
    data.magic = 0x424c5331; data.version = 1; data.sequence = ++sequence_;
    memcpy(data.owner, owner_, 32); digest(data);
    uint8_t expected[32]; memcpy(expected, data.digest, 32);
    constexpr const char *scratch = "/command-bot/learned-writing";
    auto file = SPIFFS.open(scratch, "w");
    bool written = file && file.write(reinterpret_cast<const uint8_t *>(&data), sizeof(data)) == sizeof(data);
    if (file) { file.flush(); file.close(); }
    written = written && read(scratch, data, owner_) && !memcmp(data.digest, expected, 32);
    const unsigned target = slot_ ^ 1;
    if (written && SPIFFS.exists(path(target))) written = SPIFFS.remove(path(target));
    if (written) written = SPIFFS.rename(scratch, path(target));
    if (written) written = read(path(target), data, owner_) && !memcmp(data.digest, expected, 32);
    saveMs = uint32_t(millis() - started);
    failed = !written;
    if (!written) { SPIFFS.remove(scratch); return false; }
    slot_ = target; ++writes; dirty = false; attempted_ = false;
    return true;
  }
};
} // namespace onchip
