// SPDX-License-Identifier: Apache-2.0
#include "SPIFFS.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr char Staged[] = "/command-bot/staged.lua";
constexpr char Upload[] = "/command-bot/upload.lua";
constexpr char Slot[] = "/command-bot/a.lua";

struct Fixture {
  std::string path, alias;
  explicit Fixture(unsigned number) {
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    path = std::string(cwd) + "/.spiffs-fixture-" + std::to_string(getpid()) +
           "-" + std::to_string(number);
    alias = path + "-alias";
    assert(mkdir(path.c_str(), 0700) == 0);
  }
  ~Fixture() {
    native_spiffs_shutdown();
    assert(unlink(alias.c_str()) == 0 || errno == ENOENT);
    DIR *entries = opendir(path.c_str());
    assert(entries);
    while (auto *entry = readdir(entries)) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      assert(unlink((path + "/" + entry->d_name).c_str()) == 0);
    }
    assert(closedir(entries) == 0 && rmdir(path.c_str()) == 0);
  }
  void init() const {
    assert(native_spiffs_init(path.c_str()));
    assert(!native_spiffs_faulted());
  }
  std::string disk(const char *path) const {
    return this->path + "/spiffs." + (path + strlen("/command-bot/"));
  }
};

void write(const char *path, const std::string &text) {
  auto file = SPIFFS.open(path, "w");
  assert(file);
  assert(file.write(reinterpret_cast<const uint8_t *>(text.data()), text.size()) ==
         text.size());
  file.flush();
  file.close();
  assert(!native_spiffs_faulted());
}

std::string read(const char *path) {
  auto file = SPIFFS.open(path, "r");
  assert(file && !file.isDirectory());
  std::string result(file.size(), '\0');
  assert(file.read(reinterpret_cast<uint8_t *>(result.data()), result.size()) ==
         result.size());
  assert(file.read() == -1);
  file.close();
  assert(!native_spiffs_faulted());
  return result;
}

void persistence_and_publication() {
  Fixture f(1);
  assert(native_spiffs_faulted());
  assert(!SPIFFS.open(Staged, "r"));
  f.init();
  assert(SPIFFS.begin() && !SPIFFS.begin(true));
  assert(!native_spiffs_init(f.path.c_str()));
  assert(!SPIFFS.exists(Staged) && !native_spiffs_faulted());
  write(Staged, std::string(4096, 's'));
  struct stat st{};
  assert(stat(f.disk(Staged).c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
         (st.st_mode & 07777) == 0600 && st.st_size == 4112);
  assert(SPIFFS.usedBytes() == 4096 && SPIFFS.totalBytes() >= SPIFFS.usedBytes());
  native_spiffs_shutdown();
  f.init();
  assert(read(Staged) == std::string(4096, 's'));
  assert(SPIFFS.usedBytes() == 4096);
}

void overwrite_and_chunk_update() {
  Fixture f(2);
  f.init();
  write(Upload, "hello");
  auto writer = SPIFFS.open(Upload, "w");
  assert(writer && !SPIFFS.open(Staged, "w"));
  assert(writer.write(reinterpret_cast<const uint8_t *>("new"), 3) == 3);
  assert(read(Upload) == "hello");
  writer.close();
  assert(read(Upload) == "new");
  auto interrupted = SPIFFS.open(Upload, "w");
  assert(interrupted.write(reinterpret_cast<const uint8_t *>("lost"), 4) == 4);
  native_spiffs_shutdown();
  interrupted.close();
  f.init();
  assert(read(Upload) == "new");

  auto chunk = SPIFFS.open(Upload, "r+");
  assert(chunk && chunk.seek(3) && chunk.write(reinterpret_cast<const uint8_t *>("!"), 1) == 1);
  assert(read(Upload) == "new");
  chunk.close();
  assert(read(Upload) == "new!");
  auto check = SPIFFS.open(Upload, "r");
  assert(check.seek(3) && check.read() == '!' && check.position() == 4);
  assert(!check.seek(5) && !check.seek(1, SeekEnd));
  check.close();
}

void paths_and_permissions() {
  Fixture f(3);
  assert(!native_spiffs_init("relative"));
  assert(!native_spiffs_init((f.path + "/../" + f.path.substr(f.path.rfind('/') + 1)).c_str()));
  assert(symlink(f.path.c_str(), f.alias.c_str()) == 0);
  assert(!native_spiffs_init(f.alias.c_str()));
  assert(chmod(f.path.c_str(), 0755) == 0);
  assert(!native_spiffs_init(f.path.c_str()));
  assert(chmod(f.path.c_str(), 0700) == 0);
  f.init();
  assert(!SPIFFS.open("/command-bot/../other", "w"));
  assert(native_spiffs_faulted());
  native_spiffs_shutdown();
  f.init();
  assert(!SPIFFS.open("/other/staged.lua", "r"));
  assert(native_spiffs_faulted());
  native_spiffs_shutdown();
  f.init();
  assert(!SPIFFS.open("/command-bot/x/child", "r"));
  assert(native_spiffs_faulted());
  native_spiffs_shutdown();
  f.init();
  assert(symlink("nowhere", f.disk(Staged).c_str()) == 0);
  assert(!SPIFFS.open(Staged, "r") && native_spiffs_faulted());
  native_spiffs_shutdown();
  assert(!native_spiffs_init(f.path.c_str()));
  assert(unlink(f.disk(Staged).c_str()) == 0);
  f.init();
  write(Staged, "untouched");
  assert(chmod(f.path.c_str(), 0500) == 0);
  assert(!SPIFFS.open(Staged, "r") && native_spiffs_faulted());
  assert(chmod(f.path.c_str(), 0700) == 0);
  native_spiffs_shutdown();
  f.init();
  assert(read(Staged) == "untouched");
  assert(chmod(f.disk(Staged).c_str(), 0644) == 0);
  assert(!SPIFFS.exists(Staged) && native_spiffs_faulted());
}

void limits_and_failed_publication() {
  Fixture f(4);
  f.init();
  const std::string maximum(32 * 1024, 'a');
  write(Staged, maximum);
  assert(read(Staged) == maximum && SPIFFS.usedBytes() == maximum.size());
  auto writer = SPIFFS.open(Staged, "w");
  std::vector<uint8_t> excessive(maximum.size() + 1, 'a');
  assert(writer.write(excessive.data(), excessive.size()) == 0);
  assert(native_spiffs_faulted());
  writer.close();
  native_spiffs_shutdown();
  f.init();
  assert(read(Staged) == maximum);
  auto another = SPIFFS.open(Staged, "w");
  assert(another.write(reinterpret_cast<const uint8_t *>("new"), 3) == 3);
  assert(chmod(f.path.c_str(), 0500) == 0);
  another.flush();
  another.close();
  assert(native_spiffs_faulted());
  assert(chmod(f.path.c_str(), 0700) == 0);
  native_spiffs_shutdown();
  f.init();
  assert(read(Staged) == maximum);
  assert(!SPIFFS.begin(true) && !SPIFFS.format());
}

void rename_remove_and_corruption() {
  Fixture f(5);
  f.init();
  write(Upload, "source");
  write(Slot, "old");
  assert(SPIFFS.rename(Upload, Slot));
  assert(!SPIFFS.exists(Upload) && read(Slot) == "source");
  assert(SPIFFS.usedBytes() == 6);
  native_spiffs_shutdown();
  f.init();
  assert(!SPIFFS.exists(Upload) && read(Slot) == "source");
  assert(!SPIFFS.remove(Upload) && !native_spiffs_faulted());
  assert(SPIFFS.remove(Slot) && !SPIFFS.exists(Slot));
  native_spiffs_shutdown();
  f.init();
  assert(!SPIFFS.exists(Slot) && SPIFFS.usedBytes() == 0);
  write(Staged, "source");
  native_spiffs_shutdown();
  int fd = open(f.disk(Staged).c_str(), O_RDWR | O_NOFOLLOW);
  assert(fd >= 0);
  uint8_t byte = 0;
  assert(pread(fd, &byte, 1, 16) == 1);
  byte ^= 1;
  assert(pwrite(fd, &byte, 1, 16) == 1 && fsync(fd) == 0 && close(fd) == 0);
  assert(!native_spiffs_init(f.path.c_str()) && native_spiffs_faulted());
}

void capacity_and_read_faults() {
  Fixture f(6);
  f.init();
  const std::string full(4096, 'x');
  for (unsigned i = 0; i < 32; ++i)
    write(("/command-bot/file" + std::to_string(i)).c_str(), full);
  assert(SPIFFS.usedBytes() == SPIFFS.totalBytes());
  assert(!SPIFFS.open("/command-bot/file32", "w"));
  assert(!native_spiffs_faulted());
  auto append = SPIFFS.open("/command-bot/file0", "r+");
  assert(append && append.seek(4096));
  assert(append.write(uint8_t('!')) == 0 && native_spiffs_faulted());
  append.close();
  native_spiffs_shutdown();
  f.init();
  assert(read("/command-bot/file0") == full);
  auto reader = SPIFFS.open("/command-bot/file0", "r");
  assert(reader && reader.size() == 4096);
  int fd = open(f.disk("/command-bot/file0").c_str(), O_RDWR | O_NOFOLLOW);
  assert(fd >= 0 && ftruncate(fd, 16) == 0 && close(fd) == 0);
  uint8_t bytes[8]{};
  assert(reader.read(bytes, sizeof(bytes)) == 0 && native_spiffs_faulted());
  reader.close();
  native_spiffs_shutdown();
  assert(!native_spiffs_init(f.path.c_str()) && native_spiffs_faulted());
}

void interrupted_write_recovery() {
  Fixture f(7);
  f.init();
  write(Staged, "committed");
  native_spiffs_shutdown();
  const std::string orphan = f.path + "/.spiffs-tmp.crashed";
  int fd = open(orphan.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  assert(fd >= 0 && close(fd) == 0);
  f.init();
  assert(read(Staged) == "committed");
  assert(access(orphan.c_str(), F_OK) == -1 && errno == ENOENT);
  native_spiffs_shutdown();
  assert(symlink("spiffs.staged.lua", orphan.c_str()) == 0);
  assert(!native_spiffs_init(f.path.c_str()) && native_spiffs_faulted());
}

void refuse_rename_over_symlink() {
  Fixture f(7);
  f.init();
  write(Staged, "good");
  assert(symlink("missing", f.disk(Slot).c_str()) == 0);
  assert(!SPIFFS.rename(Staged, Slot) && native_spiffs_faulted());
  native_spiffs_shutdown();
  assert(unlink(f.disk(Slot).c_str()) == 0);
  f.init();
  assert(read(Staged) == "good");
}

void refuse_untracked_files() {
  Fixture f(8);
  f.init();
  int fd = open(f.disk(Staged).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  assert(fd >= 0 && close(fd) == 0);
  assert(!SPIFFS.open(Staged, "w") && native_spiffs_faulted());
  native_spiffs_shutdown();
  assert(!native_spiffs_init(f.path.c_str()) && native_spiffs_faulted());
}
} // namespace

int main() {
  persistence_and_publication();
  overwrite_and_chunk_update();
  paths_and_permissions();
  limits_and_failed_publication();
  rename_remove_and_corruption();
  capacity_and_read_faults();
  interrupted_write_recovery();
  refuse_rename_over_symlink();
  refuse_untracked_files();
  std::puts("spiffs tests passed");
}
