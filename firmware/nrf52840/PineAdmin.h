// SPDX-License-Identifier: Apache-2.0
#pragma once
#if NRFMAST_PRODUCTION_LUA
#include "onchip/MastSource.h"
namespace onchip {
class CommandBot;
class MastAdmin {
public:
  static constexpr size_t TextLimit = 162;
  enum class Transport { Other, NativeEncrypted };
  struct Reply { char text[TextLimit + 1]{}; uint32_t ticket = 0; };
  bool begin();
  void loop();
  void stop();
  void execute(const char *, Reply &, uint32_t invokingJob = 0,
               Transport transport = Transport::Other, const uint8_t *nativeSender = nullptr);
  bool trusted(const uint8_t key[32]) const;
  bool ready() const { return ready_; }
  bool rememberTimestamp(const uint8_t key[32], uint32_t timestamp);
  void acknowledged(uint32_t ticket, bool transmitted);
  static MastAdmin *service();
private:
  struct Owner { uint8_t magic[4]{'P','L','O',1}, key[32]{}; uint32_t timestamp = 0; uint8_t digest[32]{}; } owner_;
  bool ready_ = false;
  MastSource source_;
  uint32_t rebootTicket_ = 0, rebootAt_ = 0;
  bool saveOwner(const Owner &);
};
CommandBot &commandBotService();
}
#endif
