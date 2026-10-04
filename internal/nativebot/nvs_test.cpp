// SPDX-License-Identifier: Apache-2.0
#include "nvs.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
struct Fixture {
  std::string path, alias;
  explicit Fixture(unsigned number) {
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    path = std::string(cwd) + "/.nvs-fixture-" + std::to_string(getpid()) +
           "-" + std::to_string(number);
    alias = path + "-link";
    assert(mkdir(path.c_str(), 0700) == 0);
  }
  ~Fixture() {
    native_nvs_shutdown();
    assert(unlink(alias.c_str()) == 0 || errno == ENOENT);
    assert(unlink((path + "/nvs.snapshot").c_str()) == 0 || errno == ENOENT);
    assert(rmdir(path.c_str()) == 0);
  }
  void init() const { assert(native_nvs_init(path.c_str()) == ESP_OK); }
  std::string snapshot() const { return path + "/nvs.snapshot"; }
};
nvs_handle_t open(const char *name, int mode = NVS_READWRITE) {
  nvs_handle_t handle = 0;
  assert(nvs_open(name, mode, &handle) == ESP_OK && handle);
  return handle;
}
void set(nvs_handle_t handle, const char *key, const std::vector<uint8_t> &value) {
  assert(nvs_set_blob(handle, key, value.data(), value.size()) == ESP_OK);
}
std::vector<uint8_t> get(nvs_handle_t handle, const char *key) {
  size_t size = 0;
  assert(nvs_get_blob(handle, key, nullptr, &size) == ESP_OK);
  std::vector<uint8_t> value(size);
  assert(nvs_get_blob(handle, key, value.data(), &size) == ESP_OK);
  assert(size == value.size());
  return value;
}
void replace_byte(const std::string &file) {
  int fd = ::open(file.c_str(), O_RDWR | O_NOFOLLOW);
  assert(fd >= 0);
  uint8_t byte;
  assert(pread(fd, &byte, 1, 25) == 1);
  byte ^= 0x7f;
  assert(pwrite(fd, &byte, 1, 25) == 1);
  assert(fsync(fd) == 0);
  assert(close(fd) == 0);
}
void persistence() {
  Fixture fixture(1);
  fixture.init();
  nvs_handle_t readonly;
  assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_ARG);
  assert(nvs_open("mc-onchip", NVS_READONLY, &readonly) == ESP_ERR_NVS_NOT_FOUND);
  nvs_handle_t write = open("mc-onchip");
  assert(nvs_open("mc-onchip", NVS_READONLY, &readonly) == ESP_ERR_NVS_NOT_FOUND);
  std::vector<uint8_t> seed(64);
  for (unsigned i = 0; i < seed.size(); ++i) seed[i] = uint8_t(i * 3);
  set(write, "command-bot", seed);
  size_t size = 0;
  assert(nvs_get_blob(write, "command-bot", nullptr, &size) == ESP_ERR_NVS_NOT_FOUND);
  assert(nvs_commit(write) == ESP_OK);
  readonly = open("mc-onchip", NVS_READONLY);
  assert(get(readonly, "command-bot") == seed);
  size = 1;
  uint8_t short_buffer[1] = {};
  assert(nvs_get_blob(readonly, "command-bot", short_buffer, &size) ==
         ESP_ERR_NVS_INVALID_LENGTH && size == 64);
  assert(nvs_set_blob(readonly, "command-bot", seed.data(), seed.size()) ==
         ESP_ERR_INVALID_ARG);
  nvs_close(readonly);
  nvs_close(write);
  struct stat st{};
  assert(stat(fixture.snapshot().c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
         (st.st_mode & 07777) == 0600);
  assert(stat(fixture.path.c_str(), &st) == 0 && (st.st_mode & 07777) == 0700);
  native_nvs_shutdown();
  fixture.init();
  assert(nvs_open("absent", NVS_READONLY, &readonly) == ESP_ERR_NVS_NOT_FOUND);
  readonly = open("mc-onchip", NVS_READONLY);
  assert(get(readonly, "command-bot") == seed);
  nvs_close(readonly);
}
void transactions() {
  Fixture fixture(2);
  fixture.init();
  nvs_handle_t write = open("mc-bot-kv");
  set(write, "a", {1});
  set(write, "b", {2});
  nvs_close(write);
  assert(nvs_open("mc-bot-kv", NVS_READONLY, &write) == ESP_ERR_NVS_NOT_FOUND);
  write = open("mc-bot-kv");
  set(write, "a", {3});
  set(write, "b", {4});
  assert(nvs_commit(write) == ESP_OK);
  nvs_handle_t reader = open("mc-bot-kv", NVS_READONLY);
  assert(get(reader, "a") == std::vector<uint8_t>{3});
  assert(get(reader, "b") == std::vector<uint8_t>{4});
  set(write, "a", {5});
  assert(get(write, "a") == std::vector<uint8_t>{3});
  nvs_close(write);
  native_nvs_shutdown();
  fixture.init();
  assert(nvs_get_blob(reader, "a", nullptr, nullptr) == ESP_ERR_INVALID_ARG);
  reader = open("mc-bot-kv", NVS_READONLY);
  assert(get(reader, "a") == std::vector<uint8_t>{3});
  assert(get(reader, "b") == std::vector<uint8_t>{4});
  nvs_close(reader);
}
void reclaim_one_key() {
  Fixture fixture(30);
  fixture.init();
  auto writer = open("mc-bot-kv");
  set(writer, "v00", std::vector<uint8_t>(392, 7));
  set(writer, "txn", std::vector<uint8_t>(200, 9));
  set(writer, "empty", {});
  assert(nvs_commit(writer) == ESP_OK);
  nvs_stats_t before{}, after{};
  assert(nvs_get_stats(nullptr, &before) == ESP_OK);
  auto reader = open("mc-bot-kv", NVS_READONLY);
  assert(nvs_erase_key(reader, "v00") == ESP_ERR_INVALID_ARG);
  assert(nvs_erase_key(writer, "missing") == ESP_ERR_NVS_NOT_FOUND);
  assert(nvs_erase_key(writer, "v00") == ESP_OK);
  assert(get(reader, "v00").size() == 392);
  nvs_close(writer); // Uncommitted erase is not a durable reclaim.
  writer = open("mc-bot-kv");
  assert(nvs_erase_key(writer, "v00") == ESP_OK);
  assert(nvs_commit(writer) == ESP_OK);
  size_t size = 0;
  assert(nvs_get_blob(reader, "v00", nullptr, &size) == ESP_ERR_NVS_NOT_FOUND);
  assert(nvs_get_stats(nullptr, &after) == ESP_OK && after.free_entries == before.free_entries + 15);
  assert(get(reader, "txn") == std::vector<uint8_t>(200, 9) && get(reader, "empty").empty());
  set(writer, "v00", {8});
  assert(nvs_erase_key(writer, "v00") == ESP_OK);
  set(writer, "v00", {6}); // Set after erase replaces the pending erase.
  assert(nvs_commit(writer) == ESP_OK);
  nvs_close(writer); nvs_close(reader);
  native_nvs_shutdown(); fixture.init();
  reader = open("mc-bot-kv", NVS_READONLY);
  assert(get(reader, "v00") == std::vector<uint8_t>{6});
  assert(get(reader, "txn") == std::vector<uint8_t>(200, 9) && get(reader, "empty").empty());
  nvs_close(reader);
}
void corruption() {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    Fixture fixture(3 + scenario);
    fixture.init();
    auto write = open("mc-onchip");
    set(write, "identity", {1, 2, 3});
    assert(nvs_commit(write) == ESP_OK);
    nvs_close(write);
    native_nvs_shutdown();
    if (scenario == 0) {
      assert(chmod(fixture.snapshot().c_str(), 0644) == 0);
      assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_STATE);
      assert(chmod(fixture.snapshot().c_str(), 0600) == 0);
    }
    if (scenario == 0) replace_byte(fixture.snapshot());
    if (scenario == 1) assert(truncate(fixture.snapshot().c_str(), 8) == 0);
    if (scenario == 2) assert(truncate(fixture.snapshot().c_str(), 1024 * 1024 + 1) == 0);
    assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_STATE);
    nvs_handle_t invalid;
    assert(nvs_open("mc-onchip", NVS_READWRITE, &invalid) == ESP_ERR_INVALID_STATE);
    assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_STATE);
  }
}
void paths_and_names() {
  Fixture fixture(6);
  assert(native_nvs_init("relative/path") == ESP_ERR_INVALID_ARG);
  assert(symlink(fixture.path.c_str(), fixture.alias.c_str()) == 0);
  assert(native_nvs_init(fixture.alias.c_str()) == ESP_ERR_INVALID_STATE);
  assert(native_nvs_init((fixture.path + "/../" + fixture.path.substr(fixture.path.rfind('/') + 1)).c_str()) ==
         ESP_ERR_INVALID_STATE);
  assert(chmod(fixture.path.c_str(), 0755) == 0);
  assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_STATE);
  assert(chmod(fixture.path.c_str(), 0700) == 0);
  assert(symlink("nonexistent", fixture.snapshot().c_str()) == 0);
  assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_STATE);
  assert(unlink(fixture.snapshot().c_str()) == 0);
  fixture.init();
  nvs_handle_t handle;
  assert(nvs_open("", NVS_READWRITE, &handle) == ESP_ERR_INVALID_ARG);
  assert(nvs_open("sixteencharacters", NVS_READWRITE, &handle) == ESP_ERR_INVALID_ARG);
  assert(nvs_open("bad/name", NVS_READWRITE, &handle) == ESP_ERR_INVALID_ARG);
  assert(nvs_open("valid", 99, &handle) == ESP_ERR_INVALID_ARG);
  handle = open("valid");
  uint8_t data = 1;
  assert(nvs_set_blob(handle, "bad/key", &data, 1) == ESP_ERR_INVALID_ARG);
  assert(nvs_set_blob(handle, "", &data, 1) == ESP_ERR_INVALID_ARG);
  assert(nvs_set_blob(handle, "valid", nullptr, 1) == ESP_ERR_INVALID_ARG);
  nvs_close(handle);
}
void headroom_and_threads() {
  Fixture fixture(7);
  fixture.init();
  nvs_stats_t before{}, after{};
  assert(nvs_get_stats(nullptr, &before) == ESP_OK);
  assert(before.free_entries > 308 &&
         before.free_entries + before.used_entries == before.total_entries);
  auto run = [](int index) {
    auto handle = open("mc-bot-kv");
    std::string key = "thread" + std::to_string(index);
    std::vector<uint8_t> value(192, uint8_t(index));
    set(handle, key.c_str(), value);
    assert(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) threads.emplace_back(run, i);
  for (auto &thread : threads) thread.join();
  assert(nvs_get_stats(nullptr, &after) == ESP_OK);
  assert(after.free_entries < before.free_entries && after.namespace_count == 1);
  auto handle = open("mc-bot-kv", NVS_READONLY);
  for (int i = 0; i < 8; ++i) {
    auto value = get(handle, ("thread" + std::to_string(i)).c_str());
    assert(value.size() == 192 && value[0] == i);
  }
  nvs_close(handle);
  handle = open("mc-bot-kv");
  std::vector<uint8_t> huge(32769);
  assert(nvs_set_blob(handle, "too-big", huge.data(), huge.size()) ==
         ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  std::vector<uint8_t> chunk(1024, 7);
  bool exhausted = false;
  for (unsigned i = 0; i < 30; ++i) {
    const auto key = "large" + std::to_string(i);
    if (nvs_set_blob(handle, key.c_str(), chunk.data(), chunk.size()) ==
        ESP_ERR_NVS_NOT_ENOUGH_SPACE) { exhausted = true; break; }
    assert(nvs_commit(handle) == ESP_OK);
  }
  assert(exhausted);
  assert(nvs_get_stats(nullptr, &after) == ESP_OK && after.free_entries < 308);
  nvs_close(handle);
}
void native_partition_admission_boundary() {
  Fixture fixture(11);
  fixture.init();
  auto writer = open("mc-bot-kv");
  for (unsigned i = 0; i < 55; ++i) {
    const auto key = "v" + std::to_string(i);
    set(writer, key.c_str(), std::vector<uint8_t>(32, 1));
  }
  for (unsigned i = 0; i < 5; ++i) {
    const auto key = "x" + std::to_string(i);
    set(writer, key.c_str(), {});
  }
  assert(nvs_commit(writer) == ESP_OK);
  nvs_stats_t stats{};
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK &&
         stats.total_entries == 630 && stats.used_entries == 176 &&
         stats.free_entries == 454);
  for (unsigned i = 0; i < 49; ++i) {
    const auto key = "w" + std::to_string(i);
    set(writer, key.c_str(), std::vector<uint8_t>(32, 2));
  }
  assert(nvs_commit(writer) == ESP_OK);
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK &&
         stats.total_entries == 630 && stats.used_entries == 323 &&
         stats.free_entries == 307);
  assert(stats.free_entries < 308);
  std::vector<uint8_t> journal(6000);
  assert(nvs_set_blob(writer, "txn", journal.data(), journal.size()) ==
         ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  native_nvs_shutdown();
  fixture.init();
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.free_entries == 307);
  nvs_close(writer);
}
void failed_commit_stays_failed() {
  Fixture fixture(8);
  fixture.init();
  auto write = open("mc-onchip");
  set(write, "old", {1});
  assert(nvs_commit(write) == ESP_OK);
  set(write, "old", {2});
  set(write, "other", {3});
  assert(chmod(fixture.path.c_str(), 0500) == 0);
  assert(nvs_commit(write) == ESP_FAIL);
  assert(chmod(fixture.path.c_str(), 0700) == 0);
  size_t size = 0;
  nvs_handle_t ignored;
  assert(nvs_get_blob(write, "old", nullptr, &size) == ESP_ERR_INVALID_STATE);
  assert(nvs_open("mc-onchip", NVS_READONLY, &ignored) == ESP_ERR_INVALID_STATE);
  assert(nvs_commit(write) == ESP_ERR_INVALID_STATE);
  native_nvs_shutdown();
  fixture.init();
  auto reader = open("mc-onchip", NVS_READONLY);
  assert(get(reader, "old") == std::vector<uint8_t>{1});
  assert(nvs_get_blob(reader, "other", nullptr, &size) == ESP_ERR_NVS_NOT_FOUND);
  nvs_close(reader);
}
void interrupted_commit_recovery() {
  Fixture fixture(9);
  fixture.init();
  auto writer = open("mc-onchip");
  set(writer, "command-bot", std::vector<uint8_t>(64, 9));
  assert(nvs_commit(writer) == ESP_OK);
  nvs_close(writer);
  native_nvs_shutdown();
  const std::string orphan = fixture.path + "/nvs.snapshot.tmp.crashed";
  int fd = ::open(orphan.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  assert(fd >= 0 && close(fd) == 0);
  fixture.init();
  auto reader = open("mc-onchip", NVS_READONLY);
  assert(get(reader, "command-bot") == std::vector<uint8_t>(64, 9));
  nvs_close(reader);
  assert(access(orphan.c_str(), F_OK) == -1 && errno == ENOENT);
  native_nvs_shutdown();
  assert(symlink("nvs.snapshot", orphan.c_str()) == 0);
  assert(native_nvs_init(fixture.path.c_str()) == ESP_ERR_INVALID_STATE);
  assert(unlink(orphan.c_str()) == 0);
}
void bot_identity_is_a_volatile_hello_binding() {
  Fixture fixture(10);
  uint8_t expanded[64]{};
  expanded[31] = 0x40;
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded)) == ESP_ERR_INVALID_STATE);
  fixture.init();
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded) - 1) == ESP_ERR_INVALID_ARG);
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded)) == ESP_OK);
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded)) == ESP_OK);
  auto reader = open("mc-onchip", NVS_READONLY);
  assert(get(reader, "command-bot") == std::vector<uint8_t>(expanded, expanded + 64));
  nvs_close(reader);
  native_nvs_shutdown();
  fixture.init();
  expanded[63] = 1;
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded)) == ESP_OK);
  reader = open("mc-onchip", NVS_READONLY);
  assert(get(reader, "command-bot") == std::vector<uint8_t>(expanded, expanded + 64));
  nvs_close(reader);
  auto writer = open("mc-onchip");
  assert(nvs_set_blob(writer, "command-bot", expanded, sizeof(expanded)) == ESP_ERR_INVALID_STATE);
  set(writer, "policy", {1});
  assert(nvs_commit(writer) == ESP_OK);
  nvs_close(writer);
  expanded[63] = 2;
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded)) == ESP_ERR_INVALID_STATE);
  native_nvs_shutdown();
  fixture.init();
  reader = open("mc-onchip", NVS_READONLY);
  size_t size = 0;
  assert(nvs_get_blob(reader, "command-bot", nullptr, &size) == ESP_ERR_NVS_NOT_FOUND);
  nvs_close(reader);
  expanded[0] = 1;
  assert(native_nvs_bind_bot_identity(expanded, sizeof(expanded)) == ESP_ERR_INVALID_ARG);
}
} // namespace

int main() {
  persistence();
  transactions();
  reclaim_one_key();
  corruption();
  paths_and_names();
  headroom_and_threads();
  native_partition_admission_boundary();
  failed_commit_stays_failed();
  interrupted_commit_recovery();
  bot_identity_is_a_volatile_hello_binding();
  std::puts("native NVS tests passed");
}
