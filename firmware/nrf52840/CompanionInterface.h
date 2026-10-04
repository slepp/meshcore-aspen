#pragma once

#include "RuntimeConfig.h"
#include <helpers/BaseSerialInterface.h>

namespace nrfmast {

struct CompanionRadioInfo {
  float frequency, bandwidth;
  uint8_t sf, cr;
  int8_t power, maxPower;
  float airtimeFactor, lastRssi, lastSnr;
  uint32_t physicalReceived, physicalSent, physicalErrors;
  int16_t noiseFloor;
};

class CompanionInterface {
  CommandBot& bot;
  RuntimeConfig& config;
  BaseSerialInterface& transport;
  void (*radioInfo)(CompanionRadioInfo&);
  uint16_t (*battery)();
  void (*storage)(uint32_t&, uint32_t&);
  ContactsIterator iterator{0};
  bool iterating = false;
  uint8_t version = 3, inboxCount = 0;
  uint32_t since = 0, lastmod = 0, expectedAck = 0;
  uint8_t pendingKey[PUB_KEY_SIZE] = {};
  uint8_t pendingType = 0;
  uint32_t pendingAt = 0;
  struct Frame { uint8_t length, bytes[MAX_FRAME_SIZE]; };
  Frame inbox[4] = {};
  uint8_t input[MAX_FRAME_SIZE + 1] = {}, output[MAX_FRAME_SIZE] = {};

  void respond(uint8_t code);
  void error(uint8_t code);
  void contactFrame(uint8_t code, const ContactInfo&);
  ContactInfo* uniqueContact(const uint8_t* prefix, size_t length);
  void sent(int result, uint32_t tag, uint32_t timeout);
  void handle(size_t length);

public:
  uint32_t inboxDrops = 0;
  CompanionInterface(CommandBot& commandBot, RuntimeConfig& runtime, BaseSerialInterface& interface,
                     void (*profile)(CompanionRadioInfo&), uint16_t (*batteryRead)() = nullptr,
                     void (*storageRead)(uint32_t&, uint32_t&) = nullptr)
      : bot(commandBot), config(runtime), transport(interface), radioInfo(profile),
        battery(batteryRead), storage(storageRead) {}
  void loop();
  void discovered(const ContactInfo&, bool isNew);
  void pathUpdated(const ContactInfo&);
  void received(const ContactInfo&, const mesh::Packet&, uint32_t timestamp, uint8_t type, const char* text);
  void confirmed(uint32_t ack, uint32_t elapsed);
  void response(const ContactInfo&, const uint8_t*, size_t);
};

}  // namespace nrfmast
