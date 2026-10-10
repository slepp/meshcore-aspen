// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PineRoom.h"
#include "PinePacketPrograms.h"
#include "PinePeerDevice.h"
#include "SharedRadio.h"
#include "room/PineRoomMesh.h"
#include "NoteDiagnostics.h"
#include <InternalFileSystem.h>
#include <utility/debug.h>
#include <new>
#include <cassert>

namespace nrfmast {
namespace room_pool {
#define ONCHIP_PACKET_CAPACITY PINE_ROOM_PACKETS
#include "onchip/support/PacketPool.h"
#include "onchip/support/PacketPool.inc"
#undef ONCHIP_PACKET_CAPACITY
}
namespace {
constexpr char Selection[] = "/room-enabled";
constexpr char IdentityName[] = "_pineroom";
constexpr uint32_t FreeReserve = 8192;
struct Room {
  RadioPort port;
  room_pool::StaticPoolPacketManager packets{PINE_ROOM_PACKETS};
  SimpleMeshTables tables;
  PineRoomMesh mesh;
  Room(SharedRadio& radio, mesh::MillisecondClock& clock, mesh::RNG& rng, mesh::RTCClock& rtc)
      : port(radio, 3), mesh(board, port, clock, rng, rtc, tables, packets) {}
};
Room* instance;
bool selected;
const char* failure = "";

bool readIdentity(mesh::LocalIdentity& identity) {
  IdentityStore store(InternalFS, "");
  return store.load(IdentityName, identity);
}
bool saveSelection(bool enabled) {
  const char* pending = "/room-enabled.pending";
  InternalFS.remove(pending);
  File file = InternalFS.open(pending, FILE_O_WRITE);
  const uint8_t value = enabled ? 1 : 0;
  const bool written = file && file.write(&value, 1) == 1;
  file.close();
  file = InternalFS.open(pending);
  uint8_t check = 2;
  const bool valid = written && file && file.size() == 1 && file.read(&check, 1) == 1 && check == value;
  file.close();
  if (!valid) { InternalFS.remove(pending); return false; }
  if (!InternalFS.rename(pending, Selection)) return false;
  selected = enabled;
  return true;
}
}

bool roomSelected() {
  if (!InternalFS.exists(Selection)) return false;
  File file = InternalFS.open(Selection);
  uint8_t value = 2;
  return !file || file.size() != 1 || file.read(&value, 1) != 1 || value != 0;
}
bool roomRunning() { return instance != nullptr; }

bool roomUnsafeCommand(const char* command) {
  // Allow only role-local upstream operations; prefix matching in CommonCLI
  // otherwise reaches device reset, filesystem, radio and identity operations.
  static const char* allowed[] = {
      "ver", "name", "password", "get ", "setperm ", "set name ", "set password ",
      "set guest.password ", "set room.", "set owner.info ", "set advert.", "set flood.advert.",
      "set latitude ", "set longitude ", "advert", "advert.zerohop", "room.post ", "stats", "region"
  };
  if (!strncmp(command, "password ", 9)) {
    const char* password = command + 9;
    const size_t length = strlen(password);
    if (!length || length > 15 || password[0] == ' ' || password[length - 1] == ' ') return true;
  }
  for (const auto* prefix : allowed) {
    const size_t length = strlen(prefix);
    if (!strncmp(command, prefix, length) &&
        (prefix[length - 1] == ' ' || prefix[length - 1] == '.' ||
         command[length] == 0 || command[length] == ' ')) {
      if (!strncmp(command, "set password ", 13) && !command[13]) return true;
      return false;
    }
  }
  return true;
}

bool beginRoom(SharedRadio& radio, mesh::MillisecondClock& clock, mesh::RNG& rng, mesh::RTCClock& rtc) {
  static_assert(sizeof(Room) <= 16384, "Pine Room exceeds its 16KiB role budget");
  selected = false;
  if (InternalFS.exists(Selection)) {
    File file = InternalFS.open(Selection);
    uint8_t value = 2;
    const bool valid = file && file.size() == 1 && file.read(&value, 1) == 1 && value <= 1;
    file.close();
    if (!valid) { failure = "saved Room selection unreadable; native repeater remains available"; return false; }
    selected = value == 1;
  }
  if (!selected) return false;
  mesh::LocalIdentity identity;
  if (!readIdentity(identity)) { failure = "Room identity unavailable; use local room init"; return false; }
  IdentityStore store(InternalFS, "");
  mesh::LocalIdentity other;
  if ((store.load("_main", other) && identity.matches(other)) ||
      (store.load("_nrfbot", other) && identity.matches(other))) {
    failure = "Room identity duplicates another role; restore the independent Room identity"; return false;
  }
  if (InternalFS.exists("/room-prefs.json")) {
    File file = InternalFS.open("/room-prefs.json");
    PineRoomPrefs prefs;
    const bool valid = file && prefs.loadSerial(file);
    file.close();
    if (!valid) { failure = "Room preferences unreadable; restore their backup"; return false; }
  }
  if (dbgHeapFree() < int(sizeof(Room) + FreeReserve)) {
    failure = "Room allocation would consume the 8KiB reserve"; return false;
  }
  instance = new (std::nothrow) Room(radio, clock, rng, rtc);
  if (!instance) { failure = "Room allocation failed"; return false; }
  if (dbgHeapFree() < int(FreeReserve)) {
    failure = "Room auxiliary allocation would consume the 8KiB reserve";
    delete instance; instance = nullptr; return false;
  }
  instance->mesh.self_id = identity;
  instance->mesh.begin(&InternalFS);
  auto* prefs = instance->mesh.getNodePrefs();
  if (!prefs->password[0]) {
    mesh::LocalIdentity entropy = radio_new_identity();
    char publicHex[65];
    mesh::Utils::toHex(publicHex, entropy.pub_key, PUB_KEY_SIZE);
    memcpy(prefs->password, publicHex, 15);
    prefs->password[15] = 0;
    if (!instance->mesh.saveChecked()) {
      failure = "Room administrator password save failed";
      delete instance; instance = nullptr; return false;
    }
  }
  failure = "";
  radio.room = &instance->port;
  return true;
}

bool roomCommand(uint32_t timestamp, const char* text, char* reply) {
  if (strcmp(text, "room") && strncmp(text, "room ", 5)) return false;
  const char* command = text[4] ? text + 5 : "status";
  if (!strcmp(command, "status")) {
    snprintf(reply, 160, "Room saved=%u live=%u clients=%u posts=%u packets=%u ram=%u Lua=%s%s%s",
             selected, instance != nullptr, PINE_ROOM_CLIENTS, PINE_ROOM_POSTS, PINE_ROOM_PACKETS,
             unsigned(sizeof(Room)), instance ? "paused" : "available", *failure ? " error=" : "", failure);
  } else if (!strcmp(command, "id")) {
    mesh::LocalIdentity identity;
    if (!readIdentity(identity)) strcpy(reply, "Error: Room identity unavailable; use local room init");
    else {
      char key[65];
      mesh::Utils::toHex(key, identity.pub_key, PUB_KEY_SIZE);
      snprintf(reply, 160, "Room public_key=%s", key);
    }
  } else if (!strcmp(command, "init")) {
    mesh::LocalIdentity identity;
    if (timestamp) strcpy(reply, "Error: room init requires the local USB console");
    else if (readIdentity(identity)) strcpy(reply, "OK: Room identity already saved; unchanged");
    else if (InternalFS.exists("/_pineroom.id")) strcpy(reply, "Error: Room identity unreadable; restore its backup");
    else {
      identity = radio_new_identity();
      while (identity.pub_key[0] == 0 || identity.pub_key[0] == 0xFF)
        identity = radio_new_identity();
      IdentityStore store(InternalFS, "");
      const bool saved = store.save(IdentityName, identity);
      mesh::LocalIdentity check;
      if (saved && readIdentity(check) && check.matches(identity))
        strcpy(reply, "OK: Room identity saved; room enable on; reboot to start Room instead of Lua");
      else strcpy(reply, "Error: Room identity save/readback failed; saved state retained");
    }
  } else if (!strcmp(command, "enable on") || !strcmp(command, "enable off")) {
    const bool enabled = !strcmp(command, "enable on");
    mesh::LocalIdentity identity;
    if (enabled && (packetProgramsSelected() || peerDeviceSelected())) strcpy(reply, "Error: disable packet/peer role and restart before enabling Room");
    else if (enabled && !readIdentity(identity)) strcpy(reply, "Error: Room identity unavailable; use local room init");
    else if (!saveSelection(enabled)) strcpy(reply, "Error: Room selection save failed; previous selection retained");
    else strcpy(reply, enabled ? "OK: Room selected; reboot to pause Lua and start Room; Lua source retained"
                             : "OK: Lua selected; reboot to stop Room and resume saved Lua source");
  } else if (!instance) strcpy(reply, "Error: Room not running; room status; room enable on; reboot");
  else {
    char mutableCommand[160];
    if (strlen(command) >= sizeof(mutableCommand)) strcpy(reply, "Error: Room command too long");
    else {
      strcpy(mutableCommand, command);
      instance->mesh.handleCommand(timestamp, mutableCommand, reply);
    }
  }
  return true;
}

void roomLoop() { if (instance) instance->mesh.loop(); }
}
#endif
