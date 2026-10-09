// SPDX-License-Identifier: Apache-2.0
#include "EspNodeBackup.h"
#include "NodeBackup.h"
#include "RoleIdentity.h"
#include <SPIFFS.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <cstdio>
#include <string>

unsigned long millis() { return 0; }
void delay(unsigned long) {}
namespace onchip {
bool backupWorkerReady() { return true; }
void wakeBackupWorker() {}
void HardwareRNG::random(uint8_t *data, size_t size) {
  for (size_t i = 0; i < size; ++i) data[i] = uint8_t(i + 1);
}
}
namespace {
unsigned directories = 0;
bool reverseFiles = false;
void (*beforeVerifyFiles)() = nullptr;
class Directory final : public filesystem_test::FileImpl {
  std::vector<std::string> paths_;
  size_t offset_ = 0;
public:
  Directory() : FileImpl("/", false) {
    if (directories == 1 && beforeVerifyFiles) beforeVerifyFiles();
    for (const auto &entry : filesystem_test::files) paths_.push_back(entry.first);
    if (++directories % 2 == 0 && reverseFiles)
      std::reverse(paths_.begin(), paths_.end());
  }
  fs::FileImplPtr openNextFile(const char *) override {
    if (offset_ == paths_.size()) return {};
    return std::make_shared<filesystem_test::FileImpl>(paths_[offset_++], false);
  }
};
class Filesystem final : public filesystem_test::Impl {
public:
  size_t totalBytes() const override { return 4 * 1024 * 1024; }
  fs::FileImplPtr open(const char *path, const char *mode, bool create) override {
    if (!strcmp(path, "/")) return std::make_shared<Directory>();
    return Impl::open(path, mode, create);
  }
};
std::string command(const char *text) {
  char reply[163]{};
  onchip::nodeBackup().command(text + 7, reply, sizeof(reply), false);
  return reply;
}
std::string snapshot() {
  assert(command("backup start 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8")
         .find("PREPARING") == 0);
  onchip::nodeBackup().work();
  return command("backup status");
}
void seed() {
  identity_test::durable.clear();
  filesystem_test::files.clear();
  backup_test::inventories = directories = 0;
  identity_test::durable[{"mc-onchip", "companion"}] = {1, 2, 3};
  identity_test::durable[{"mc-mast-admin", "settings"}] = {4, 5, 6};
  filesystem_test::files["/command-bot/a.lua"] = {7, 8, 9};
  filesystem_test::files["/command-bot/b.lua"] = {10, 11, 12};
  filesystem_test::files["/command-bot/upload.stage"] = {13};
  filesystem_test::files["/command-bot/upload.tmp"] = {14};
  onchip::beginEspNodeBackup();
}
}
int main() {
  SPIFFS.setDriver(std::make_shared<Filesystem>());
  seed();
  const auto baseline = snapshot();
  assert(baseline.find("READY ") == 0);
  seed();
  reverseFiles = true;
  const auto reorderedFiles = snapshot();
  printf("Reordered filesystem: %s\n", reorderedFiles.c_str());
  assert(reorderedFiles.find("READY ") == 0);
  seed();
  reverseFiles = false;
  backup_test::reverseNvs = true;
  const auto reorderedNvs = snapshot();
  printf("Reordered NVS: %s\n", reorderedNvs.c_str());
  assert(reorderedNvs.find("READY ") == 0);
  backup_test::reverseNvs = false;
  const auto checkFailure = [](const char *expected) {
    seed();
    const auto nvsHook = backup_test::beforeVerifyNvs;
    const auto filesHook = beforeVerifyFiles;
    backup_test::beforeVerifyNvs = beforeVerifyFiles = nullptr;
    const auto saved = snapshot();
    assert(saved.find("READY ") == 0);
    backup_test::beforeVerifyNvs = nvsHook;
    beforeVerifyFiles = filesHook;
    backup_test::inventories = directories = 0;
    const auto failed = snapshot();
    assert(failed.find(expected) != std::string::npos);
    assert(command("backup load").find("PREPARING") == 0);
    onchip::nodeBackup().work();
    assert(command("backup status") == saved);
  };
  backup_test::beforeVerifyNvs = [] {
    identity_test::durable.at({"mc-onchip", "companion"})[0] ^= 1;
  };
  checkFailure("NVS settings changed");
  backup_test::beforeVerifyNvs = [] {
    identity_test::durable[{"mc-onchip", "new"}] = {15};
  };
  checkFailure("NVS settings changed");
  backup_test::beforeVerifyNvs = [] {
    identity_test::durable.erase({"mc-onchip", "companion"});
  };
  checkFailure("NVS settings changed");
  backup_test::beforeVerifyNvs = nullptr;
  beforeVerifyFiles = [] { filesystem_test::files.at("/command-bot/a.lua")[0] ^= 1; };
  checkFailure("Saved files changed");
  beforeVerifyFiles = [] { filesystem_test::files.at("/command-bot/a.lua").push_back(15); };
  checkFailure("Saved files changed");
  beforeVerifyFiles = [] { filesystem_test::files.erase("/command-bot/a.lua"); };
  checkFailure("Saved files changed");
  beforeVerifyFiles = [] {
    filesystem_test::files["/command-bot/c.lua"] = filesystem_test::files.at("/command-bot/a.lua");
    filesystem_test::files.erase("/command-bot/a.lua");
  };
  checkFailure("Saved files changed");
  beforeVerifyFiles = [] { filesystem_test::files["/command-bot/c.lua"] = {15}; };
  checkFailure("Saved files changed");
  beforeVerifyFiles = [] {
    filesystem_test::files["/command-bot/upload.stage"].push_back(15);
    filesystem_test::files["/command-bot/upload.tmp"].push_back(16);
  };
  seed();
  assert(snapshot().find("READY ") == 0);
  beforeVerifyFiles = nullptr;
  backup_test::beforeVerifyNvs = [] {
    identity_test::durable[{"mc-mast-admin", "owner-replay"}] = {17};
  };
  seed();
  assert(snapshot().find("READY ") == 0);
  backup_test::beforeVerifyNvs = nullptr;
  seed();
  const auto captured = snapshot();
  backup_test::inventories = directories = 0;
  backup_test::beforeVerifyNvs = [] {
    filesystem_test::afterWrite = [] {
      filesystem_test::afterWrite = nullptr;
      filesystem_test::files.at("/command-bot/a.lua")[0] ^= 1;
      identity_test::durable.at({"mc-onchip", "companion"})[0] ^= 1;
    };
  };
  assert(snapshot() == captured);
  assert(filesystem_test::files.at("/command-bot/a.lua")[0] == (7 ^ 1));
  backup_test::beforeVerifyNvs = nullptr;
  for (int allocation = 0; allocation < 9; ++allocation) {
    seed();
    psram_test::failAfter = allocation;
    const auto failed = snapshot();
    psram_test::failAfter = -1;
    assert(failed.find("Error:") == 0);
    assert(psram_test::allocations.empty());
  }
  seed();
  filesystem_test::files["/empty"] = {};
  assert(snapshot().find("READY ") == 0);
  assert(psram_test::allocations.empty());
  seed();
  for (unsigned i = 0; i < 507; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "/boundary/%04u", i);
    filesystem_test::files[path] = {};
  }
  const auto maximum = snapshot();
  assert(maximum.find("READY ") == 0);
  filesystem_test::files["/boundary/extra"] = {};
  assert(snapshot().find("record limit exceeded") != std::string::npos);
  assert(command("backup load").find("PREPARING") == 0);
  onchip::nodeBackup().work();
  assert(command("backup status") == maximum);
  seed();
  filesystem_test::files["/oversize"] = std::vector<uint8_t>(onchip::backup::RawLimit);
  assert(snapshot().find("size or record limit exceeded") != std::string::npos);
  assert(psram_test::allocations.empty());
  puts("PASS ESP backup: reordered inventories, settings/content/size/path/add/remove changes, staging exclusion, replay counters and retained saved snapshot");
  puts("PASS immutable capture: live changes during output retain captured bytes; allocation failures and empty files release scrubbed PSRAM");
  puts("PASS archive boundaries: 512 records accepted; extra records and oversized data rejected without replacing saved backup");
}
