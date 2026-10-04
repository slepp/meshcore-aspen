// SPDX-License-Identifier: Apache-2.0
#include "SPIFFS.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr char Prefix[] = "/command-bot/";
constexpr char DiskPrefix[] = "spiffs.";
constexpr char TempPrefix[] = ".spiffs-tmp.";
constexpr uint8_t Magic[8] = {'M', 'C', 'F', 'S', '\r', '\n', 0, 1};
constexpr size_t HeaderSize = 16, MaxFile = 32 * 1024, MaxFiles = 32;
constexpr size_t Capacity = 128 * 1024;

std::mutex guard;
int directory = -1;
bool fault = false, writing = false, scanning = false;
uint64_t generation = 0, temp_sequence = 0;
std::string active_temp;
std::map<std::string, size_t> committed;

void poison() { fault = true; }

bool ready() {
  if (directory < 0 || fault) return false;
  struct stat st{};
  if (fstat(directory, &st) || !S_ISDIR(st.st_mode) ||
      (st.st_mode & 07777) != 0700 || st.st_uid != geteuid()) {
    poison();
    return false;
  }
  return true;
}

bool name_ok(const std::string &name) {
  if (name.empty() || name.size() > 31 || name[0] == '.' ||
      name.find("..") != std::string::npos) return false;
  for (unsigned char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return false;
  return true;
}

bool disk_name(const char *path, std::string &name) {
  if (!path || std::strncmp(path, Prefix, sizeof(Prefix) - 1)) {
    poison();
    return false;
  }
  const std::string leaf(path + sizeof(Prefix) - 1);
  if (!name_ok(leaf)) {
    poison();
    return false;
  }
  name = std::string(DiskPrefix) + leaf;
  return true;
}

int open_directory(const char *path) {
  if (!path || path[0] != '/') return -1;
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  const std::string full(path);
  size_t at = 1;
  while (at < full.size()) {
    const size_t end = full.find('/', at);
    const std::string part = full.substr(at, end - at);
    if (part.empty() || part == "." || part == "..") {
      ::close(fd);
      return -1;
    }
    const int child = openat(fd, part.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(fd);
    if (child < 0) return -1;
    fd = child;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  struct stat st{};
  if (full == "/" || full.back() == '/' || fstat(fd, &st) ||
      !S_ISDIR(st.st_mode) || (st.st_mode & 07777) != 0700 ||
      st.st_uid != geteuid() || flock(fd, LOCK_EX | LOCK_NB)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool file_ok(int fd, size_t &length) {
  struct stat st{};
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
      (st.st_mode & 07777) != 0600 || st.st_uid != geteuid() ||
      st.st_nlink != 1 || st.st_size < static_cast<off_t>(HeaderSize) ||
      st.st_size > static_cast<off_t>(HeaderSize + MaxFile)) return false;
  length = static_cast<size_t>(st.st_size) - HeaderSize;
  return true;
}

bool read_exact(int fd, void *buffer, size_t length, off_t offset) {
  auto *bytes = static_cast<uint8_t *>(buffer);
  while (length) {
    const ssize_t n = pread(fd, bytes, length, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n;
    offset += n;
    length -= static_cast<size_t>(n);
  }
  return true;
}

bool write_exact(int fd, const void *buffer, size_t length, off_t offset) {
  auto *bytes = static_cast<const uint8_t *>(buffer);
  while (length) {
    const ssize_t n = pwrite(fd, bytes, length, offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n;
    offset += n;
    length -= static_cast<size_t>(n);
  }
  return true;
}

uint32_t crc32(const uint8_t *bytes, size_t length, uint32_t crc = ~uint32_t(0)) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc;
}

void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t get32(const uint8_t *p) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8 * i);
  return value;
}

bool check_contents(int fd, size_t &length) {
  if (!file_ok(fd, length)) return false;
  uint8_t header[HeaderSize], buffer[512];
  if (!read_exact(fd, header, sizeof(header), 0) ||
      std::memcmp(header, Magic, sizeof(Magic)) ||
      get32(header + 8) != length) return false;
  uint32_t crc = ~uint32_t(0);
  for (size_t at = 0; at < length;) {
    const size_t n = std::min(sizeof(buffer), length - at);
    if (!read_exact(fd, buffer, n, HeaderSize + at)) return false;
    crc = crc32(buffer, n, crc);
    at += n;
  }
  size_t after = 0;
  return file_ok(fd, after) && after == length && get32(header + 12) == ~crc;
}

// Missing files are normal. Invalid existing entries or I/O errors are not.
int checked_open(const std::string &name, size_t &length) {
  const int fd = openat(directory, name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno != ENOENT || committed.count(name)) poison();
    return -1;
  }
  if (!scanning && !committed.count(name)) {
    poison();
    ::close(fd);
    return -1;
  }
  if (!check_contents(fd, length)) {
    poison();
    ::close(fd);
    return -1;
  }
  return fd;
}

bool scan() {
  scanning = true;
  const int copy = dup(directory);
  if (copy < 0) { scanning = false; return false; }
  DIR *listing = fdopendir(copy);
  if (!listing) { ::close(copy); scanning = false; return false; }
  bool ok = true;
  std::vector<std::string> stale;
  errno = 0;
  while (auto *entry = readdir(listing)) {
    const std::string name(entry->d_name);
    if (name.compare(0, sizeof(TempPrefix) - 1, TempPrefix) == 0) {
      struct stat st{};
      if (fstatat(directory, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) ||
          !S_ISREG(st.st_mode) || (st.st_mode & 07777) != 0600 ||
          st.st_uid != geteuid() || st.st_nlink != 1 ||
          st.st_size > static_cast<off_t>(HeaderSize + MaxFile)) {
        ok = false;
        break;
      }
      stale.push_back(name);
      errno = 0;
      continue;
    }
    if (name.compare(0, sizeof(DiskPrefix) - 1, DiskPrefix) != 0) {
      errno = 0;
      continue;
    }
    if (!name_ok(name.substr(sizeof(DiskPrefix) - 1))) { ok = false; break; }
    size_t length = 0;
    const int fd = checked_open(name, length);
    if (fd < 0) { ok = false; break; }
    if (::close(fd)) { ok = false; break; }
    committed[name] = length;
    if (committed.size() > MaxFiles) { ok = false; break; }
    errno = 0;
  }
  if (errno) ok = false;
  if (closedir(listing)) ok = false;
  if (ok && !stale.empty()) {
    for (const auto &name : stale)
      if (unlinkat(directory, name.c_str(), 0)) { ok = false; break; }
    if (ok) ok = fsync(directory) == 0;
    if (ok) std::fprintf(stderr, "Host bot recovered %zu uncommitted source files\n", stale.size());
  }
  scanning = false;
  return ok;
}

size_t used() {
  size_t bytes = 0;
  for (const auto &entry : committed) bytes += entry.second;
  return bytes;
}
} // namespace

namespace hostfs {
struct File::Impl {
  int fd;
  std::string name, temp;
  size_t length, offset = 0;
  uint64_t epoch;
  bool writable;
};

} // namespace hostfs

bool native_spiffs_init(const char *path) {
  std::lock_guard<std::mutex> lock(guard);
  if (directory >= 0) return false;
  const int fd = open_directory(path);
  if (fd < 0) return false;
  directory = fd;
  fault = false;
  committed.clear();
  if (!scan() || used() > Capacity) {
    ::close(directory);
    directory = -1;
    committed.clear();
    fault = true;
    return false;
  }
  ++generation;
  return true;
}

void native_spiffs_shutdown() {
  std::lock_guard<std::mutex> lock(guard);
  if (directory >= 0) {
    if (!active_temp.empty()) unlinkat(directory, active_temp.c_str(), 0);
    ::close(directory);
  }
  directory = -1;
  writing = false;
  active_temp.clear();
  committed.clear();
  fault = false;
  ++generation;
}

bool native_spiffs_faulted() {
  std::lock_guard<std::mutex> lock(guard);
  return directory < 0 || fault;
}

namespace hostfs {
File::File() = default;
File::File(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
File::~File() { close(); }
File::File(File &&) noexcept = default;
File &File::operator=(File &&other) noexcept {
  if (this != &other) { close(); impl_ = std::move(other.impl_); }
  return *this;
}

File::operator bool() const {
  std::lock_guard<std::mutex> lock(guard);
  return impl_ && impl_->fd >= 0 && impl_->epoch == generation && ready();
}

size_t File::read(uint8_t *data, size_t length) {
  std::lock_guard<std::mutex> lock(guard);
  if (!impl_ || impl_->writable || !ready() || impl_->epoch != generation) return 0;
  if (length && !data) { poison(); return 0; }
  size_t actual = 0;
  if (!check_contents(impl_->fd, actual) || actual != impl_->length) {
    poison();
    return 0;
  }
  if (length > impl_->length - impl_->offset)
    length = impl_->length - impl_->offset;
  if (!length) return 0;
  if (!read_exact(impl_->fd, data, length, HeaderSize + impl_->offset) ||
      !check_contents(impl_->fd, actual) || actual != impl_->length) {
    poison();
    return 0;
  }
  impl_->offset += length;
  return length;
}

int File::read() {
  uint8_t byte;
  return read(&byte, 1) == 1 ? byte : -1;
}

size_t File::write(const uint8_t *data, size_t length) {
  std::lock_guard<std::mutex> lock(guard);
  if (!impl_ || !impl_->writable || !ready() || impl_->epoch != generation) return 0;
  if (length && !data) { poison(); return 0; }
  const auto current = committed.find(impl_->name);
  const size_t old = current == committed.end() ? 0 : current->second;
  if (impl_->offset > MaxFile || length > MaxFile - impl_->offset ||
      length > Capacity - (used() - old + impl_->offset)) {
    poison();
    return 0;
  }
  if (!write_exact(impl_->fd, data, length, HeaderSize + impl_->offset)) {
    poison();
    return 0;
  }
  impl_->offset += length;
  impl_->length = std::max(impl_->length, impl_->offset);
  return length;
}
size_t File::write(uint8_t data) { return write(&data, 1); }
size_t File::size() const {
  std::lock_guard<std::mutex> lock(guard);
  if (!impl_ || !ready() || impl_->epoch != generation) return 0;
  if (!impl_->writable) {
    size_t actual = 0;
    if (!check_contents(impl_->fd, actual) || actual != impl_->length) {
      poison();
      return 0;
    }
  }
  return impl_->length;
}
size_t File::position() const {
  std::lock_guard<std::mutex> lock(guard);
  return impl_ && ready() && impl_->epoch == generation ? impl_->offset : 0;
}
bool File::seek(uint32_t position, SeekMode mode) {
  std::lock_guard<std::mutex> lock(guard);
  if (!impl_ || !ready() || impl_->epoch != generation) return false;
  const size_t base = mode == SeekSet ? 0 : mode == SeekCur ? impl_->offset : impl_->length;
  if (base + position > impl_->length) return false;
  impl_->offset = base + position;
  return true;
}
bool File::isDirectory() const { return false; }
void File::flush() {
  std::lock_guard<std::mutex> lock(guard);
  if (impl_ && impl_->writable && impl_->epoch == generation && ready() &&
      fsync(impl_->fd)) poison();
}
void File::close() {
  std::lock_guard<std::mutex> lock(guard);
  if (!impl_) return;
  auto &file = *impl_;
  if (file.writable && file.epoch == generation && directory >= 0) {
    size_t actual = 0;
    if (!ready() || !file_ok(file.fd, actual) || actual != file.length) poison();
    struct stat opened{}, staged{};
    if (!fault &&
        (fstat(file.fd, &opened) ||
         fstatat(directory, file.temp.c_str(), &staged, AT_SYMLINK_NOFOLLOW) ||
         !S_ISREG(staged.st_mode) || staged.st_dev != opened.st_dev ||
         staged.st_ino != opened.st_ino || staged.st_nlink != 1))
      poison();
    if (!fault) {
      uint32_t crc = ~uint32_t(0);
      uint8_t buffer[512];
      for (size_t at = 0; at < file.length;) {
        const size_t n = std::min(sizeof(buffer), file.length - at);
        if (!read_exact(file.fd, buffer, n, HeaderSize + at)) { poison(); break; }
        crc = crc32(buffer, n, crc);
        at += n;
      }
      uint8_t header[HeaderSize];
      std::memcpy(header, Magic, sizeof(Magic));
      put32(header + 8, static_cast<uint32_t>(file.length));
      put32(header + 12, ~crc);
      if (!fault && (!write_exact(file.fd, header, sizeof(header), 0) ||
                     fsync(file.fd))) poison();
    }
    if (::close(file.fd)) poison();
    file.fd = -1;
    if (!fault) {
      size_t previous = 0;
      const int prior = checked_open(file.name, previous);
      if (prior >= 0 && ::close(prior)) poison();
    }
    if (!fault && renameat(directory, file.temp.c_str(), directory, file.name.c_str()))
      poison();
    if (!fault && fsync(directory)) poison();
    bool cleaned = true;
    if (!fault) committed[file.name] = file.length;
    else if (unlinkat(directory, file.temp.c_str(), 0) && errno != ENOENT)
      cleaned = false;
    writing = false;
    if (cleaned) active_temp.clear();
  } else if (::close(file.fd)) {
    if (file.epoch == generation) poison();
  }
  impl_.reset();
}

File SPIFFSFS::open(const char *path, const char *mode) {
  std::lock_guard<std::mutex> lock(guard);
  if (!ready()) return {};
  std::string name;
  if (!disk_name(path, name) || !mode) { poison(); return {}; }
  const bool read = !std::strcmp(mode, "r");
  const bool write = !std::strcmp(mode, "w");
  const bool update = !std::strcmp(mode, "r+");
  if (!read && !write && !update) { poison(); return {}; }
  size_t old_length = 0;
  if (read) {
    const int fd = checked_open(name, old_length);
    if (fd < 0) return {};
    return File(std::make_unique<File::Impl>(File::Impl{fd, name, "", old_length, 0, generation, false}));
  }
  if (writing || (committed.size() >= MaxFiles && !committed.count(name)))
    return {};
  if (update && !committed.count(name)) return {};
  if (!committed.count(name)) {
    struct stat unexpected{};
    if (fstatat(directory, name.c_str(), &unexpected, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
      poison();
      return {};
    }
  }
  if (committed.count(name)) {
    const int existing = checked_open(name, old_length);
    if (existing < 0) return {};
    if (::close(existing)) { poison(); return {}; }
  }
  int fd = -1;
  std::string temp;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    temp = std::string(TempPrefix) + std::to_string(getpid()) + "." +
           std::to_string(++temp_sequence);
    fd = openat(directory, temp.c_str(), O_RDWR | O_CREAT | O_EXCL |
                O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) { poison(); return {}; }
  bool ok = fchmod(fd, 0600) == 0;
  uint8_t blank[HeaderSize]{};
  if (ok) ok = write_exact(fd, blank, sizeof(blank), 0);
  if (ok && update) {
    size_t len = 0;
    const int source = checked_open(name, len);
    if (source < 0) ok = false;
    else {
      uint8_t buffer[512];
      for (size_t at = 0; ok && at < len;) {
        const size_t n = std::min(sizeof(buffer), len - at);
        ok = read_exact(source, buffer, n, HeaderSize + at) &&
             write_exact(fd, buffer, n, HeaderSize + at);
        at += n;
      }
      if (::close(source)) ok = false;
    }
  }
  if (!ok) {
    poison();
    ::close(fd);
    unlinkat(directory, temp.c_str(), 0);
    return {};
  }
  writing = true;
  active_temp = temp;
  return File(std::make_unique<File::Impl>(
      File::Impl{fd, name, temp, update ? old_length : 0, 0, generation, true}));
}

bool SPIFFSFS::exists(const char *path) {
  std::lock_guard<std::mutex> lock(guard);
  if (!ready()) return false;
  std::string name;
  if (!disk_name(path, name)) return false;
  size_t length;
  const int fd = checked_open(name, length);
  if (fd < 0) return false;
  if (::close(fd)) { poison(); return false; }
  return true;
}

bool SPIFFSFS::remove(const char *path) {
  std::lock_guard<std::mutex> lock(guard);
  if (!ready() || writing) return false;
  std::string name;
  if (!disk_name(path, name)) return false;
  size_t length;
  const int fd = checked_open(name, length);
  if (fd < 0) return false;
  if (::close(fd) || unlinkat(directory, name.c_str(), 0) || fsync(directory)) {
    poison();
    return false;
  }
  committed.erase(name);
  return true;
}

bool SPIFFSFS::rename(const char *from, const char *to) {
  std::lock_guard<std::mutex> lock(guard);
  if (!ready() || writing) return false;
  std::string src, dst;
  if (!disk_name(from, src) || !disk_name(to, dst)) return false;
  size_t length;
  const int input = checked_open(src, length);
  if (input < 0) return false;
  if (::close(input)) { poison(); return false; }
  if (src == dst) return true;
  if (committed.size() >= MaxFiles && !committed.count(dst)) return false;
  size_t replaced;
  const int target = checked_open(dst, replaced);
  if (target >= 0 && ::close(target)) { poison(); return false; }
  if (fault || renameat(directory, src.c_str(), directory, dst.c_str()) ||
      fsync(directory)) {
    poison();
    return false;
  }
  committed.erase(src);
  committed[dst] = length;
  return true;
}

size_t SPIFFSFS::totalBytes() const { return Capacity; }
size_t SPIFFSFS::usedBytes() const {
  std::lock_guard<std::mutex> lock(guard);
  return ready() ? used() : 0;
}

} // namespace hostfs

namespace {
void unsupported_operation() {
  std::lock_guard<std::mutex> lock(guard);
  poison();
}

class HostFileImpl final : public fs::FileImpl {
  hostfs::File file_;
  std::string path_;

public:
  HostFileImpl(hostfs::File file, const char *path) :
      file_(std::move(file)), path_(path) {}
  size_t write(const uint8_t *data, size_t size) override { return file_.write(data, size); }
  size_t read(uint8_t *data, size_t size) override { return file_.read(data, size); }
  void flush() override { file_.flush(); }
  bool seek(uint32_t position, SeekMode mode) override { return file_.seek(position, mode); }
  size_t position() const override { return file_.position(); }
  size_t size() const override { return file_.size(); }
  bool setBufferSize(size_t) override { return !native_spiffs_faulted(); }
  void close() override { file_.close(); }
  time_t getLastWrite() override { unsupported_operation(); return 0; }
  const char *path() const override { return path_.c_str(); }
  const char *name() const override { return path_.c_str(); }
  boolean isDirectory() override { return false; }
  fs::FileImplPtr openNextFile(const char *) override { unsupported_operation(); return {}; }
  boolean seekDir(long) override { unsupported_operation(); return false; }
  String getNextFileName() override { unsupported_operation(); return {}; }
  String getNextFileName(bool *dir) override {
    if (dir) *dir = false;
    unsupported_operation();
    return {};
  }
  void rewindDirectory() override { unsupported_operation(); }
  operator bool() override { return bool(file_); }
};

class HostFSImpl final : public fs::FSImpl {
  hostfs::SPIFFSFS backend_;

public:
  fs::FileImplPtr open(const char *path, const char *mode, bool) override {
    if (!path || !mode) { unsupported_operation(); return {}; }
    auto file = backend_.open(path, mode);
    if (!file) return {};
    return std::make_shared<HostFileImpl>(std::move(file), path);
  }
  bool exists(const char *path) override { return backend_.exists(path); }
  bool remove(const char *path) override { return backend_.remove(path); }
  bool rename(const char *from, const char *to) override { return backend_.rename(from, to); }
  bool mkdir(const char *) override { unsupported_operation(); return false; }
  bool rmdir(const char *) override { unsupported_operation(); return false; }
};
} // namespace

fs::SPIFFSFS::SPIFFSFS() : FS(std::make_shared<HostFSImpl>()) {}
size_t fs::SPIFFSFS::totalBytes() const { return hostfs::SPIFFSFS{}.totalBytes(); }
size_t fs::SPIFFSFS::usedBytes() const { return hostfs::SPIFFSFS{}.usedBytes(); }

fs::SPIFFSFS SPIFFS;
