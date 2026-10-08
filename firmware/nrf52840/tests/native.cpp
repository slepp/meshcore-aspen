#include "CommandBot.h"
#include "SharedRadio.h"
#include "../../runtime/AdaptiveAdmission.h"
#include "RuntimeConfig.h"
#include "NoteStore.h"
#include "CompanionInterface.h"
#include "CompanionProtocol.h"
#include "../../esp32/FirmwareIdentity.h"
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/TransportKeyStore.h>
#include <cassert>
#include <deque>
#include <limits>
#include <string>
#include <vector>

struct Clock : mesh::MillisecondClock {
  uint32_t now = 1000;
  unsigned long getMillis() override { return now; }
};
struct RTC : mesh::RTCClock {
  uint32_t now = 1800000000;
  uint32_t getCurrentTime() override { return now; }
  void setCurrentTime(uint32_t value) override { now = value; }
};
struct Random : mesh::RNG {
  uint32_t state = 9;
  void random(uint8_t* out, size_t length) override {
    while (length--) { state = state * 1664525 + 1013904223; *out++ = state >> 24; }
  }
};
struct Physical : mesh::Radio {
  std::deque<std::vector<uint8_t>> input;
  std::vector<std::vector<uint8_t>> sent;
  bool sending = false, failStart = false, complete = true;
  bool completionReported = false;
  int beginCount = 0, reads = 0, finishes = 0;
  float rssi = -87, snr = -2.25;
  void begin() override { ++beginCount; }
  int recvRaw(uint8_t* out, int capacity) override {
    assert(!sending);
    ++reads;
    if (input.empty()) return 0;
    auto value = input.front(); input.pop_front();
    assert(int(value.size()) <= capacity);
    memcpy(out, value.data(), value.size());
    return value.size();
  }
  uint32_t airtime = 40;
  uint32_t getEstAirtimeFor(int) override { return airtime; }
  float packetScore(float, int) override { return 1; }
  bool startSendRaw(const uint8_t* bytes, int length) override {
    assert(!sending);
    if (failStart) return false;
    sending = true;
    completionReported = false;
    sent.emplace_back(bytes, bytes + length);
    return true;
  }
  bool isSendComplete() override {
    const bool ready = sending && complete && !completionReported;
    if (ready) completionReported = true;
    return ready;
  }
  void onSendFinished() override { assert(sending); sending = false; ++finishes; }
  bool isInRecvMode() const override { return !sending; }
  float getLastRSSI() const override { return rssi; }
  float getLastSNR() const override { return snr; }
};

static void testNotes() {
  Clock clock;
  NativeFilesystem fs;
  nrfmast::NoteStore notes(fs, clock);
  assert(notes.begin());
  uint8_t bot[32] = {1}, user[32] = {2}, other[32] = {2}, otherBot[32] = {1};
  other[31] = otherBot[31] = 1;  // Same prefixes must not share private scopes.
  char reply[161];
  uint32_t timestamp = 100;
  auto run = [&](const char* text, const uint8_t* principal = nullptr, const uint8_t* identity = nullptr) {
    clock.now += nrfmast::NoteStore::WRITE_REFILL_MS;
    memset(reply, 0, sizeof(reply));
    assert(notes.command(identity ? identity : bot, principal ? principal : user, ++timestamp,
                         text, reply, sizeof(reply)));
    assert(strlen(reply) < sizeof(reply));
    return std::string(reply);
  };
  assert(run("!remember antenna spare in box") == "remembered antenna");
  assert(notes.countNotes() == 1 && notes.countPrincipals() == 1);
  assert(run("!recall antenna") == "antenna=spare in box");
  assert(run("!recall antenna", other).find("not found") != std::string::npos);
  assert(run("!recall antenna", nullptr, otherBot).find("not found") != std::string::npos);
  assert(run("!list") == "notes 1/4: antenna");
  for (size_t capacity = 1; capacity <= 8; ++capacity) {
    char tiny[10];
    memset(tiny, 'x', sizeof(tiny));
    assert(notes.command(bot, user, timestamp, "!list", tiny, capacity));
    assert(memchr(tiny, 0, capacity) && tiny[capacity] == 'x');
  }
  assert(run("!remember antenna replaced") == "remembered antenna");
  auto before = *fs.files.at("/pine-notes");
  assert(before.size() == 3404);
  fs.writeBudget = 180;
  assert(run("!remember antenna failed").find("commit failed") != std::string::npos);
  assert(*fs.files.at("/pine-notes") == before);
  assert(fs.activeHandles == 0);
  fs.writeBudget = -1;
  fs.failRename = true;
  assert(run("!forget antenna").find("commit failed") != std::string::npos);
  assert(*fs.files.at("/pine-notes") == before);
  fs.failRename = false;
  assert(run("!recall antenna") == "antenna=replaced");
  assert(run("!remember b two") == "remembered b");
  assert(run("!remember c three") == "remembered c");
  assert(run("!remember d four") == "remembered d");
  assert(run("!remember e five").find("principal quota") != std::string::npos);
  assert(run("!list") == "notes 4/4: antenna b c d");
  assert(run("!forget antenna") == "forgot antenna");
  const uint32_t forgottenAt = timestamp;
  assert(run("!recall antenna").find("not found") != std::string::npos);
  nrfmast::NoteStore restarted(fs, clock);
  assert(restarted.begin());
  assert(restarted.command(bot, user, forgottenAt - 1, "!remember antenna replay", reply, sizeof(reply)));
  assert(strstr(reply, "stale write"));
  assert(restarted.command(bot, user, ++timestamp, "!list", reply, sizeof(reply)));
  assert(!strcmp(reply, "notes 3/4: b c d"));
  for (auto bad : {"!list junk", "!remember", "!remember ../escape x", "!remember key",
                   "!remember key line\nbreak", "!recall b extra", "!forget b extra",
                   "!remember 12345678901234567 x"})
    assert(run(bad).find("Error:") == 0);
  assert(run(("!remember too-long " + std::string(97, 'a')).c_str()).find("96 bytes") != std::string::npos);
  assert(!notes.command(bot, user, ++timestamp, "!ping", reply, sizeof(reply)));
  assert(run(("!remember max " + std::string(96, 'x')).c_str()).find("remembered") == 0);
  assert(run("!forget b").find("forgot") == 0);
  assert(run("!remember b again").find("remembered") == 0);
  // Four full-key principals fill the global 16-note quota without eviction.
  for (int p = 3; p <= 5; ++p) {
    uint8_t principal[32] = {};
    principal[0] = p;
    for (int k = 0; k < 4; ++k)
      assert(run(("!remember n" + std::to_string(k) + " value").c_str(), principal).find("remembered") == 0);
  }
  uint8_t principal[32] = {6};
  assert(run("!remember overflow x", principal).find("device quota 16") != std::string::npos);
  assert(run("!forget nonexistent", principal).find("no change") != std::string::npos);
  assert(run("!forget b").find("forgot") == 0);
  assert(run("!remember n0 value", principal).find("remembered") == 0);
  assert(run("!forget n0", principal).find("forgot") == 0);
  for (int p = 7; p <= 9; ++p) {
    principal[0] = p;
    assert(run("!remember n0 value", principal).find("remembered") == 0);
    assert(run("!forget n0", principal).find("forgot") == 0);
  }
  principal[0] = 10;
  assert(run("!remember n0 value", principal).find("quota 8 full-key principals") != std::string::npos);
  assert(notes.countNotes() == 15 && notes.countPrincipals() == 8);
  fs.files.at("/pine-notes")->at(80) ^= 1;
  assert(!restarted.begin());
  assert(restarted.command(bot, user, ++timestamp, "!recall b", reply, sizeof(reply)));
  assert(strstr(reply, "storage unavailable"));
  fs.files.at("/pine-notes")->resize(2);
  assert(!restarted.begin() && fs.activeHandles == 0);
  assert(restarted.reset() && restarted.begin());
  std::puts("notes: full-key isolation, quotas, restart/replay, validation, atomic failures and corruption");
}

static void testNoteWriteBudget() {
  using Store = nrfmast::NoteStore;
  Clock clock;
  NativeFilesystem fs;
  Store notes(fs, clock);
  assert(notes.begin());
  uint8_t bot[32] = {1}, user[32] = {2}, other[32] = {2}, third[32] = {4};
  other[31] = 1;
  uint32_t timestamp = 100;
  char reply[161];
  auto run = [&](const char* text, const uint8_t* principal = nullptr, uint32_t senderTime = 0) {
    assert(notes.command(bot, principal ? principal : user, senderTime ? senderTime : ++timestamp,
                         text, reply, sizeof(reply)));
    return std::string(reply);
  };
  for (unsigned i = 0; i < Store::PRINCIPAL_WRITE_BURST; ++i)
    assert(run(("!remember a value" + std::to_string(i)).c_str()) == "remembered a");
  const auto saved = *fs.files.at("/pine-notes");
  const unsigned opens = fs.writeOpens;
  assert(run("!remember a denied").find("principal write budget exhausted; retry in 15s") != std::string::npos);
  assert(run("!forget a").find("principal write budget exhausted") != std::string::npos);
  assert(run("!remember a value7").find("no change; write timestamp not committed") != std::string::npos);
  assert(run("!forget absent", third).find("no change; write timestamp not committed") != std::string::npos);
  assert(fs.writeOpens == opens && *fs.files.at("/pine-notes") == saved && notes.countPrincipals() == 1);
  assert(run("!recall a") == "a=value7" && run("!list") == "notes 1/4: a");
  uint8_t fourth[32] = {5};
  for (const auto* principal : {other, third, fourth})
    for (unsigned i = 0; i < Store::PRINCIPAL_WRITE_BURST; ++i)
      assert(run(("!remember a value" + std::to_string(i)).c_str(), principal) == "remembered a");
  const auto full = *fs.files.at("/pine-notes");
  const unsigned fullOpens = fs.writeOpens;
  assert(run("!remember e denied", third, UINT32_MAX).find("device write budget exhausted") != std::string::npos);
  clock.now += Store::WRITE_REFILL_MS - 1;
  assert(run("!remember e denied", third).find("retry in 1s; timestamp not committed") != std::string::npos);
  assert(fs.writeOpens == fullOpens && *fs.files.at("/pine-notes") == full);
  ++clock.now;
  assert(run("!remember a admitted") == "remembered a");
  assert(run("!remember e denied", third).find("device write budget exhausted") != std::string::npos);
  assert(run("!recall a") == "a=admitted");
  assert(notes.reset());
  assert(run("!remember reset cannot-refill", third).find("device write budget exhausted") != std::string::npos);

  Clock failingClock;
  NativeFilesystem failingFS;
  Store failing(failingFS, failingClock);
  assert(failing.begin());
  failingFS.writeBudget = 0;
  for (unsigned attempt = 0; attempt < Store::PRINCIPAL_WRITE_BURST; ++attempt) {
    assert(failing.command(bot, user, 1 + attempt, "!remember a failed", reply, sizeof(reply)));
    assert(strstr(reply, "commit failed"));
  }
  failingFS.writeBudget = -1;
  const unsigned failedOpens = failingFS.writeOpens;
  assert(failing.command(bot, user, 20, "!remember a retry", reply, sizeof(reply)));
  assert(strstr(reply, "principal write budget exhausted") && failingFS.writeOpens == failedOpens);
  assert(!failingFS.exists("/pine-notes") && failing.countNotes() == 0 && failing.countPrincipals() == 0);
  assert(failing.command(bot, other, 99, "!remember b independent", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered b"));  // A failed first writer does not own another sender's bucket.
  failingClock.now += Store::WRITE_REFILL_MS;
  assert(failing.command(bot, user, 2, "!remember a retry", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered a"));  // Failed attempts never committed a replay timestamp.

  Clock slotsClock;
  NativeFilesystem slotsFS;
  Store slots(slotsFS, slotsClock);
  assert(slots.begin());
  slotsFS.writeBudget = 0;
  uint8_t failedSender[32] = {};
  for (unsigned i = 0; i < Store::PRINCIPALS; ++i) {
    failedSender[0] = i + 10;
    assert(slots.command(bot, failedSender, 1, "!remember a failed", reply, sizeof(reply)));
    assert(strstr(reply, "commit failed"));
  }
  failedSender[0] = 99;
  const unsigned slotOpens = slotsFS.writeOpens;
  assert(slots.command(bot, failedSender, 1, "!remember a denied", reply, sizeof(reply)));
  assert(strstr(reply, "write principal slots busy") && slotsFS.writeOpens == slotOpens);
  slotsClock.now += Store::WRITE_REFILL_MS;
  slotsFS.writeBudget = -1;
  assert(slots.command(bot, failedSender, 1, "!remember a refilled", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered a"));

  Clock wrapClock;
  wrapClock.now = UINT32_MAX - 30;
  NativeFilesystem wrapFS;
  Store wrapping(wrapFS, wrapClock);
  assert(wrapping.begin());
  for (unsigned i = 0; i < Store::PRINCIPAL_WRITE_BURST - 1; ++i)
    assert(wrapping.command(bot, user, 100 + i,
        ("!remember a value" + std::to_string(i)).c_str(), reply, sizeof(reply)));
  assert(wrapping.command(bot, user, 107, "!forget a", reply, sizeof(reply)));
  assert(!strcmp(reply, "forgot a"));
  const auto deleted = *wrapFS.files.at("/pine-notes");
  const unsigned deletedOpens = wrapFS.writeOpens;
  assert(wrapping.command(bot, user, 200, "!forget a", reply, sizeof(reply)));
  assert(strstr(reply, "no change; write timestamp not committed"));
  assert(*wrapFS.files.at("/pine-notes") == deleted && wrapFS.writeOpens == deletedOpens);
  Store restarted(wrapFS, wrapClock);
  assert(restarted.begin());
  assert(restarted.command(bot, user, 100, "!remember a stale", reply, sizeof(reply)));
  assert(strstr(reply, "stale write"));
  wrapClock.now += Store::WRITE_REFILL_MS - 1;
  assert(wrapping.command(bot, user, 300, "!remember a denied", reply, sizeof(reply)));
  assert(strstr(reply, "principal write budget exhausted") && wrapFS.writeOpens == deletedOpens);
  ++wrapClock.now;
  assert(wrapping.command(bot, user, 150, "!remember a fresh", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered a"));  // A rejected no-op did not promise a durable fence at 200.
  assert(wrapping.command(bot, user, 151, "!forget a", reply, sizeof(reply)));
  assert(strstr(reply, "principal write budget exhausted"));
  std::puts("notes write budget: device/full-key limits, trusted monotonic refill/rollover, "
            "read admission, charged failures, explicit no-op rejection and durable replay fence");
}

struct Frames : BaseSerialInterface {
  bool enabled = true, secured = true, busy = false;
  std::deque<std::vector<uint8_t>> incoming;
  std::vector<std::vector<uint8_t>> outgoing;
  void enable() override { enabled = true; }
  void disable() override { enabled = false; }
  bool isEnabled() const override { return enabled; }
  bool isConnected() const override { return enabled && secured; }
  bool isWriteBusy() const override { return busy; }
  size_t writeFrame(const uint8_t* bytes, size_t length) override {
    assert(length <= MAX_FRAME_SIZE);
    outgoing.emplace_back(bytes, bytes + length);
    return length;
  }
  size_t checkRecvFrame(uint8_t* bytes) override {
    if (incoming.empty()) return 0;
    auto frame = incoming.front(); incoming.pop_front();
    memcpy(bytes, frame.data(), frame.size());
    return frame.size();
  }
};

static void testCompanion() {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical;
  SimpleMeshTables tables;
  StaticPoolPacketManager pool(8);
  NativeFilesystem fs;
  auto relay = mesh::LocalIdentity(&rng);
  nrfmast::CommandBot bot(physical, clock, rng, rtc, pool, tables);
  bot.self_id = mesh::LocalIdentity(&rng);
  bot.begin();
  IdentityStore store(fs, "");
  assert(store.save("_main", relay) && store.save("_nrfbot", bot.self_id));
  nrfmast::RuntimeConfig config(fs, relay, bot);
  assert(config.begin() && !config.ble.isEnabled());
  assert(!config.ble.setEnabled(true) && !config.ble.setPin(123));
  assert(config.ble.setPin(234567) && config.ble.setEnabled(true));
  nrfmast::BleConfig reread(fs);
  assert(reread.begin() && reread.isEnabled() && reread.getPin() == 234567);
  const auto before = *fs.files.at("/pine-ble");
  fs.failRename = true;
  assert(!config.ble.setPin(345678) && *fs.files.at("/pine-ble") == before);
  fs.failRename = false;
  Frames frames;
  nrfmast::CompanionInterface companion(bot, config, frames, [](nrfmast::CompanionRadioInfo& info) {
    info = {};
    info.frequency = 912.525f;
    info.bandwidth = 250;
    info.sf = 7;
    info.cr = 5;
    info.power = 2;
    info.maxPower = 22;
    info.airtimeFactor = 1;
  });
  bot.setCompanion(companion);
  auto command = [&](std::vector<uint8_t> frame) {
    frames.incoming.push_back(frame);
    const size_t previous = frames.outgoing.size();
    companion.loop();
    assert(frames.outgoing.size() == previous + 1);
    return frames.outgoing.back();
  };
  frames.secured = false;
  frames.incoming.push_back({CMD_DEVICE_QUERY, 3});
  companion.loop();
  assert(frames.outgoing.empty() && frames.incoming.size() == 1);
  frames.secured = true;
  companion.loop();
  assert(frames.outgoing.back()[0] == RESP_CODE_DEVICE_INFO && frames.outgoing.back().size() == 82);
  assert(!strcmp(reinterpret_cast<const char*>(frames.outgoing.back().data() + 60), MESHCORE_SLP_PINE_VERSION));
  auto self = command({CMD_APP_START, 0, 0, 0, 0, 0, 0, 0});
  assert(self[0] == RESP_CODE_SELF_INFO && !memcmp(self.data() + 4, bot.self_id.pub_key, 32));
  assert(self[2] == 2 && self[56] == 7 && self[57] == 5);
  auto name = std::vector<uint8_t>{CMD_SET_ADVERT_NAME};
  for (char c : std::string("Pine-Bot")) name.push_back(c);
  assert(command(name)[0] == RESP_CODE_OK && !strcmp(bot.getName(), "Pine-Bot"));
  assert(command({CMD_SET_ADVERT_NAME, ' ', 'x'}) ==
         (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
  assert(command({CMD_SET_RADIO_PARAMS}) == std::vector<uint8_t>{RESP_CODE_DISABLED});
  for (auto code : {CMD_SET_RADIO_TX_POWER, CMD_IMPORT_PRIVATE_KEY, CMD_EXPORT_PRIVATE_KEY,
                    CMD_FACTORY_RESET, CMD_SET_CHANNEL, CMD_SIGN_START})
    assert(command({uint8_t(code)})[0] == RESP_CODE_DISABLED);
  for (auto code : {CMD_SET_DEVICE_TIME, CMD_SEND_TXT_MSG, CMD_SEND_LOGIN, CMD_ADD_UPDATE_CONTACT,
                    CMD_GET_CONTACT_BY_KEY, CMD_RESET_PATH, CMD_SET_DEVICE_PIN, CMD_APP_START})
    assert(command({uint8_t(code)}) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
  const auto originalTime = rtc.now;
  const uint32_t utc = 1800000000;
  std::vector<uint8_t> setTime(5);
  setTime[0] = CMD_SET_DEVICE_TIME;
  memcpy(setTime.data() + 1, &utc, 4);
  frames.secured = false;
  frames.incoming.push_back(setTime);
  companion.loop();
  assert(rtc.now == originalTime && frames.incoming.size() == 1);
  frames.secured = true;
  companion.loop();
  assert(rtc.now == utc && frames.outgoing.back() == std::vector<uint8_t>{RESP_CODE_OK});
  assert(command(setTime) == std::vector<uint8_t>{RESP_CODE_OK});
  const uint32_t backwardsUtc = utc - 1;
  memcpy(setTime.data() + 1, &backwardsUtc, 4);
  assert(command(setTime) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
  assert(rtc.now == utc);
  const uint32_t invalidUtc = 4102444801u;
  memcpy(setTime.data() + 1, &invalidUtc, 4);
  assert(command(setTime) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
  assert(rtc.now == utc);
  rtc.now = originalTime;
  assert(command({255}) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_UNSUPPORTED_CMD}));
  ContactInfo contact = {};
  contact.id = mesh::LocalIdentity(&rng);
  contact.type = ADV_TYPE_CHAT;
  contact.out_path_len = OUT_PATH_UNKNOWN;
  contact.lastmod = rtc.now;
  strcpy(contact.name, "Companion");
  assert(bot.addContact(contact));
  assert(command({CMD_GET_CONTACTS})[0] == RESP_CODE_CONTACTS_START);
  companion.loop();
  assert(frames.outgoing.back()[0] == RESP_CODE_CONTACT && frames.outgoing.back().size() == 148);
  companion.loop();
  assert(frames.outgoing.back()[0] == RESP_CODE_END_OF_CONTACTS);
  mesh::Packet packet;
  packet.header = ROUTE_TYPE_FLOOD;
  packet.path_len = 0;
  packet.payload_len = 0;
  packet._snr = -9;
  companion.received(contact, packet, rtc.now, TXT_TYPE_PLAIN, "ordinary private message");
  auto message = command({CMD_SYNC_NEXT_MESSAGE});
  assert(message[0] == RESP_CODE_CONTACT_MSG_RECV_V3 && message[1] == uint8_t(-9));
  assert(!memcmp(message.data() + 4, contact.id.pub_key, 6));
  assert(command({CMD_SYNC_NEXT_MESSAGE})[0] == RESP_CODE_NO_MORE_MESSAGES);
  for (int i = 0; i < 5; ++i) companion.received(contact, packet, rtc.now, 0, "bounded inbox");
  assert(companion.inboxDrops == 1);
  assert(command({CMD_GET_CUSTOM_VARS})[0] == RESP_CODE_CUSTOM_VARS);
  assert(command({CMD_GET_TUNING_PARAMS}).size() == 9);
  for (const auto& selector : {std::pair<uint8_t, size_t>{0, 11}, {1, 14}, {2, 30}})
    assert(command({CMD_GET_STATS, selector.first}).size() == selector.second);
  auto exported = command({CMD_EXPORT_CONTACT});
  assert(exported[0] == RESP_CODE_EXPORT_CONTACT);
  mesh::Packet advert;
  assert(advert.readFrom(exported.data() + 1, exported.size() - 1));
  assert(!memcmp(advert.payload, bot.self_id.pub_key, PUB_KEY_SIZE));
  SimpleMeshTables importTables;
  StaticPoolPacketManager importPool(8);
  nrfmast::CommandBot source(physical, clock, rng, rtc, importPool, importTables);
  uint8_t importedKeys[2][PUB_KEY_SIZE];
  std::vector<uint8_t> imports[2];
  for (size_t i = 0; i < 2; ++i) {
    source.self_id = mesh::LocalIdentity(&rng);
    memcpy(importedKeys[i], source.self_id.pub_key, PUB_KEY_SIZE);
    auto* signedAdvert = source.createSelfAdvert(i ? "import-two" : "import-one");
    assert(signedAdvert);
    signedAdvert->header |= ROUTE_TYPE_FLOOD;
    imports[i].resize(MAX_TRANS_UNIT + 1);
    imports[i][0] = CMD_IMPORT_CONTACT;
    imports[i].resize(signedAdvert->writeTo(imports[i].data() + 1) + 1);
    source.releasePacket(signedAdvert);
  }
  assert(pool.getFreeCount() == 8);
  assert(command(imports[0]) == (std::vector<uint8_t>{RESP_CODE_OK}));
  assert(bot.contactImportPending() && pool.getFreeCount() == 7);
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    assert(command(imports[1]) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_BAD_STATE}));
    assert(pool.getFreeCount() == 7 && bot.contactImportPending());
  }
  assert(!bot.lookupContactByPubKey(importedKeys[0], PUB_KEY_SIZE));
  assert(!bot.lookupContactByPubKey(importedKeys[1], PUB_KEY_SIZE));
  bot.loop();
  assert(!bot.contactImportPending() && pool.getFreeCount() == 8);
  assert(bot.lookupContactByPubKey(importedKeys[0], PUB_KEY_SIZE));
  assert(!bot.lookupContactByPubKey(importedKeys[1], PUB_KEY_SIZE));
  assert(command(imports[1]) == (std::vector<uint8_t>{RESP_CODE_OK}));
  bot.loop();
  assert(!bot.contactImportPending() && pool.getFreeCount() == 8);
  assert(bot.lookupContactByPubKey(importedKeys[1], PUB_KEY_SIZE));
  std::puts("companion imports: back-to-back without bot.loop rejected; first/next signed contact retained; "
            "8-slot packet pool fully recovered");
  ContactInfo collision = contact;
  collision.id.pub_key[31] ^= 1;
  assert(bot.addContact(collision));
  std::vector<uint8_t> ambiguous(15, 0);
  ambiguous[0] = CMD_SEND_TXT_MSG;
  ambiguous[1] = TXT_TYPE_PLAIN;
  memcpy(ambiguous.data() + 7, contact.id.pub_key, 6);
  ambiguous[13] = 'h';
  ambiguous[14] = 'i';
  assert(command(ambiguous) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_NOT_FOUND}));
  auto pin = std::vector<uint8_t>(5);
  pin[0] = CMD_SET_DEVICE_PIN;
  const uint32_t newPin = 345678;
  memcpy(pin.data() + 1, &newPin, 4);
  assert(command(pin)[0] == RESP_CODE_OK && config.ble.getPin() == newPin);
  fs.files.at("/pine-ble")->resize(3);
  assert(config.begin() && !config.ble.isEnabled() && !config.ble.getPin());
  assert(config.ble.setPin(234567) && config.ble.setEnabled(true));
  mesh::LocalIdentity retained;
  assert(store.load("_main", retained) && retained.matches(relay));
  assert(store.load("_nrfbot", retained) && retained.matches(bot.self_id));
  std::puts("companion: secured gate, v13 frame layouts, saved name/PIN, bounded inbox and denied RF/key authority");
}

static void testRuntimeConfig() {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical;
  SimpleMeshTables tables;
  StaticPoolPacketManager pool(8);
  NativeFilesystem fs;
  IdentityStore store(fs, "");
  auto repeater = mesh::LocalIdentity(&rng);
  nrfmast::CommandBot bot(physical, clock, rng, rtc, pool, tables);
  bot.self_id = mesh::LocalIdentity(&rng);
  const auto originalBot = bot.self_id;
  assert(store.save("_main", repeater) && store.save("_nrfbot", bot.self_id));
  nrfmast::RuntimeConfig config(fs, repeater, bot);
  assert(config.begin());
  char reply[160] = {};
  auto command = [&](const std::string& text, uint32_t timestamp = 0) {
    memset(reply, 0, sizeof(reply));
    assert(config.handleCommand(timestamp, text.c_str(), reply));
    assert(strlen(reply) < 157);
    return std::string(reply);
  };
  assert(command("get capabilities").find("channel-keys=unsupported") != std::string::npos);
  assert(command("ver").find("v" MESHCORE_SLP_PINE_VERSION) == 0);
  assert(command("help").find("help bot|source|wifi") == 0);
  assert(command("help wifi").find("slp-pine has no WiFi") != std::string::npos);
  assert(command("get bot.time.provider").find("provider=off fault=0") == 0);
  auto provider = mesh::LocalIdentity(&rng);
  char providerHex[65];
  mesh::Utils::toHex(providerHex, provider.pub_key, 32);
  assert(command(std::string("set bot.time.provider ") + providerHex, 1800000000).find("OK saved/live") == 0);
  assert(bot.getTimeProvider() && !memcmp(bot.getTimeProvider(), provider.pub_key, 32));
  assert(fs.files.at("/pine-time")->size() == 36);
  bot.setTimeProvider(nullptr);
  assert(config.begin() && bot.getTimeProvider() && !memcmp(bot.getTimeProvider(), provider.pub_key, 32));
  const auto savedProvider = *fs.files.at("/pine-time");
  fs.failRename = true;
  assert(command("set bot.time.provider off").find("Error:") == 0);
  assert(*fs.files.at("/pine-time") == savedProvider && bot.getTimeProvider());
  fs.failRename = false;
  fs.writeBudget = 10;
  assert(command("set bot.time.provider off").find("Error:") == 0);
  assert(*fs.files.at("/pine-time") == savedProvider);
  fs.writeBudget = -1;
  assert(command("set bot.time.provider " + std::string(64, '0')).find("Error:") == 0);
  assert(command("set bot.time.provider off").find("OK saved/live") == 0);
  assert(!bot.getTimeProvider());
  fs.files.at("/pine-time")->resize(5);
  assert(config.begin() && !bot.getTimeProvider());
  assert(command("get bot.time.provider").find("fault=1") != std::string::npos);
  assert(command("set bot.time.provider off").find("OK saved/live") == 0);
  assert(command("set wifi.ssid example").find("slp-pine has no WiFi") != std::string::npos);
  assert(command("bot adaptive").find("saved=0 live=0") != std::string::npos);
  assert(command("bot adaptive").find("last=allowed") != std::string::npos);
  assert(command("bot adaptive on") == "Saved adaptive admission; reboot required");
  assert(command("bot adaptive").find("saved=1 live=0") != std::string::npos);
  assert(config.begin() && bot.adaptiveEnabled());
  assert(command("bot adaptive everyone").find("Error:") == 0);
  fs.failRename = true;
  assert(command("bot adaptive off").find("Error:") == 0);
  fs.failRename = false;
  assert(command("bot adaptive").find("saved=1 live=1") != std::string::npos);
  assert(command("bot adaptive off") == "Saved adaptive admission; reboot required");
  assert(config.begin() && !bot.adaptiveEnabled());
  for (const auto& bad : std::vector<std::vector<uint8_t>>{
         {'B', 'A', 'A', 1, 2}, {'B', 'A', 'D', 1, 0}, {'B', 'A', 'A', 1}}) {
    *fs.files.at("/pine-adapt") = bad;
    assert(config.begin() && !bot.adaptiveEnabled());
    assert(command("bot adaptive").find("live=0 policy-fault=1") != std::string::npos);
    *fs.files.at("/pine-adapt") = {'B', 'A', 'A', 1, 0};
    assert(command("bot adaptive").find("fault retained") != std::string::npos);
    *fs.files.at("/pine-adapt") = bad;
    fs.failRename = true;
    assert(command("bot adaptive off").find("Error:") == 0);
    assert(command("bot adaptive").find("policy-fault=1") != std::string::npos);
    fs.failRename = false;
    assert(command("bot adaptive off") == "Saved adaptive admission; reboot required");
    assert(command("bot adaptive").find("saved=0 live=0") != std::string::npos);
    assert(config.begin() && !bot.adaptiveEnabled());
  }
  assert(command("set bot.name Pine-Bot", 1800000000).find("OK saved") == 0);
  assert(!strcmp(bot.getName(), "Pine-Bot") && bot.self_id.matches(originalBot));
  assert(command("get bot.name") == "> Pine-Bot");
  for (auto bad : {"", " space", "space ", "line\nbreak", "12345678901234567890123456789012"})
    assert(command(std::string("set bot.name ") + bad).find("Error:") == 0);
  for (auto unsupported : {"set bot.channel secret", "get bot.radio",
                           "get prv.key", "get bot.prv.key"})
    assert(command(unsupported).find("Error:") == 0);
  auto before = *fs.files.at("/_nrfbot.id");
  fs.writeBudget = 90;
  assert(command("set bot.name Broken").find("Error:") == 0);
  assert(*fs.files.at("/_nrfbot.id") == before && !strcmp(bot.getName(), "Pine-Bot"));
  fs.writeBudget = -1;
  fs.failRename = true;
  assert(command("set bot.name Broken").find("Error:") == 0);
  assert(*fs.files.at("/_nrfbot.id") == before);
  fs.failRename = false;
  fs.failOpen = true;
  assert(command("set bot.name Broken").find("Error:") == 0);
  fs.failOpen = false;
  assert(command("get bot.pub.key").find("reboot=0") != std::string::npos);

  auto replacement = mesh::LocalIdentity(&rng);
  auto hexKey = [](mesh::LocalIdentity identity) {
    uint8_t bytes[PRV_KEY_SIZE];
    char hex[PRV_KEY_SIZE * 2 + 1];
    assert(identity.writeTo(bytes, sizeof(bytes)) == sizeof(bytes));
    mesh::Utils::toHex(hex, bytes, sizeof(bytes));
    return std::string(hex);
  };
  assert(command("set bot.prv.key " + hexKey(replacement), 1800000000).find("Error:") == 0);
  assert(*fs.files.at("/_nrfbot.id") == before);
  assert(command("set bot.prv.key " + hexKey(repeater)).find("distinct identities") != std::string::npos);
  assert(command("set bot.prv.key " + std::string(128, 'z')).find("Error:") == 0);
  assert(command("set bot.prv.key " + hexKey(replacement) + "0").find("Error:") == 0);
  assert(command("set bot.prv.key " + hexKey(replacement)).find("OK saved; reboot required") == 0);
  assert(bot.self_id.matches(originalBot));
  assert(command("get bot.pub.key").find("reboot=1") != std::string::npos);
  // A later name edit must preserve a deliberately staged replacement identity.
  assert(command("set bot.name Pine-Bot-Next").find("OK saved") == 0);
  mesh::LocalIdentity restarted;
  assert(store.load("_nrfbot", restarted) && restarted.matches(replacement));
  bot.self_id = restarted;
  nrfmast::RuntimeConfig rebooted(fs, repeater, bot);
  assert(rebooted.begin() && !strcmp(bot.getName(), "Pine-Bot-Next"));
  assert(command("get bot.pub.key").find("reboot=0") != std::string::npos);
  assert(command("set prv.key " + hexKey(replacement)).find("distinct identities") != std::string::npos);
  auto newRepeater = mesh::LocalIdentity(&rng);
  assert(command("set prv.key " + hexKey(newRepeater), 1800000000).find("Error:") == 0);
  assert(command("set prv.key " + hexKey(newRepeater)).find("OK saved; reboot required") == 0);
  assert(command("get pub.key").find("reboot=1") != std::string::npos);
  assert(store.load("_main", restarted) && restarted.matches(newRepeater));
  repeater = restarted;
  assert(command("get pub.key").find("reboot=0") != std::string::npos);
  fs.files.at("/_nrfbot.id")->resize(100);
  assert(!rebooted.begin());
  assert(!config.handleCommand(0, "get radio", reply));
  std::puts("runtime config: durable names, USB-only staged fixture keys, restart, atomic failures and capability limits");
}

class NativePeer : public BaseChatMesh {
  uint32_t expectedAck = 0;
  uint8_t recipient[PUB_KEY_SIZE] = {};
  uint8_t hashBytes;
  const TransportKey* sendScope;

protected:
  int calcRxDelay(float, uint32_t) const override { return 0; }
  void sendFloodScoped(const ContactInfo&, mesh::Packet* packet, uint32_t delay) override {
    if (sendScope) {
      uint16_t codes[] = {sendScope->calcTransportCode(packet), 0};
      sendFlood(packet, codes, delay, hashBytes);
    } else {
      sendFlood(packet, delay, hashBytes);
    }
  }
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
  ContactInfo* processAck(const uint8_t* bytes) override {
    if (!expectedAck || memcmp(bytes, &expectedAck, sizeof(expectedAck))) return nullptr;
    expectedAck = 0;
    ++acks;
    return lookupContactByPubKey(recipient, sizeof(recipient));
  }
  void onContactPathUpdated(const ContactInfo&) override { ++pathUpdates; }
  void onMessageRecv(const ContactInfo& from, mesh::Packet* packet, uint32_t, const char* text) override {
    assert(from.id.matches(recipient));
    messages.emplace_back(text);
    messageRoutes.push_back(packet->getRouteType());
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t) const override { return 15000; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t, uint8_t) const override { return 15000; }
  void onSendTimeout() override { ++timeouts; }
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}

public:
  std::vector<std::string> messages;
  std::vector<uint8_t> messageRoutes;
  unsigned acks = 0, pathUpdates = 0, timeouts = 0;
  NativePeer(mesh::Radio& radio, mesh::MillisecondClock& clock, mesh::RNG& rng,
             mesh::RTCClock& rtc, mesh::PacketManager& pool, mesh::MeshTables& tables, uint8_t width,
             const TransportKey* scope = nullptr)
      : BaseChatMesh(radio, clock, rng, rtc, pool, tables), hashBytes(width), sendScope(scope) {}
  int command(const ContactInfo& bot, const char* text) {
    memcpy(recipient, bot.id.pub_key, sizeof(recipient));
    uint32_t timeout;
    return sendMessage(bot, getRTCClock()->getCurrentTimeUnique(), 0, text, expectedAck, timeout);
  }
};

class NativeTimeProvider : public NativePeer {
  void onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                      uint8_t *data, size_t length) override {
    assert(packet->isRouteDirect() && packet->getPathHashCount() == 0);
    assert(radio_time::request(data, length));
    ++requests;
    memcpy(requester, sender.pub_key, 32);
    uint8_t reply[32];
    radio_time::response(data, reply, 1, 1900000000, 1900000001, 0, 3600000);
    auto *response = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply, sizeof(reply));
    assert(response);
    sendZeroHop(response);
  }
public:
  using NativePeer::NativePeer;
  unsigned requests = 0;
  uint8_t requester[32]{};
};

static void testRadioTime() {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical clientRadio, providerRadio;
  SimpleMeshTables clientTables, providerTables;
  StaticPoolPacketManager clientPool(8), providerPool(8);
  nrfmast::CommandBot bot(clientRadio, clock, rng, rtc, clientPool, clientTables);
  NativeTimeProvider provider(providerRadio, clock, rng, rtc, providerPool, providerTables, 1);
  bot.self_id = mesh::LocalIdentity(&rng);
  provider.self_id = mesh::LocalIdentity(&rng);
  bot.begin(false);
  provider.begin();
  ContactInfo contact{};
  contact.id = provider.self_id; contact.type = ADV_TYPE_REPEATER;
  contact.out_path_len = OUT_PATH_UNKNOWN;
  assert(bot.addContact(contact));
  bot.setTimeProvider(provider.self_id.pub_key);
  assert(!bot.fetchTime());
  auto *learned = bot.lookupContactByPubKey(provider.self_id.pub_key, 32);
  learned->out_path_len = 0;
  clientRadio.airtime = 1001;
  assert(!bot.fetchTime());
  clientRadio.airtime = 40;
  assert(bot.fetchTime() && !bot.fetchTime());
  unsigned copied = 0;
  for (unsigned i = 0; i < 100; ++i) {
    clock.now += 10;
    bot.loop(); provider.loop();
    while (copied < clientRadio.sent.size()) providerRadio.input.push_back(clientRadio.sent[copied++]);
  }
  assert(provider.requests == 1 && !memcmp(provider.requester, bot.self_id.pub_key, 32));
  assert(providerRadio.sent.size() == 1 && rtc.now == 1800000000);
  auto corrupt = providerRadio.sent.front();
  corrupt.back() ^= 1;
  clientRadio.input.push_back(corrupt);
  for (unsigned i = 0; i < 20; ++i) { clock.now += 10; bot.loop(); }
  assert(rtc.now == 1800000000);
  clientRadio.input.push_back(providerRadio.sent.front());
  for (unsigned i = 0; i < 20; ++i) { clock.now += 10; bot.loop(); }
  assert(rtc.now == 1900000000);
  const auto packets = clientRadio.sent.size();
  clock.now += 10000;
  for (unsigned i = 0; i < 20; ++i) { clock.now += 10; bot.loop(); }
  assert(clientRadio.sent.size() == packets); // no uncertain request replay
  bot.setTimeProvider(provider.self_id.pub_key);
  assert(bot.fetchTime() && clientPool.getOutboundTotal() == 1);
  clock.now += radio_time::DeadlineMs;
  bot.loop();
  assert(clientPool.getOutboundTotal() == 0 && clientPool.getFreeCount() == 8 &&
         clientRadio.sent.size() == packets);
  clock.now += 10000;
  assert(bot.fetchTime() && clientPool.getOutboundTotal() == 1);
  bot.setTimeProvider(nullptr);
  assert(!bot.fetchTime() && clientPool.getOutboundTotal() == 0 && clientPool.getFreeCount() == 8);
  puts("RF time: native full-identity anonymous encryption, direct path, tampered-MAC denial, correlated UTC, airtime/deadline cap, queued cancellation and no replay");
}

class NativeRelay : public mesh::Mesh {
protected:
  bool allowPacketForward(const mesh::Packet*) override { return true; }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  uint32_t getRetransmitDelay(const mesh::Packet*) override { return 0; }
  uint32_t getDirectRetransmitDelay(const mesh::Packet*) override { return 0; }
public:
  NativeRelay(mesh::Radio& radio, mesh::MillisecondClock& clock, mesh::RNG& rng,
              mesh::RTCClock& rtc, mesh::PacketManager& pool, mesh::MeshTables& tables)
      : mesh::Mesh(radio, clock, rng, rtc, pool, tables) {}
};

static void testNativePeerRouting(uint8_t hashBytes, bool scoped = false) {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical botRadio, peerRadio, relayRadio;
  nrfmast::SharedRadio shared(botRadio, clock, true);
  SimpleMeshTables botTables, peerTables, relayTables;
  StaticPoolPacketManager botPool(8), peerPool(16), relayPool(32);
  TransportKey scope;
  rng.random(scope.key, sizeof(scope.key));
  nrfmast::CommandBot bot(shared.bot, clock, rng, rtc, botPool, botTables);
  NativeFilesystem fs;
  nrfmast::NoteStore notes(fs, clock);
  assert(notes.begin());
  bot.setNotes(notes);
  NativePeer peer(peerRadio, clock, rng, rtc, peerPool, peerTables, hashBytes, scoped ? &scope : nullptr);
  NativeRelay relay(relayRadio, clock, rng, rtc, relayPool, relayTables);
  bot.self_id = mesh::LocalIdentity(&rng);
  peer.self_id = mesh::LocalIdentity(&rng);
  relay.self_id = mesh::LocalIdentity(&rng);
  assert(bot.setName("Runtime Native Bot"));
  bot.begin(true); peer.begin(); relay.begin();
  size_t botSent = 0, peerSent = 0, relaySent = 0;
  unsigned scopedCommands = 0;
  auto run = [&](uint32_t duration) {
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += 20) {
      shared.poll();
      bot.loop();
      peer.loop();
      relay.loop();
      uint8_t discard[MAX_TRANS_UNIT];
      shared.repeater.recvRaw(discard, sizeof(discard));
      // There is no direct peer/bot RF link: all delivery must use native routing.
      while (botSent < botRadio.sent.size()) relayRadio.input.push_back(botRadio.sent[botSent++]);
      while (peerSent < peerRadio.sent.size()) {
        const auto& raw = peerRadio.sent[peerSent++];
        mesh::Packet packet;
        assert(packet.readFrom(raw.data(), raw.size()));
        if (scoped && packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG && packet.isRouteFlood()) {
          assert(packet.hasTransportCodes());
          assert(packet.transport_codes[0] == scope.calcTransportCode(&packet));
          ++scopedCommands;
        }
        relayRadio.input.push_back(raw);
      }
      while (relaySent < relayRadio.sent.size()) {
        peerRadio.input.push_back(relayRadio.sent[relaySent]);
        botRadio.input.push_back(relayRadio.sent[relaySent++]);
      }
      clock.now += 20;
    }
  };

  auto advert = peer.createSelfAdvert("native-community-peer");
  assert(advert);
  peer.sendFlood(advert, 0, hashBytes);
  run(24000);
  auto* botContact = peer.lookupContactByPubKey(bot.self_id.pub_key, PUB_KEY_SIZE);
  assert(botContact && botContact->type == ADV_TYPE_CHAT);
  assert(!strcmp(botContact->name, "Runtime Native Bot"));
  assert(bot.getNumContacts() == 1);
  assert(botContact->out_path_len == OUT_PATH_UNKNOWN);
  assert(peer.command(*botContact, "!ping") == MSG_SEND_SENT_FLOOD);
  run(5000);
  assert(peer.messages.size() == 1 && peer.messages[0].find("pong uptime_s=") == 0);
  assert(peer.messageRoutes[0] == ROUTE_TYPE_FLOOD);
  assert(scopedCommands == (scoped ? 1u : 0u));
  assert(peer.acks == 1 && peer.pathUpdates > 0);
  assert((botContact->out_path_len & 63) == 1);
  assert((botContact->out_path_len >> 6) + 1 == hashBytes);
  assert(memcmp(botContact->out_path, relay.self_id.pub_key, hashBytes) == 0);
  auto* peerContact = bot.lookupContactByPubKey(peer.self_id.pub_key, PUB_KEY_SIZE);
  assert(peerContact && (peerContact->out_path_len & 63) == 1);
  assert(memcmp(peerContact->out_path, relay.self_id.pub_key, 1) == 0);

  assert(!bot.txBusy());
  assert(peer.command(*botContact, "!signal") == MSG_SEND_SENT_DIRECT);
  run(5000);
  assert(peer.messages.size() == 2 && peer.acks == 2);
  assert(peer.messageRoutes[1] == ROUTE_TYPE_DIRECT);
  assert(peer.messages[1].find("snr=-2.25dB rssi=-87dBm") == 0);
  assert(peer.messages[1].find("direct_remaining=0 end_to_end_hops=unknown") != std::string::npos);
  assert(bot.replies == 2 && bot.timeouts == 0 && peer.timeouts == 0);
  if (hashBytes == 3) {
    assert(!bot.txBusy());
    botContact->out_path_len = OUT_PATH_UNKNOWN;
    assert(peer.command(*botContact, "!remember antenna spare in box") == MSG_SEND_SENT_FLOOD);
    run(5000);
    assert(peer.messages.back() == "remembered antenna");
    assert(!bot.txBusy());
    assert(peer.command(*botContact, "!recall antenna") == MSG_SEND_SENT_DIRECT);
    run(5000);
    assert(peer.messages.back() == "antenna=spare in box");
    nrfmast::NoteStore restarted(fs, clock);
    assert(restarted.begin());
    char reply[161];
    assert(restarted.command(bot.self_id.pub_key, peer.self_id.pub_key, rtc.now,
                              "!recall antenna", reply, sizeof(reply)));
    assert(!strcmp(reply, "antenna=spare in box"));
    uint32_t timeout;
    assert(peer.sendCommandData(*botContact, rtc.now + 20, 0, "!forget antenna", timeout) != MSG_SEND_FAILED);
    run(5000);
    assert(restarted.command(bot.self_id.pub_key, peer.self_id.pub_key, rtc.now,
                              "!recall antenna", reply, sizeof(reply)));
    assert(!strcmp(reply, "antenna=spare in box"));
    std::puts("notes RF: encrypted DMs and replies through native relay; durable readback; CLI-data cannot invoke notes");
  }
  std::printf("native interop: %s peer discovery, flood-to-direct path learning, reciprocal paths and ACKs through relay (%u-byte hashes)\n",
              scoped ? "scoped" : "unscoped", hashBytes);
}

static void testStartupAdvert(uint32_t start, bool enabled, bool exhaustPool = false) {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  SimpleMeshTables tables;
  StaticPoolPacketManager pool(8);
  nrfmast::CommandBot bot(shared.bot, clock, rng, rtc, pool, tables);
  bot.self_id = mesh::LocalIdentity(&rng);
  // Construct the native dispatcher at boot, before advancing near rollover.
  clock.now = start;
  bot.begin(enabled);
  mesh::Packet* held[8] = {};
  if (exhaustPool) {
    for (auto& packet : held) { packet = pool.allocNew(); assert(packet); }
  }
  auto tick = [&]() { shared.poll(); bot.loop(); };
  clock.now = start + 19999;
  tick();
  assert(pool.getOutboundTotal() == 0 && physical.sent.empty());
  clock.now = start + 20000;
  tick();
  unsigned expected = enabled && !exhaustPool ? 1 : 0;
  assert(pool.getOutboundTotal() == int(expected));
  clock.now += 1;
  tick();
  clock.now += 40;
  tick();
  assert(physical.sent.size() == expected);
  if (exhaustPool) {
    assert(bot.failures == 1);
    for (auto* packet : held) pool.free(packet);
  }
  // Simulated elapsed time, not an on-device soak: there is no periodic retry.
  for (uint32_t elapsed : {300000u, 600000u, 3600000u, 86400000u}) {
    clock.now = start + elapsed;
    tick();
    clock.now += 41;
    tick();
    assert(physical.sent.size() == expected && pool.getOutboundTotal() == 0);
  }
  assert(bot.advertise()); // Explicit late operator discovery still works.
  clock.now += 1;
  tick();
  clock.now += 40;
  tick();
  assert(physical.sent.size() == expected + 1);
  mesh::Packet advert;
  const auto& raw = physical.sent.back();
  assert(advert.readFrom(raw.data(), raw.size()));
  assert(advert.getPathHashSize() == 3);
  assert(advert.isRouteFlood() && advert.getPayloadType() == PAYLOAD_TYPE_ADVERT);
  assert(memcmp(advert.payload, bot.self_id.pub_key, PUB_KEY_SIZE) == 0);
  std::puts("native: one startup advert at 20s, no periodic adverts/retries, operator discovery retained");
}

static void testOperatorAdvertPreemptsStartup() {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  SimpleMeshTables tables;
  StaticPoolPacketManager pool(8);
  nrfmast::CommandBot bot(shared.bot, clock, rng, rtc, pool, tables);
  bot.self_id = mesh::LocalIdentity(&rng);
  bot.begin(true);
  assert(bot.setName("Pine-Bot"));
  assert(bot.advertise(true));
  for (int i = 0; i < 10; ++i) {
    shared.poll(); bot.loop(); clock.now += 40;
  }
  assert(physical.sent.size() == 1);
  mesh::Packet advert;
  assert(advert.readFrom(physical.sent[0].data(), physical.sent[0].size()));
  assert(advert.getRouteType() == ROUTE_TYPE_DIRECT && advert.getPathHashCount() == 0);
  assert(advert.getPayloadType() == PAYLOAD_TYPE_ADVERT);
  clock.now += 30000;
  shared.poll(); bot.loop();
  assert(physical.sent.size() == 1 && pool.getOutboundTotal() == 0);
}

static void testSharing() {
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  shared.repeater.begin(); shared.bot.begin();
  assert(physical.beginCount == 1);
  uint8_t bytes[] = {1, 0, 7};
  physical.input.emplace_back(bytes, bytes + sizeof(bytes));
  shared.poll();
  physical.rssi = -111; physical.snr = 4;
  physical.input.emplace_back(bytes, bytes + sizeof(bytes));
  shared.poll();
  physical.input.emplace_back(bytes, bytes + sizeof(bytes));
  shared.poll();
  assert(shared.repeater.rxDrops == 1 && shared.bot.rxDrops == 1);
  assert(shared.receivedAirtimeMs() == 120); // Three physical frames, not six role copies.
  uint8_t out[MAX_TRANS_UNIT];
  assert(shared.bot.recvRaw(out, sizeof(out)) == 3);
  assert(shared.bot.getLastRSSI() == -87 && shared.bot.getLastSNR() == -2.25);
  assert(shared.repeater.recvRaw(out, sizeof(out)) == 3);
  assert(shared.bot.recvRaw(out, sizeof(out)) == 3 && shared.bot.getLastRSSI() == -111);
  assert(shared.repeater.recvRaw(out, 1) == 0 && shared.repeater.rxDrops == 2);
  assert(shared.repeater.startSendRaw(bytes, sizeof(bytes)));
  int reads = physical.reads;
  shared.poll();
  assert(physical.reads == reads);
  assert(!shared.botCanLoop() && shared.repeaterCanLoop());
  assert(shared.bot.isReceiving());
  assert(!shared.bot.startSendRaw(bytes, sizeof(bytes)));
  assert(!shared.bot.isSendComplete());
  shared.bot.onSendFinished();
  assert(physical.finishes == 0);
  assert(shared.repeater.isSendComplete());
  assert(shared.repeater.isSendComplete());
  clock.now += 40;
  shared.repeater.onSendFinished();
  assert(physical.finishes == 1);
  assert(shared.aggregateRfMs() == 40 && shared.lastTransmitMs() == 40);
  assert(shared.bot.isReceiving()); // One shared off-air allowance, not one per role.
  clock.now += 40;
  assert(!shared.bot.isReceiving());
  assert(shared.bot.startSendRaw(bytes, sizeof(bytes)));
  assert(shared.bot.isSendComplete());
  clock.now += 40;
  shared.bot.onSendFinished();
  assert(shared.aggregateRfMs() == 80 && shared.lastTransmitMs() == 40);
  clock.now += 40;
  physical.failStart = true;
  assert(!shared.repeater.startSendRaw(bytes, sizeof(bytes)) && shared.idle());
  assert(!shared.setAirtimeFactor(std::numeric_limits<float>::quiet_NaN()));
  assert(!shared.setAirtimeFactor(-1));
  physical.failStart = false;
  clock.now = 0xFFFFFFF0;
  nrfmast::SharedRadio wrap(physical, clock, true);
  wrap.begin();
  assert(wrap.bot.startSendRaw(bytes, sizeof(bytes)));
  assert(wrap.bot.isSendComplete());
  clock.now += 40;
  wrap.bot.onSendFinished();
  assert(wrap.aggregateRfMs() == 40 && wrap.lastTransmitWasConfirmed());
  assert(wrap.bot.isReceiving());
  clock.now += 40;
  assert(!wrap.bot.isReceiving());
  physical.complete = false;
  assert(wrap.bot.startSendRaw(bytes, sizeof(bytes)));
  clock.now += 100;
  wrap.bot.onSendFinished(); // Driver timeout is active time, never confirmed RF.
  assert(wrap.aggregateActiveMs() == 140 && wrap.aggregateRfMs() == 40 &&
         !wrap.lastTransmitWasConfirmed());
  physical.complete = true;
  nrfmast::SharedRadio muted(physical, clock, false);
  muted.begin();
  assert(!muted.bot.startSendRaw(bytes, sizeof(bytes)));
  assert(!muted.repeater.startSendRaw(bytes, sizeof(bytes)));
  assert(muted.txRejected == 2 && muted.txStarted == 0);
  physical.input.emplace_back(1, ROUTE_TYPE_DIRECT);
  muted.poll();
  physical.input.emplace_back(5, ROUTE_TYPE_TRANSPORT_FLOOD);
  muted.poll();
  assert(muted.bot.rxDrops == 2 && muted.repeater.rxDrops == 2);
  assert(muted.bot.recvRaw(out, sizeof(out)) == 0);
}

static void testAdaptiveMeasurements() {
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  shared.begin();
  onchip::AdaptiveAdmission gate;
  gate.configure(true, 3600, clock.now);
  gate.sample(clock.now, 1, shared.aggregateRfMs(), shared.receivedAirtimeMs(), 0);
  const uint8_t bytes[] = {1, 0, 7}, callerA[32] = {1}, callerB[32] = {2};
  for (unsigned sample = 0; sample < 6; ++sample) {
    for (unsigned frame = 0; frame < 25; ++frame) {
      physical.input.emplace_back(bytes, bytes + sizeof(bytes));
      shared.poll();
    }
    clock.now += 1000;
    gate.sample(clock.now, 1, shared.aggregateRfMs(), shared.receivedAirtimeMs(), 0);
  }
  assert(shared.receivedAirtimeMs() == 6000 && gate.congested());
  assert(shared.bot.rxDrops == 148 && shared.repeater.rxDrops == 148);
  assert(gate.work(clock.now, callerA, true, 0, 0) == onchip::AdaptiveAdmission::Allowed);
  assert(gate.work(clock.now, callerB, true, 0, 0) == onchip::AdaptiveAdmission::Allowed);
  int packet;
  for (unsigned i = 0; i < 11; ++i) {
    assert(gate.reserve(clock.now, callerA, &packet, 40, true) == onchip::AdaptiveAdmission::Allowed);
    gate.finish(clock.now, &packet, true, 40);
  }
  assert(gate.reserve(clock.now, callerA, &packet, 40, true) == onchip::AdaptiveAdmission::CallerShare);
  assert(gate.reserve(clock.now, callerB, &packet, 40, true) == onchip::AdaptiveAdmission::Allowed);
  gate.finish(clock.now, &packet, true, 0);
  for (unsigned i = 0; i < 25; ++i) {
    clock.now += 1000;
    gate.sample(clock.now, 1, shared.aggregateRfMs(), shared.receivedAirtimeMs(), 0);
  }
  assert(!gate.congested() && gate.pending() == 0);
}

static void testFormatting() {
  mesh::Packet packet;
  packet.header = ROUTE_TYPE_FLOOD;
  packet.setPathHashSizeAndCount(2, 3);
  for (int i = 0; i < 6; ++i) packet.path[i] = i + 1;
  packet._snr = -9;
  char reply[MAX_TEXT_LEN + 1];
  assert(nrfmast::CommandBot::formatReply("!ping", packet, -87, 123, reply, sizeof(reply)));
  assert(strcmp(reply, "pong uptime_s=123") == 0);
  assert(nrfmast::CommandBot::formatReply("!signal", packet, -87, 123, reply, sizeof(reply)));
  assert(strcmp(reply, "snr=-2.25dB rssi=-87dBm flood_hops=3 hash_bytes=2 path=010203040506") == 0);
  assert(!nrfmast::CommandBot::formatReply("!ping extra", packet, -87, 123, reply, sizeof(reply)));
  assert(!nrfmast::CommandBot::formatReply("!ping", packet, -87, 123, reply, 4) && reply[0] == 0);
  packet.header = ROUTE_TYPE_DIRECT;
  packet.path_len = 0;
  assert(nrfmast::CommandBot::formatReply("!path", packet, NAN, 123, reply, sizeof(reply)));
  assert(strstr(reply, "rssi=unknown") && strstr(reply, "direct_remaining=0 end_to_end_hops=unknown"));
  packet.header = ROUTE_TYPE_FLOOD;
  packet.setPathHashSizeAndCount(1, 63);
  memset(packet.path, 0xAB, sizeof(packet.path));
  assert(nrfmast::CommandBot::formatReply("!path", packet, -87, 123, reply, sizeof(reply)));
  assert(strlen(reply) <= MAX_TEXT_LEN && strstr(reply, "..."));
  packet.setPathHashSizeAndCount(3, 63);
  assert(!nrfmast::CommandBot::formatReply("!path", packet, -87, 123, reply, sizeof(reply)));
}

static void inject(Physical& radio, mesh::Packet* packet, mesh::Mesh& sender) {
  assert(packet);
  uint8_t bytes[MAX_TRANS_UNIT];
  auto length = packet->writeTo(bytes);
  radio.input.emplace_back(bytes, bytes + length);
  sender.releasePacket(packet);
}

static void testAdaptivePolicyBoot() {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical, peerRadio;
  nrfmast::SharedRadio shared(physical, clock, true);
  SimpleMeshTables botTables, relayTables, peerTables;
  StaticPoolPacketManager botPool(8), relayPool(8), peerPool(8);
  nrfmast::CommandBot bot(shared.bot, clock, rng, rtc, botPool, botTables);
  NativeRelay relay(shared.repeater, clock, rng, rtc, relayPool, relayTables);
  NativePeer peer(peerRadio, clock, rng, rtc, peerPool, peerTables, 1);
  bot.self_id = mesh::LocalIdentity(&rng);
  relay.self_id = mesh::LocalIdentity(&rng);
  peer.self_id = mesh::LocalIdentity(&rng);
  NativeFilesystem fs;
  IdentityStore store(fs, "");
  assert(store.save("_main", relay.self_id) && store.save("_nrfbot", bot.self_id));
  const auto relayRecord = *fs.files.at("/_main.id");
  const auto botRecord = *fs.files.at("/_nrfbot.id");
  fs.files["/pine-adapt"] = std::make_shared<std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{'B', 'A', 'D', 1, 1});
  nrfmast::RuntimeConfig config(fs, relay.self_id, bot);
  assert(config.begin() && !bot.adaptiveEnabled());
  relay.begin();
  bot.begin(false);
  char reply[160];
  assert(config.handleCommand(0, "bot adaptive", reply) && strstr(reply, "policy-fault=1"));
  auto* advert = peer.createSelfAdvert("policy-fault-peer");
  assert(advert);
  advert->header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  inject(physical, advert, peer);
  for (unsigned i = 0; i < 30; ++i) {
    shared.poll();
    if (shared.repeaterCanLoop()) relay.loop();
    if (shared.botCanLoop()) bot.loop();
    clock.now += 20;
  }
  assert(shared.repeater.transmissionsStarted && shared.aggregateRfMs());
  assert(config.handleCommand(0, "bot adaptive", reply) && strstr(reply, "policy-fault=1"));
  assert(config.handleCommand(0, "bot adaptive off", reply) && !strncmp(reply, "Saved", 5));
  assert(config.handleCommand(0, "bot adaptive", reply) && strstr(reply, "saved=0 live=0"));
  assert(*fs.files.at("/_main.id") == relayRecord && *fs.files.at("/_nrfbot.id") == botRecord);
  puts("adaptive policy boot: invalid flag keeps native forwarding/static bot running, identities unchanged, fault retained until checked owner repair");
}

static void testAdaptiveBot() {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  SimpleMeshTables botTables, peerTables;
  StaticPoolPacketManager botPool(8), peerPool(8);
  nrfmast::CommandBot bot(shared.bot, clock, rng, rtc, botPool, botTables);
  nrfmast::CommandBot peer(shared.repeater, clock, rng, rtc, peerPool, peerTables);
  NativeFilesystem fs;
  nrfmast::NoteStore notes(fs, clock);
  assert(notes.begin());
  bot.setNotes(notes);
  bot.self_id = mesh::LocalIdentity(&rng); peer.self_id = mesh::LocalIdentity(&rng);
  bot.bindAdaptiveRadio(shared);
  shared.observeQueues(peerPool, botPool);
  bot.setAdaptiveAdmission(true);
  bot.begin();
  auto tick = [&]() {
    shared.poll(); bot.loop(); clock.now += 20;
    uint8_t discard[MAX_TRANS_UNIT];
    shared.repeater.recvRaw(discard, sizeof(discard));
  };
  auto advert = peer.createSelfAdvert("adaptive-peer");
  advert->header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  inject(physical, advert, peer); tick();
  assert(bot.getNumContacts() == 1);
  uint8_t secret[32]; peer.self_id.calcSharedSecret(secret, bot.self_id);
  auto command = [&](const char* text) {
    uint8_t body[MAX_TEXT_LEN + 6]{};
    const uint32_t at = ++rtc.now;
    memcpy(body, &at, 4); strcpy(reinterpret_cast<char*>(body + 5), text);
    auto* packet = peer.createDatagram(PAYLOAD_TYPE_TXT_MSG, bot.self_id, secret, body, 5 + strlen(text));
    packet->header = (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
    inject(physical, packet, peer);
  };
  for (unsigned i = 0; i < 4; ++i) {
    auto* packet = peer.createSelfAdvert("queued-role");
    peer.sendZeroHop(packet);
  }
  physical.airtime = 2000;
  command("!remember blocked value"); tick();
  assert(bot.throttled == 1 && !bot.replies);
  char text[160];
  assert(notes.command(bot.self_id.pub_key, peer.self_id.pub_key, ++rtc.now,
                       "!recall blocked", text, sizeof(text)) && strstr(text, "not found"));
  while (peerPool.getOutboundTotal()) peer.releasePacket(peerPool.removeOutboundByIdx(0));
  physical.airtime = 40;
  for (unsigned i = 0; i < 25; ++i) { clock.now += 1000; tick(); }
  bot.adaptiveStatus(true, text, sizeof(text));
  assert(strstr(text, "congested=0") && strstr(text, "pending=0") && strstr(text, "last=congestion"));
  const auto* contact = bot.lookupContactByPubKey(peer.self_id.pub_key, 32);
  assert(contact);
  uint32_t ack, timeout;
  physical.complete = false;
  assert(bot.companionSend(*contact, TXT_TYPE_CLI_DATA, 0, ++rtc.now, "native app", ack, timeout) != MSG_SEND_FAILED);
  command("!remember allowed value");
  tick();
  bot.adaptiveStatus(true, text, sizeof(text));
  assert(strstr(text, "pending=1"));
  physical.complete = true;
  tick();
  bot.adaptiveStatus(true, text, sizeof(text));
  assert(strstr(text, "pending=1")); // Companion TXT completion cannot settle the bot reply.
  for (unsigned i = 0; i < 100; ++i) tick();
  assert(bot.replies == 1);
  bot.adaptiveStatus(true, text, sizeof(text));
  assert(strstr(text, "pending=0"));
  assert(notes.command(bot.self_id.pub_key, peer.self_id.pub_key, ++rtc.now,
                       "!recall allowed", text, sizeof(text)) && !strcmp(text, "allowed=value"));
  puts("native adaptive bot: role queue/load denial before note mutation, owner opt-in, actual packet TX settlement and recovery passed");
}

static void testEncryptedBot(bool scoped = false, bool canTransmit = true) {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, canTransmit);
  SimpleMeshTables botTables, peerTables;
  StaticPoolPacketManager botPool(8), peerPool(8);
  nrfmast::CommandBot bot(shared.bot, clock, rng, rtc, botPool, botTables, canTransmit);
  nrfmast::CommandBot peer(shared.repeater, clock, rng, rtc, peerPool, peerTables);
  NativeFilesystem noteFS;
  nrfmast::NoteStore notes(noteFS, clock);
  assert(notes.begin());
  bot.setNotes(notes);
  bot.self_id = mesh::LocalIdentity(&rng);
  peer.self_id = mesh::LocalIdentity(&rng);
  assert(!bot.self_id.matches(peer.self_id));
  bot.begin();
  auto tick = [&]() {
    shared.poll();
    bot.loop();
    uint8_t discard[MAX_TRANS_UNIT];
    shared.repeater.recvRaw(discard, sizeof(discard));
    clock.now += 20;
  };
  auto advert = peer.createSelfAdvert("generated-test-peer");
  advert->header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  inject(physical, advert, peer);
  tick();
  assert(bot.getNumContacts() == 1);
  uint8_t secret[PUB_KEY_SIZE];
  peer.self_id.calcSharedSecret(secret, bot.self_id);
  TransportKey scope;
  rng.random(scope.key, sizeof(scope.key));
  auto makeCommand = [&](const char* text, bool flood) {
    uint8_t plain[MAX_TEXT_LEN + 6] = {};
    uint32_t timestamp = ++rtc.now;
    memcpy(plain, &timestamp, 4);
    strcpy(reinterpret_cast<char*>(plain + 5), text);
    auto packet = peer.createDatagram(PAYLOAD_TYPE_TXT_MSG, bot.self_id, secret, plain, 5 + strlen(text));
    assert(packet);
    auto route = scoped ? (flood ? ROUTE_TYPE_TRANSPORT_FLOOD : ROUTE_TYPE_TRANSPORT_DIRECT)
                        : (flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT);
    packet->header = (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | route;
    if (scoped) {
      packet->transport_codes[0] = scope.calcTransportCode(packet);
      packet->transport_codes[1] = 0;
    }
    if (flood) {
      packet->setPathHashSizeAndCount(2, 2);
      memcpy(packet->path, "\x12\x34\x56\x78", 4);
    }
    return packet;
  };
  auto sendCommand = [&](const char* text, bool flood) {
    inject(physical, makeCommand(text, flood), peer);
  };
  if (!canTransmit) {
    sendCommand("!remember disabled-radio value", false);
    for (int i = 0; i < 20; ++i) tick();
    assert(bot.failures >= 1 && bot.replies == 0 && !noteFS.exists("/pine-notes"));
    assert(physical.sent.empty() && !bot.txBusy());
    std::puts("native: RF-disabled command rejected without note mutation");
    return;
  }
  if (scoped) {
    for (bool flood : {true, false}) {
      for (int index : {0, 2}) { // Wrong destination, then damaged message MAC.
        auto packet = makeCommand("!remember compromised value", flood);
        packet->payload[index] ^= 1;
        packet->transport_codes[0] = scope.calcTransportCode(packet);
        inject(physical, packet, peer);
        for (int i = 0; i < 100; ++i) tick();
        assert(bot.replies == 0 && physical.sent.empty());
        assert(!noteFS.exists("/pine-notes"));
      }
    }
  }
  mesh::Packet* held[7] = {};
  for (auto& packet : held) { packet = botPool.allocNew(); assert(packet); }
  sendCommand("!remember pool-full value", false);
  for (int i = 0; i < 20; ++i) tick();
  assert(bot.failures == 1 && bot.replies == 0 && !noteFS.exists("/pine-notes"));
  for (auto* packet : held) botPool.free(packet);
  assert(botPool.getFreeCount() == 8);
  const uint32_t firstReplyAt = clock.now;
  sendCommand("!ping", true);
  for (int i = 0; i < 100; ++i) tick();
  assert(bot.replies == 1);
  bool received = false;
  uint32_t ack = 0;
  for (const auto& raw : physical.sent) {
    mesh::Packet packet;
    assert(packet.readFrom(raw.data(), raw.size()));
    assert(!packet.hasTransportCodes());
    if (packet.getPayloadType() != PAYLOAD_TYPE_TXT_MSG) continue;
    assert(packet.payload[0] == peer.self_id.pub_key[0]);
    assert(packet.payload[1] == bot.self_id.pub_key[0]);
    uint8_t decoded[MAX_PACKET_PAYLOAD + 1] = {};
    int length = mesh::Utils::MACThenDecrypt(secret, decoded, packet.payload + 2, packet.payload_len - 2);
    assert(length > 5);
    assert(strstr(reinterpret_cast<char*>(decoded + 5), "pong uptime_s=") == reinterpret_cast<char*>(decoded + 5));
    mesh::Utils::sha256(reinterpret_cast<uint8_t*>(&ack), 4, decoded,
                        5 + strlen(reinterpret_cast<char*>(decoded + 5)), bot.self_id.pub_key, PUB_KEY_SIZE);
    received = true;
  }
  assert(received);
  sendCommand("!remember busy-slot value", false);
  for (int i = 0; i < 20; ++i) tick();
  assert(bot.replies == 1 && bot.throttled == 1 && !noteFS.exists("/pine-notes"));
  auto ackPacket = peer.createAck(ack);
  ackPacket->header = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_DIRECT;
  inject(physical, ackPacket, peer);
  tick();
  assert(!bot.txBusy() && uint32_t(clock.now - firstReplyAt) < 10000);
  physical.sent.clear();
  sendCommand("!signal", false);
  for (int i = 0; i < 100; ++i) tick();
  assert(bot.replies == 2);
  assert(bot.throttled == 1);  // ACK release admits the next command without a fixed delay.
  received = false;
  for (const auto& raw : physical.sent) {
    mesh::Packet packet;
    assert(packet.readFrom(raw.data(), raw.size()));
    assert(!packet.hasTransportCodes());
    if (packet.getPayloadType() != PAYLOAD_TYPE_TXT_MSG) continue;
    uint8_t decoded[MAX_PACKET_PAYLOAD + 1] = {};
    assert(mesh::Utils::MACThenDecrypt(secret, decoded, packet.payload + 2, packet.payload_len - 2) > 5);
    auto text = reinterpret_cast<char*>(decoded + 5);
    assert(strstr(text, "snr=-2.25dB rssi=-87dBm"));
    assert(strstr(text, "direct_remaining=0 end_to_end_hops=unknown"));
    received = true;
  }
  assert(received);
  clock.now += 30000;
  for (int i = 0; i < 20; ++i) tick();
  assert(bot.timeouts == 1);
  assert(!noteFS.exists("/pine-notes"));  // Capacity rejections are not deferred/replayed later.
  physical.sent.clear();
  sendCommand("!path", true);
  for (int i = 0; i < 100; ++i) tick();
  assert(bot.replies == 3);
  received = false;
  for (const auto& raw : physical.sent) {
    mesh::Packet packet;
    assert(packet.readFrom(raw.data(), raw.size()));
    assert(!packet.hasTransportCodes());
    if (packet.getPayloadType() != PAYLOAD_TYPE_TXT_MSG) continue;
    uint8_t decoded[MAX_PACKET_PAYLOAD + 1] = {};
    assert(mesh::Utils::MACThenDecrypt(secret, decoded, packet.payload + 2, packet.payload_len - 2) > 5);
    assert(strstr(reinterpret_cast<char*>(decoded + 5),
                  "snr=-2.25dB rssi=-87dBm flood_hops=2 hash_bytes=2 path=12345678"));
    received = true;
  }
  assert(received);
  std::printf("native: generated-key adverts, encrypted %sflood/direct commands, distinct bot sender and reply decryption passed\n",
              scoped ? "transport-" : "");
}

int main() {
  testAdaptivePolicyBoot();
  testAdaptiveBot();
  testAdaptiveMeasurements();
  extern void testNoteJournal();
  testNoteJournal();
  testNotes();
  testNoteWriteBudget();
  testCompanion();
  testRuntimeConfig();
  testRadioTime();
  testSharing();
  testFormatting();
  testEncryptedBot();
  testEncryptedBot(true);
  testEncryptedBot(false, false);
  testEncryptedBot(true, false);
  testStartupAdvert(1000, true);
  testStartupAdvert(0xFFFFFFF0u, true);
  testStartupAdvert(1000, false);
  testStartupAdvert(1000, true, true);
  testOperatorAdvertPreemptsStartup();
  for (uint8_t width = 1; width <= 3; ++width) {
    testNativePeerRouting(width);
    testNativePeerRouting(width, true);
  }
  std::puts("native: RX snapshots, bounds, shared TX exclusion/duty spacing, rollover and hard RF mute passed");
}
