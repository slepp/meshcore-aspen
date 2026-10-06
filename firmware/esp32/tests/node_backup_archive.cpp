// SPDX-License-Identifier: Apache-2.0
#include "NodeBackupArchive.h"
#include "NodeBackup.h"
#include <cassert>
#include <fstream>
#include <string>
#include <vector>

using namespace onchip::backup;
struct MemorySink : Sink {
  std::vector<uint8_t> bytes;
  size_t limit = SIZE_MAX;
  bool write(const uint8_t *data, size_t size) override {
    if (size > limit - bytes.size()) return false;
    bytes.insert(bytes.end(), data, data + size);
    return true;
  }
};
struct MemoryReader : Reader {
  std::vector<uint8_t> bytes;
  size_t offset = 0, largestRead = 0;
  explicit MemoryReader(const std::string &text) : bytes(text.begin(), text.end()) {}
  size_t read(uint8_t *out, size_t count) override {
    largestRead = std::max(largestRead, count);
    count = std::min(count, bytes.size() - offset);
    memcpy(out, bytes.data() + offset, count); offset += count;
    return count;
  }
};
struct Platform : onchip::NodeBackupPlatform {
  std::vector<uint8_t> staged, saved;
  uint8_t ephemeral[32]{}, recipient[32]{}, shared[32]{}, nonce[16]{}, digest[32]{};
  uint32_t clock = 0;
  bool exists = false, wakeRequested = false, failReads = false;
  bool available() const override { return true; }
  void wake() override { wakeRequested = true; }
  bool beginOutput(char *, size_t) override { staged.clear(); return true; }
  bool publish(const uint8_t hash[32], char *, size_t) override {
    saved = staged; staged.clear(); exists = true; memcpy(digest, hash, 32); return true;
  }
  bool remove(bool published) override {
    if (published) { saved.clear(); exists = false; } else staged.clear();
    return true;
  }
  bool size(uint32_t &bytes, uint8_t hash[32]) override { bytes = saved.size(); memcpy(hash, digest, 32); return exists; }
  size_t read(uint32_t offset, uint8_t *bytes, size_t size) override {
    if (failReads || offset > saved.size() || size > saved.size() - offset) return 0;
    memcpy(bytes, saved.data() + offset, size); return size;
  }
  bool write(const uint8_t *bytes, size_t size) override {
    staged.insert(staged.end(), bytes, bytes + size); return true;
  }
  bool snapshot(TarWriter &archive, char *, size_t) override {
    MemoryReader manifest("{\"schema_version\":1,\"format\":\"meshcore-node-backup\",\"platform\":\"test\"}");
    return archive.add("manifest.json", manifest.bytes.size(), manifest);
  }
  bool seal(uint8_t out[32], uint8_t iv[16], const uint8_t target[32], uint8_t secret[32]) override {
    assert(!memcmp(target, recipient, 32));
    memcpy(out, ephemeral, 32); memcpy(iv, nonce, 16); memcpy(secret, shared, 32); return true;
  }
  uint32_t now() const override { return clock; }
};
int main(int argc, char **argv) {
  assert(argc == 5);
  uint8_t ephemeral[32], recipient[32], shared[32], nonce[16];
  const auto decode = [](const char *hex, uint8_t *out, size_t size) {
    assert(strlen(hex) == size * 2);
    for (size_t i = 0; i < size; ++i) {
      unsigned value = 0;
      assert(sscanf(hex + 2 * i, "%2x", &value) == 1);
      out[i] = value;
    }
  };
  decode(argv[2], ephemeral, 32); decode(argv[3], recipient, 32); decode(argv[4], shared, 32);
  for (unsigned i = 0; i < sizeof(nonce); ++i) nonce[i] = i;
  MemorySink output;
  SealedSink sealed(output);
  assert(sealed.begin(ephemeral, recipient, nonce, shared));
  TarWriter archive(sealed);
  MemoryReader manifest("{\"schema_version\":1,\"format\":\"meshcore-node-backup\",\"platform\":\"test\"}");
  assert(archive.add("manifest.json", manifest.bytes.size(), manifest));
  MemoryReader config(std::string(2048, '\0'));
  for (unsigned i = 0; i < 256; ++i) config.bytes[i] = i;
  assert(archive.add("nvs/mc-onchip/companion.blob", config.bytes.size(), config));
  assert(config.largestRead <= 512);
  MemoryReader source("--@meshcore-bot/1;name=backup-fixture\nreturn 'hello'\n");
  assert(archive.add("files/command-bot/a.lua", source.bytes.size(), source));
  assert(archive.finish() && sealed.finish());
  assert(archive.entries() == 3 && archive.rawBytes() == 5632);
  assert(output.bytes.size() < 1024);
  assert(!archive.finish() && !sealed.finish());
  assert(!sealed.begin(ephemeral, recipient, nonce, shared));
  std::ofstream file(argv[1], std::ios::binary);
  file.write(reinterpret_cast<const char *>(output.bytes.data()), output.bytes.size());
  assert(file.good());
  for (const char *name : {"", "/", "../key", "files/../key", "./key", "files//key", "files/key/", "files\\key"})
    assert(!TarWriter::validName(name));
  assert(!TarWriter::validName(std::string(100, 'a').c_str()));
  MemorySink plain;
  TarWriter invalid(plain);
  MemoryReader empty("");
  assert(!invalid.add("../key", 0, empty));
  assert(!invalid.add("files/huge", RawLimit, empty));
  assert(!invalid.add("files/truncated", 1, empty));
  assert(!invalid.finish());
  MemorySink failing;
  failing.limit = HeaderBytes;
  SealedSink failed(failing);
  assert(failed.begin(ephemeral, recipient, nonce, shared));
  const uint8_t byte = 1;
  assert(!failed.write(&byte, 1) && !failed.finish());
  Platform platform;
  memcpy(platform.ephemeral, ephemeral, 32); memcpy(platform.recipient, recipient, 32);
  memcpy(platform.shared, shared, 32); memcpy(platform.nonce, nonce, 16);
  onchip::NodeBackup service;
  service.begin(platform);
  char reply[163], command[163], id[17]{};
  snprintf(command, sizeof(command), "start %s", argv[3]);
  service.command(command, reply, sizeof(reply), true);
  assert(platform.wakeRequested && service.active());
  service.work();
  service.command("status", reply, sizeof(reply), true);
  assert(sscanf(reply, "READY %16s", id) == 1 && platform.exists);
  snprintf(command, sizeof(command), "read %s 0", id);
  service.command(command, reply, sizeof(reply), true);
  assert(!strncmp(reply, "CHUNK ", 6));
  service.command(command, reply, sizeof(reply), true);
  assert(!strcmp(reply, "WAIT ms=5000"));
  service.command(command, reply, sizeof(reply), false);
  assert(!strncmp(reply, "CHUNK ", 6));
  platform.clock = onchip::NodeBackup::RfPacingMs;
  service.command(command, reply, sizeof(reply), true);
  assert(!strncmp(reply, "CHUNK ", 6));
  assert(service.beginRead(id));
  service.command("clear", reply, sizeof(reply), false);
  assert(!strncmp(reply, "Error:", 6) && platform.exists);
  service.endRead();
  const auto saved = platform.saved;
  snprintf(command, sizeof(command), "start %s", argv[3]);
  service.command(command, reply, sizeof(reply), false);
  service.command("cancel", reply, sizeof(reply), false);
  service.work();
  service.command("status", reply, sizeof(reply), false);
  assert(strstr(reply, "cancelled") && platform.saved == saved && platform.staged.empty());
  service.command("load", reply, sizeof(reply), false);
  service.work();
  assert(service.beginRead(id));
  service.endRead();
  onchip::NodeBackup restarted;
  restarted.begin(platform);
  restarted.command("status", reply, sizeof(reply), false);
  restarted.work();
  assert(restarted.beginRead(id));
  restarted.endRead();
  platform.failReads = true;
  snprintf(command, sizeof(command), "read %s 0", id);
  restarted.command(command, reply, sizeof(reply), false);
  assert(!strcmp(reply, "Error: backup file read failed"));
  platform.failReads = false;
  restarted.command("clear", reply, sizeof(reply), false);
  assert(!platform.exists && !strcmp(reply, "Saved backup removed; device settings unchanged"));
  puts("PASS bounded tar/RLE, sealed archive, unsafe names, truncated reads and failed writes");
}
