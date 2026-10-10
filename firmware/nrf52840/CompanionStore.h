// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <helpers/BaseChatMesh.h>
#include <helpers/BaseSerialInterface.h>
#include <memory>

namespace nrfmast {
class CompanionStore {
public:
  static constexpr unsigned Contacts = 8, Channels = 4;
  static constexpr uint32_t FlushMs = 30000, RefillMs = 15000;
  struct Contact {
    uint8_t key[32]{}, name[32]{}, path[64]{};
    uint8_t type = 0, flags = 0, pathLength = OUT_PATH_UNKNOWN, advertLength = 0;
    uint32_t advertTime = 0, modified = 0;
    int32_t latitude = 0, longitude = 0;
    uint32_t syncSince = 0;
    uint8_t advert[MAX_FRAME_SIZE - 1]{};
    uint8_t reserved = 0;
  };
  struct Channel { char name[32]{}; uint8_t secret[16]{}; };
  struct Record {
    uint8_t magic[4]{'P', 'C', 'S', 1}, owner[32]{};
    uint32_t count = 0;
    Contact contacts[Contacts]{};
    Channel channels[Channels]{};
    uint8_t digest[32]{};
  };
  static_assert(sizeof(Record) <= 4096, "Pine companion metadata exceeds one record");
  static_assert(sizeof(Contact) == 328 && sizeof(Record) == 2888,
                "Pine companion journal layout changed");
private:
  BaseChatMesh &bot_;
  std::unique_ptr<Record> state_;
  uint32_t dirtyAt_ = 0, refillAt_ = 0;
  uint8_t credits_ = 8;
  bool ready_ = false, dirty_ = false;
  const char *error_ = "not started";
  std::unique_ptr<Record> allocateSnapshot();
  void capture(Record &) const;
  bool save(Record &, uint32_t now);
  bool validate(const Record &) const;
public:
  explicit CompanionStore(BaseChatMesh &bot) : bot_(bot) {}
  bool begin(uint32_t now);
  void stop() { ready_ = dirty_ = false; state_.reset(); error_ = "companion store stopped; restart required"; }
  bool ready() const { return ready_; }
  const char *error() const { return error_; }
  void changed(uint32_t now);
  void loop(uint32_t now);
  bool contact(const ContactInfo &, bool remove, uint32_t now);
  bool channel(unsigned slot, const ChannelDetails &, uint32_t now);
  bool putAdvert(const uint8_t *key, const uint8_t *bytes, int size, uint32_t now);
  int getAdvert(const uint8_t *key, uint8_t *bytes) const;
};
}
