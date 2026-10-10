// SPDX-License-Identifier: Apache-2.0
#include "PineRoom.h"
#include "PineApplication.h"
#include "SharedRadio.h"
#include "room/PineRoomMesh.h"
#include <SPIFFS.h>
#include <target.h>
#include <deque>
#include <vector>
#include <cassert>
#include <string>

static uint32_t now = 1000;
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
long random(long low, long high) { return low + (now++ % (high - low)); }
void randomSeed(long) {}
RoomTestBoard board;
RoomTestDriver radio_driver;
SensorManager sensors;
static unsigned identities = 0;
mesh::LocalIdentity radio_new_identity() {
  struct Seed : mesh::RNG {
    void random(uint8_t* bytes, size_t length) override {
      memset(bytes, identities, length);
    }
  } seed;
  ++identities;
  return mesh::LocalIdentity(&seed);
}
struct Clock : mesh::MillisecondClock {
  unsigned long getMillis() override { return now; }
};
struct RTC : mesh::RTCClock {
  uint32_t getCurrentTime() override { return 1800000000 + now / 1000; }
  void setCurrentTime(uint32_t) override {}
};
struct RNG : mesh::RNG {
  void random(uint8_t* data, size_t size) override {
    for (size_t i = 0; i < size; ++i) data[i] = i + now++;
  }
};
struct Radio : mesh::Radio {
  unsigned sent = 0;
  bool active = false;
  std::deque<std::vector<uint8_t>> frames;
  int recvRaw(uint8_t* data, int capacity) override {
    if (frames.empty()) return 0;
    auto frame = frames.front(); frames.pop_front();
    assert(frame.size() <= size_t(capacity));
    memcpy(data, frame.data(), frame.size()); return frame.size();
  }
  uint32_t getEstAirtimeFor(int) override { return 10; }
  float packetScore(float, int) override { return 1; }
  bool startSendRaw(const uint8_t*, int) override { assert(!active); active = true; ++sent; return true; }
  bool isSendComplete() override { return active; }
  void onSendFinished() override { active = false; }
  bool isInRecvMode() const override { return !active; }
};

int main() {
  assert(nrfmast::applicationBytes(0x27000, 0xD7004, 0x20006000, 0x200077B0, 0xED000) == 726964);
  assert(!nrfmast::applicationBytes(0, 0xD7004, 0x20006000, 0x200077B0, 0xED000));
  assert(!nrfmast::applicationBytes(0x27000, 0xF0000, 0x20006000, 0x200077B0, 0xED000));
  assert(!nrfmast::applicationBytes(0x27000, 0xD7004, 0x20008000, 0x200077B0, 0xED000));
  Clock clock;
  RTC rtc;
  RNG rng;
  Radio radio;
  nrfmast::SharedRadio shared(radio, clock, true);
  filesystem_test::files["/prefs.json"] = {1, 2, 3};
  filesystem_test::files["/_main.id"] = {4, 5, 6};
  filesystem_test::files["/_nrfbot.id"] = {7, 8, 9};
  const auto original = filesystem_test::files;
  auto command = [](const char* text, uint32_t timestamp = 0) {
    char reply[160]{};
    assert(nrfmast::roomCommand(timestamp, text, reply));
    return std::string(reply);
  };
  assert(!nrfmast::beginRoom(shared, clock, rng, rtc));
  assert(!nrfmast::roomSelected());
  assert(command("room init", 123).find("local USB") != std::string::npos);
  assert(identities == 0);
  assert(command("room enable on").find("identity unavailable") != std::string::npos);
  assert(command("room init").find("identity saved") != std::string::npos);
  const auto identity = filesystem_test::files.at("/_pineroom.id");
  assert(command("room init").find("unchanged") != std::string::npos);
  assert(filesystem_test::files.at("/_pineroom.id") == identity);
  assert(command("room enable on").find("Room selected") != std::string::npos);
  assert(nrfmast::beginRoom(shared, clock, rng, rtc));
  assert(nrfmast::roomRunning() && nrfmast::roomSelected());
  assert(command("room status").find("Lua=paused") != std::string::npos);
  assert(command("room set name Pocket Room") == "OK");
  assert(command("room get name") == "> Pocket Room");
  assert(command("room password test-room-pass").find("saved") != std::string::npos);
  for (const auto* text : {"room password ", "room erase", "room set radio 918,250,7,5",
                           "room set tx 22", "room set repeat on", "room reboot",
                           "room start ota confirm", "room set prv.key ABCD"})
    assert(command(text).find("Error:") == 0);
  assert(command("room room.post hello from Pine") == "OK");
  assert(command("room advert") == "OK - advert queued");
  for (unsigned i = 0; i < 50; ++i) {
    now += 100;
    shared.poll();
    if (shared.roomCanLoop()) nrfmast::roomLoop();
  }
  assert(radio.sent > 0);
  assert(!radio.active);
  assert(filesystem_test::files.at("/_pineroom.id") == identity);
  for (const auto& item : original) assert(filesystem_test::files.at(item.first) == item.second);
  const auto prefs = filesystem_test::files.at("/room-prefs.json");
  filesystem_test::writeLimit = 0;
  assert(command("room set name Lost Room").find("save failed") != std::string::npos);
  filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  assert(filesystem_test::files.at("/room-prefs.json") == prefs);
  assert(command("room get name") == "> Pocket Room");
  PineRoomACL acl;
  acl.load(&SPIFFS, mesh::LocalIdentity{});
  std::vector<mesh::LocalIdentity> admins;
  for (unsigned i = 0; i < PINE_ROOM_CLIENTS; ++i) {
    admins.push_back(radio_new_identity());
    assert(acl.applyPermissions(admins.back(), admins.back().pub_key, 32, PERM_ACL_ADMIN));
  }
  const auto excess = radio_new_identity();
  assert(!acl.putClient(excess, 0));
  assert(!acl.applyPermissions(excess, excess.pub_key, 32, PERM_ACL_ADMIN));
  for (const auto& admin : admins) assert(acl.getClient(admin.pub_key, 32));
  assert(acl.save(&SPIFFS, PineRoomMesh::saveFilter));
  const auto contacts = filesystem_test::files.at("/room-s_contacts");
  filesystem_test::writeLimit = 0;
  assert(!acl.save(&SPIFFS, PineRoomMesh::saveFilter));
  filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  assert(filesystem_test::files.at("/room-s_contacts") == contacts);
  PineRoomACL reload;
  reload.load(&SPIFFS, admins.front());
  for (const auto& admin : admins) assert(reload.getClient(admin.pub_key, 32));
  PineRoomPrefs reloadPrefs;
  auto file = SPIFFS.open("/room-prefs.json");
  assert(file && reloadPrefs.loadSerial(file));
  file.close();
  assert(!strcmp(reloadPrefs.node_name, "Pocket Room"));
  assert(!strcmp(reloadPrefs.password, "test-room-pass"));
  assert(command("room enable off").find("Lua selected") != std::string::npos);
  assert(!nrfmast::roomSelected() && nrfmast::roomRunning());
  puts("Pine Room: independent identity, isolated saved settings, native post/advert, admission and save recovery passed");
}
