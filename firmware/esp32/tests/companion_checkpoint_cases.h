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
    assert(f.action("role name companion Before failure").find("Saved") == 0);
    assert(f.action("role channel companion 1 4f7073 0102030405060708090a0b0c0d0e0f10").find("Saved") == 0);
    const auto preferences = filesystem_test::files.at("/companion/prefs.json");
    const auto channels = filesystem_test::files.at("/companion/channels2");
    const auto channel = f.action("role channel companion 1");
    for (size_t limit : {size_t(0), size_t(1)}) {
      filesystem_test::writeLimit = limit;
      assert(f.action("role name companion Failed write").find("Error:") == 0);
      assert(f.action("role channel companion 1 off").find("Error:") == 0);
      assert(filesystem_test::files.at("/companion/prefs.json") == preferences);
      assert(filesystem_test::files.at("/companion/channels2") == channels);
      assert(f.action("role name companion") == "Name: Before failure");
      assert(f.action("role channel companion 1") == channel);
    }
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    // The actual companion setter must send ERR, not OK, after a failed save.
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(companionSessions().port());
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    const auto appCommand = [&](const Bytes &data) {
      Bytes request{'<', uint8_t(data.size()), 0};
      request.insert(request.end(), data.begin(), data.end());
      assert(send(fd, request.data(), request.size(), MSG_NOSIGNAL) == ssize_t(request.size()));
      Bytes response;
      for (unsigned tick = 0; tick < 100 && (response.size() < 3 ||
           response.size() < size_t(3 + response[1] + (unsigned(response[2]) << 8))); ++tick) {
        f.step(10); uint8_t buffer[512];
        const auto size = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size > 0) response.insert(response.end(), buffer, buffer + size);
        else assert(size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
      }
      assert(response.size() >= 4 && response[0] == '>');
      return Bytes(response.begin() + 3, response.end());
    };
    filesystem_test::writeLimit = 0;
    assert(appCommand({8, 'A', 'p', 'p'}) == Bytes({1, 5})); // CMD_SET_ADVERT_NAME; file I/O error.
    assert(filesystem_test::files.at("/companion/prefs.json") == preferences);
    Bytes setChannel(2 + 32 + 16, 0); setChannel[0] = 32; setChannel[1] = 1;
    memcpy(setChannel.data() + 2, "Changed", 7);
    assert(appCommand(setChannel) == Bytes({1, 5}));
    assert(filesystem_test::files.at("/companion/channels2") == channels);
    assert(f.action("role channel companion 1") == channel);
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    assert(appCommand({8, 'A', 'p', 'p'}) == Bytes({0}));
    assert(f.action("role name companion") == "Name: App");
    close(fd);
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
    const std::string previous = std::string("/companion/bp/") + prefix;
    filesystem_test::files[previous] = oldBlob;
    // Staging/readback succeeds, then the current primary cannot be opened.
    filesystem_test::step = [](const char *phase) {
      static unsigned readOpens = 0;
      if (!strcmp(phase, "file.before-open-read") && ++readOpens == 3) filesystem_test::failOpen = true;
    };
    onchip::ScopedFS primaryFiles(SPIFFS, "/companion");
    const std::string blobPath = std::string("/bl/") + prefix;
    assert(!onchip::companion_persistence::blobPut(primaryFiles, blobPath.c_str(), lastAdvert.data(), lastAdvert.size()));
    filesystem_test::step = nullptr; filesystem_test::failOpen = false;
    assert(filesystem_test::files.at(primary) == oldBlob);
    assert(filesystem_test::files.at(previous) == oldBlob);
    filesystem_test::step = [](const char *phase) {
      if (!strcmp(phase, "file.after-open-write") && filesystem_test::files.count("/companion/bl-writing")) {
        filesystem_test::failReadAfterRename = true;
        filesystem_test::step = nullptr;
      }
    };
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
    assert(restarted.action("role name companion") == "Name: App");
    assert(restarted.action("role channel companion 1").find("name=Ops ") != std::string::npos);
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
  filesystem_test::files["/companion/prefs.json"] = {' ', ' '};
  filesystem_test::files["/companion/channels2"].resize(3);
  for (auto &entry : filesystem_test::files)
    if (entry.first == "/companion/contacts3") entry.second.resize(3);
  {
    BetaFixture restarted(true);
    assert(restarted.action("role name companion") == "Name: App");
    assert(restarted.action("role channel companion 1").find("name=Ops ") != std::string::npos);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
    mesh::Packet packet{}; assert(packet.readFrom(raw, size));
    assert(!memcmp(packet.payload, caller.self_id.pub_key, 32));
    restarted.bot.setContactLookup(companionContactAdvert);
    const auto request = caller.command(restarted.bot.publicKey(), "!ping");
    restarted.mux.received(request.data(), request.size(), -90, 5); restarted.step(300);
    assert(caller.replies(restarted.bot.publicKey(), restarted.radio) == std::vector<std::string>{"Pong"});
  }
  // A restart between the two renames loads the old preferences and channels.
  filesystem_test::files = committed;
  filesystem_test::files.erase("/companion/prefs.json");
  filesystem_test::files.erase("/companion/channels2");
  {
    BetaFixture restarted(true);
    assert(restarted.action("role name companion") == "Name: App");
    assert(restarted.action("role channel companion 1").find("name=Ops ") != std::string::npos);
  }
  // A rolled-back application can legitimately clear legacy metadata; never resurrect its previous contacts.
  filesystem_test::files = committed;
  filesystem_test::files["/companion/contacts3"].clear();
  filesystem_test::files["/companion/channels2"].clear();
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(!companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
    assert(restarted.action("role channel companion 1").find("name=off ") != std::string::npos);
  }
  identity_test::durable = records; filesystem_test::files = files;
  filesystem_test::maxPathLength = oldMaxPathLength;
  assert(reloadAutomaticAdverts());
  puts("PASS companion checkpoint: latest pending signed export without per-advert writes, first-dirty300s bounded checkpoint, idle suppression, failed-write retry, flush errno and reset, forced flush, full restart DM and corrupt primary fallback");
}
