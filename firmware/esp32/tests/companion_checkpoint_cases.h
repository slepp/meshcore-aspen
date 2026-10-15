// SPDX-License-Identifier: Apache-2.0
#include "CompanionPersistence.h"
#include "ScopedFS.h"
static void companion_checkpoint() {
  const auto oldMaxPathLength = filesystem_test::maxPathLength;
  filesystem_test::maxPathLength = 31; // ESP32 SPIFFS CONFIG_SPIFFS_OBJ_NAME_LEN=32 includes NUL.
  const auto records = identity_test::durable;
  const auto files = filesystem_test::files;
  {
    onchip::ScopedFS scoped(SPIFFS, "/diagnostic");
    filesystem_test::failOpen = true;
    filesystem_test::step = [](const char *phase) {
      if (!strcmp(phase, "file.before-open-write")) errno = EIO;
    };
    assert(!scoped.open("/test", "w") && scoped.failed() && scoped.error() == EIO);
    filesystem_test::failOpen = false; filesystem_test::step = nullptr;
    scoped.clearErrors(); assert(!scoped.failed() && !scoped.error());
    auto file = scoped.open("/test", "w"); assert(file);
    filesystem_test::writeLimit = 0;
    filesystem_test::step = [](const char *phase) {
      if (!strcmp(phase, "file.before-write")) errno = ENOSPC;
      if (!strcmp(phase, "file.after-close")) errno = EBADF;
    };
    const uint8_t byte = 1;
    assert(!file.write(&byte, 1) && scoped.failed() && scoped.error() == ENOSPC);
    file.close(); assert(scoped.error() == ENOSPC); // Cleanup cannot hide the write error.
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    filesystem_test::step = nullptr; scoped.clearErrors();
    file = scoped.open("/test", "w"); assert(file);
    errno = EIO; assert(file.write(&byte, 1) == 1 && !scoped.failed() && !scoped.error());
    filesystem_test::afterFlush = [] { errno = EIO; };
    file.flush(); assert(!scoped.failed() && scoped.error() == EIO);
    filesystem_test::afterFlush = nullptr; scoped.clearErrors();
    filesystem_test::step = [](const char *phase) {
      if (!strcmp(phase, "file.after-close")) errno = ENOSPC;
    };
    file.close(); assert(!scoped.failed() && scoped.error() == ENOSPC);
    filesystem_test::step = nullptr; scoped.clearErrors();
    assert(!scoped.failed() && !scoped.error());
    assert(SPIFFS.remove("/diagnostic/test"));
  }
  assert(saveRoleProfile({7}) && saveBotEnabled(true) && saveAutomaticAdverts(false));
  beginClocks(RoleProfile(7));
  Peer caller; caller.rtc.setCurrentTime(1791700000);
  Bytes lastAdvert;
  std::map<std::string, std::vector<uint8_t>> committed;
  {
    BetaFixture f(true);
    const auto before = filesystem_test::files;
    lastAdvert = caller.advert();
    f.mux.received(lastAdvert.data(), lastAdvert.size(), -90, 5); f.step();
    assert(filesystem_test::files == before); // No signed-advert file or metadata write on reception.
    for (unsigned update = 1; update < 4; ++update) {
      timeMs += 60000; caller.rtc.setCurrentTime(1791700000 + update * 60);
      lastAdvert = caller.advert();
      f.mux.received(lastAdvert.data(), lastAdvert.size(), -90, 5); f.step();
      assert(filesystem_test::files == before);
      unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
      assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
      assert(Bytes(raw, raw + size) == lastAdvert); // Export reads the pending latest version.
    }
    timeMs += 120000; f.step(1); // First dirty deadline, despite the repeated updates.
    assert(filesystem_test::files != before);
    committed = filesystem_test::files;
    f.step(100); assert(filesystem_test::files == committed);
    // A failed periodic write preserves the previous files and waits another300s.
    caller.rtc.setCurrentTime(1791700400); lastAdvert = caller.advert();
    f.mux.received(lastAdvert.data(), lastAdvert.size(), -90, 5); f.step();
    filesystem_test::writeLimit = 0; timeMs += 300000; f.step(1);
    const auto failed = filesystem_test::files;
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    f.step(100); assert(filesystem_test::files == failed);
    filesystem_test::writeLimit = 0;
    assert(!companionFlush()); // Failed explicit checkpoint identifies the affected native store.
    assert(!strcmp(runtimeFlushError(), "companion preferences"));
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    // Failure after the old blob moves aside still leaves its verified signed bytes recoverable.
    char prefix[17]; mesh::Utils::toHex(prefix, caller.self_id.pub_key, 8);
    const std::string primary = std::string("/companion/bl/") + prefix;
    const auto oldBlob = filesystem_test::files.at(primary);
    filesystem_test::failReadAfterRename = true;
    MastAdmin::Reply reboot;
    f.management.admin().execute("reboot", reboot);
    assert(strstr(reboot.text, "Accepted reboot"));
    f.management.admin().acknowledged(reboot.ticket, true);
    f.step(70);
    assert(f.action("job").find("companion advert blob flush failed; reboot cancelled") != std::string::npos);
    filesystem_test::readLimit = std::numeric_limits<size_t>::max();
    assert(filesystem_test::files.at(std::string("/companion/bp/") + prefix) == oldBlob);
    filesystem_test::files[primary].resize(3);
    onchip::ScopedFS companionFiles(SPIFFS, "/companion");
    uint8_t fallback[255]{};
    const std::string local = std::string("/bl/") + prefix;
    const auto recovered = onchip::companion_persistence::blobGet(companionFiles, local.c_str(), fallback);
    assert(Bytes(fallback, fallback + recovered) == oldBlob);
    assert(companionFlush()); // Explicit force updates an existing 30-character blob path.
    assert(filesystem_test::files.at(primary) == lastAdvert);
    const auto metadata = filesystem_test::files.at("/companion/contacts3");
    filesystem_test::step = [](const char *phase) {
      if (!strcmp(phase, "file.before-write") && filesystem_test::files.count("/companion/contacts3-writing")) {
        filesystem_test::writeLimit = 0; errno = EIO;
      }
    };
    assert(!flushRuntimeState());
    assert(!strcmp(runtimeFlushError(), "companion contact metadata errno=5"));
    assert(filesystem_test::files.at("/companion/contacts3") == metadata);
    filesystem_test::step = nullptr;
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    assert(companionFlush());
    committed = filesystem_test::files;
    for (const auto &entry : committed)
      assert(entry.first.size() <= 31 && entry.first.rfind("/companion/bp/", 0) != 0);
  }
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
    assert(Bytes(raw, raw + size) == lastAdvert);
    restarted.bot.setContactLookup(companionContactAdvert);
    const auto request = caller.command(restarted.bot.publicKey(), "!ping");
    restarted.mux.received(request.data(), request.size(), -90, 5); restarted.step(300);
    assert(caller.replies(restarted.bot.publicKey(), restarted.radio) == std::vector<std::string>{"Pong"});
  }
  // A truncated primary metadata file cannot hide the previous complete record set.
  filesystem_test::files = committed;
  for (auto &entry : filesystem_test::files)
    if (entry.first == "/companion/contacts3") entry.second.resize(3);
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
    mesh::Packet packet{}; assert(packet.readFrom(raw, size));
    assert(!memcmp(packet.payload, caller.self_id.pub_key, 32));
    restarted.bot.setContactLookup(companionContactAdvert);
    const auto request = caller.command(restarted.bot.publicKey(), "!ping");
    restarted.mux.received(request.data(), request.size(), -90, 5); restarted.step(300);
    assert(caller.replies(restarted.bot.publicKey(), restarted.radio) == std::vector<std::string>{"Pong"});
  }
  // A rolled-back application can legitimately clear legacy metadata; never resurrect its previous contacts.
  filesystem_test::files = committed;
  filesystem_test::files["/companion/contacts3"].clear();
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(!companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
  }
  identity_test::durable = records; filesystem_test::files = files;
  filesystem_test::maxPathLength = oldMaxPathLength;
  assert(reloadAutomaticAdverts());
  puts("PASS companion checkpoint: latest pending signed export without per-advert writes, first-dirty300s bounded checkpoint, idle suppression, failed-write retry, flush errno and reset, forced flush, full restart DM and corrupt primary fallback");
}
