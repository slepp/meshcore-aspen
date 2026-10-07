// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastSource.h"
#include "BotSignal.h"
#if (defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE) || defined(NRF52_PLATFORM)
#include "CommandBot.h"
namespace onchip { CommandBot &commandBotService(); }
#else
#include "Runtime.h"
#include "CommandBot.h"
#endif
#include "BotHttps.h"
#include "BotRegistry.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <stdio.h>
#include <algorithm>
#include <ctype.h>
#include <new>

namespace onchip {
namespace {
constexpr char UploadPath[] = "/command-bot/upload.lua";
const char *const Slots[] = {"/command-bot/a.lua", "/command-bot/b.lua", "/command-bot/c.lua"};
constexpr char PackagePrefix[] = "--@meshcore-bot/1;";
constexpr const char *PackageFields[] = {
  "name=", "version=", "runtime=", "api=", "caps=", "schema=", "rollback="
};
class PackageSourceSink final : public BotHttpsBodySink {
public:
  void select(const char *path) { path_ = path; }
  bool begin(uint32_t length) override {
    if (active_ || length < 1 || length > BotSourceLimit) return false;
    committed_ = false;
    expected_ = written_ = 0;
    cleanupOk_ = true;
    memset(digest_, 0, sizeof(digest_));
    if (file_) file_.close();
    if (SPIFFS.exists(path_) && !SPIFFS.remove(path_)) {
      cleanupOk_ = false;
      return false;
    }
    file_ = SPIFFS.open(path_, "w");
    if (!file_) return false;
    expected_ = length;
    written_ = 0;
    active_ = true;
    return true;
  }
  bool write(const uint8_t *bytes, size_t size) override {
    if (!active_ || !file_ || !bytes || !size || size > expected_ - written_ ||
        file_.write(bytes, size) != size) return false;
    written_ += size;
    return true;
  }
  bool commit(uint32_t bytes, const uint8_t sha256[32]) override {
    if (!active_ || !file_ || !sha256 || bytes != expected_ || written_ != expected_)
      return false;
    file_.flush();
    file_.close();
    auto readback = SPIFFS.open(path_, "r");
    if (!readback || readback.size() != bytes) {
      if (readback) readback.close();
      return false;
    }
    readback.close();
    memcpy(digest_, sha256, sizeof(digest_));
    active_ = false;
    committed_ = true;
    return true;
  }
  void abort(const char *reason) override {
    (void)reason;
    if (file_) file_.close();
    cleanupOk_ = !SPIFFS.exists(path_) || SPIFFS.remove(path_);
    active_ = committed_ = false;
    expected_ = written_ = 0;
    memset(digest_, 0, sizeof(digest_));
  }
  bool cleanupOk() const { return cleanupOk_; }
  bool committed(size_t bytes, const uint8_t sha256[32]) const {
    return committed_ && bytes == expected_ && sha256 &&
        !memcmp(sha256, digest_, sizeof(digest_));
  }
private:
  const char *path_ = UploadPath;
  File file_;
  size_t expected_ = 0, written_ = 0;
  bool active_ = false, committed_ = false, cleanupOk_ = true;
  uint8_t digest_[32]{};
};
PackageSourceSink packageSourceSink;
bool abortPackageSource(const char *reason) {
  packageSourceSink.abort(reason);
  return packageSourceSink.cleanupOk();
}
struct PackageFetchState {
  bool pending = false, cancelling = false;
  bool wasm = false;
  uint32_t id = 0, nextId = 0;
  uint8_t hash[32]{};
} packageFetch;
void clearPackageFetch() {
  packageFetch.pending = packageFetch.cancelling = false;
  packageFetch.id = 0;
  memset(packageFetch.hash, 0, sizeof(packageFetch.hash));
}
bool decode(const char *text, uint8_t *bytes, size_t size) {
  if (strlen(text) != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = text[i * 2 + j];
      const int n = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (n < 0) return false;
      value = value * 16 + n;
    }
    bytes[i] = value;
  }
  return true;
}
bool packageName(const char *text) {
  if (!text || !text[0] || strlen(text) > 24 || text[0] < 'a' || text[0] > 'z') return false;
  for (const char *p = text + 1; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
  return true;
}
bool semanticVersion(const char *text) {
  if (!text || !text[0] || strlen(text) > 23) return false;
  unsigned components = 0;
  const char *p = text;
  while (*p) {
    const char *start = p;
    while (*p >= '0' && *p <= '9') ++p;
    if (p == start || (p - start > 1 && *start == '0') || ++components > 3) return false;
    if (!*p) break;
    if (*p++ != '.') return false;
    if (!*p) return false;
  }
  return components == 3;
}
bool schemaName(const char *text) {
  if (!text || !strcmp(text, "none")) return text != nullptr;
  const char *separator = strchr(text, '@');
  if (!separator || separator == text || !separator[1] || separator - text > 24) return false;
  char name[25]{};
  memcpy(name, text, size_t(separator - text));
  if (!packageName(name)) return false;
  if (separator[1] == '0') return false;
  unsigned digits = 0;
  for (const char *p = separator + 1; *p; ++p) {
    if (*p < '0' || *p > '9' || ++digits > 5) return false;
  }
  return true;
}
bool packageCapabilities(const char *text) {
  if (!text || !text[0]) return false;
  if (!strcmp(text, "none")) return true;
  static constexpr const char *Supported[] = {
    "cmdmeta", "events", "https", "kv", "kv.atomic", "mesh", "mesh-chan", "mesh-dest", "modules",
    "reminders", "timers", "utilities"
  };
  char previous[24]{};
  const char *start = text;
  while (*start) {
    const char *end = strchr(start, ',');
    const size_t size = end ? size_t(end - start) : strlen(start);
    if (!size || size >= sizeof(previous)) return false;
    char value[24]{};
    memcpy(value, start, size);
    bool known = false;
    for (const auto *supported : Supported) known = known || !strcmp(value, supported);
#if !ONCHIP_BOT_HTTPS
    if (!strcmp(value, "https")) known = false;
#endif
    if (!known || (previous[0] && strcmp(previous, value) >= 0)) return false;
    strcpy(previous, value);
    if (!end) break;
    if (!end[1]) return false;
    start = end + 1;
  }
  return true;
}
bool parsePackageHeader(char *line, MastPackageMetadata &metadata, char *error, size_t capacity) {
  metadata = {};
  if (strncmp(line, "--@meshcore-bot", 15)) return true;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(line); *p; ++p) {
    if (*p < 32 || *p > 126) {
      snprintf(error, capacity, "package metadata header must be printable ASCII");
      return false;
    }
  }
  if (strncmp(line, PackagePrefix, sizeof(PackagePrefix) - 1)) {
    snprintf(error, capacity, "unsupported package metadata format");
    return false;
  }
  const char *cursor = line + sizeof(PackagePrefix) - 1;
  char *outputs[] = {metadata.name, metadata.version, metadata.runtime, metadata.api,
                     metadata.capabilities, metadata.schema, metadata.rollback};
  const size_t limits[] = {sizeof(metadata.name), sizeof(metadata.version), sizeof(metadata.runtime),
                           sizeof(metadata.api), sizeof(metadata.capabilities), sizeof(metadata.schema),
                           sizeof(metadata.rollback)};
  for (size_t i = 0; i < sizeof(PackageFields) / sizeof(PackageFields[0]); ++i) {
    const size_t keySize = strlen(PackageFields[i]);
    if (strncmp(cursor, PackageFields[i], keySize)) {
      snprintf(error, capacity, "package metadata field order/name invalid");
      return false;
    }
    cursor += keySize;
    const char *end = strchr(cursor, ';');
    if ((i + 1 < sizeof(PackageFields) / sizeof(PackageFields[0])) != (end != nullptr)) {
      snprintf(error, capacity, "package metadata field separator invalid");
      return false;
    }
    const size_t valueSize = end ? size_t(end - cursor) : strlen(cursor);
    if (!valueSize || valueSize >= limits[i] || memchr(cursor, ' ', valueSize)) {
      snprintf(error, capacity, "package metadata value invalid or too long");
      return false;
    }
    memcpy(outputs[i], cursor, valueSize);
    if (!end) cursor += valueSize;
    else cursor = end + 1;
  }
  if (*cursor || !packageName(metadata.name) || !semanticVersion(metadata.version) ||
      ((!strcmp(metadata.runtime, "lua-5.5.1") && strcmp(metadata.api, "named-commands-v1")) ||
       (!strcmp(metadata.runtime, "wamr-2.4.1") && strcmp(metadata.api, "meshcore-v1")) ||
       (strcmp(metadata.runtime, "lua-5.5.1") && strcmp(metadata.runtime, "wamr-2.4.1"))) ||
      !packageCapabilities(metadata.capabilities) || !schemaName(metadata.schema) ||
      !schemaName(metadata.rollback)) {
    snprintf(error, capacity, "package runtime/API/capability/schema metadata unsupported");
    return false;
  }
  if (!strcmp(metadata.runtime, "wamr-2.4.1")) {
#if !ONCHIP_BOT_WASM
    snprintf(error, capacity, "Wasm runtime unavailable in this build"); return false;
#endif
    for (const char *unavailable : {"modules"})
      if (strstr(metadata.capabilities, unavailable)) {
        snprintf(error, capacity, "Wasm required capability unavailable"); return false;
      }
  }
  metadata.present = true;
  return true;
}
bool packageHeader(const char *source, size_t size, MastPackageMetadata &metadata,
                   char *error, size_t capacity) {
  char line[256]{};
  size_t length = 0;
  while (length < sizeof(line) - 1 && length < size && source[length] != '\n') {
    const unsigned char c = static_cast<unsigned char>(source[length]);
    line[length] = char(c);
    ++length;
  }
  if (length == sizeof(line) - 1 && length < size && source[length] != '\n' &&
      !strncmp(line, "--@meshcore-bot", 15)) {
    snprintf(error, capacity, "package metadata header exceeds 255 bytes");
    return false;
  }
  if (!parsePackageHeader(line, metadata, error, capacity)) return false;
  if (!metadata.runtime[0]) strcpy(metadata.runtime, botSourceIsWasm(source, size) ? "wamr-2.4.1" : "lua-5.5.1");
  return true;
}
bool inspectMetadataFile(const char *path, size_t expectedSize, MastPackageMetadata &metadata,
                         char *error, size_t capacity) {
  auto file = SPIFFS.open(path, "r");
  if (!file) {
    snprintf(error, capacity, "source file unavailable");
    return false;
  }
  if (file.size() != expectedSize) {
    snprintf(error, capacity, "source size changed: expected=%u actual=%u",
             unsigned(expectedSize), unsigned(file.size()));
    return false;
  }
  char line[256]{};
  size_t length = 0;
  while (length < sizeof(line) - 1 && length < expectedSize) {
    const int value = file.read();
    if (value < 0) {
      file.close();
      snprintf(error, capacity, "source metadata read incomplete");
      return false;
    }
    if (value == '\n') break;
    line[length++] = char(value);
  }
  file.close();
  if (length == sizeof(line) - 1 && length < expectedSize &&
      !strncmp(line, "--@meshcore-bot", 15)) {
    snprintf(error, capacity, "package metadata header exceeds 255 bytes");
    return false;
  }
  if (!parsePackageHeader(line, metadata, error, capacity)) return false;
  if (!metadata.runtime[0]) strcpy(metadata.runtime, botSourceIsWasm(line, length) ? "wamr-2.4.1" : "lua-5.5.1");
  return true;
}
}
const char *MastSource::slotPath(uint8_t slot) const {
  static const char *const wasm[] = {"/command-bot/wa.lua", "/command-bot/wb.lua", "/command-bot/wc.lua"};
  return wasmRuntime_ ? wasm[slot] : Slots[slot];
}
const char *MastSource::uploadPath() const { return wasmRuntime_ ? "/command-bot/wupload.lua" : UploadPath; }
const char *MastSource::journalKey() const { return wasmRuntime_ ? "wasm-source" : "source"; }
size_t MastSource::bundledSize() const { return wasmRuntime_ ? 0 : strlen(BotDefaultSource); }
#if ONCHIP_BOT_SINGLE_SESSION
BotWorker &MastSource::validator() { return commandBotService().sourceWorker(); }
bool MastSource::pollValidation(BotWorker::Result &result) {
  return commandBotService().pollSourceResult(result);
}
#endif
void MastSource::fail(const char *message) {
  phase_ = Phase::Idle;
  stopValidation();
  if (publication_ && !publicationDurable_) {
    commandBotService().releaseSourcePublication(publication_);
    publication_ = 0;
  }
  offlinePublication_ = false;
#if ONCHIP_BOT_SINGLE_SESSION
  if (validator().sourceSuspended()) {
    if (validator().recoverSource()) phase_ = Phase::Recover;
  } else commandBotService().setRuntimeAdmission(wasmRuntime_, true);
#endif
  snprintf(outcome_, sizeof(outcome_), "Error: %.183s", message);
  Serial.printf("Mast source: %s\n", outcome_);
}
void MastSource::liveFailed(const char *message) {
  phase_ = Phase::Idle;
  auto &bot = commandBotService();
  if (publication_ && !bot.sourcePublicationCurrent(publication_)) {
    bot.releaseSourcePublication(publication_);
    publication_ = 0;
  }
#if ONCHIP_BOT_SINGLE_SESSION
  bot.setRuntimeAdmission(wasmRuntime_, false);
  if (validator().sourceSuspended() && validator().recoverSource()) phase_ = Phase::Recover;
  else if (!validator().sourceSuspended()) bot.setRuntimeAdmission(wasmRuntime_, true);
#else
  if (bot.sourceReady()) bot.setRuntimeAdmission(wasmRuntime_, !startupPending_);
#endif
  livePending_ = ++liveRetries_ <= 3;
  retryAt_ = millis() + 1000 * liveRetries_;
  snprintf(outcome_, sizeof(outcome_), "Error: %.119s; %s",
           message, livePending_ ? "live retry pending" :
           startupPending_ ?
             (wasmRuntime_ ? "use source wasm retry or source wasm remove; startup blocked" :
                             "use source retry or source remove; startup blocked") :
             (wasmRuntime_ ? "use source wasm retry; prior live retained" : "use source retry; prior live retained"));
  Serial.printf("Mast source: %s\n", outcome_);
}
const char *MastSource::journalError(const Journal &journal) {
  const auto &deployment = journal.deployment;
  const auto &upload = journal.upload;
  if (deployment.version != 1 || deployment.active > Bundled || deployment.previous > Bundled ||
      upload.version != 1 || upload.size > BotSourceLimit ||
      upload.received > upload.size || upload.id[16])
    return "source journal invalid";
  for (unsigned i = 0; i < 4; ++i)
    if ((i < 3 && deployment.sizes[i] > BotSourceLimit) ||
        !memchr(deployment.help[i], 0, sizeof(deployment.help[i])))
      return "source size/help invalid";
  return nullptr;
}
bool MastSource::begin() {
#if ONCHIP_BOT_WASM
  if (!wasmRuntime_ && !wasmSource_) {
    wasmSource_ = new (std::nothrow) MastSource(true);
    if (wasmSource_) wasmSource_->begin();
    else fail("Wasm source state unavailable; restart to recover");
  }
#else
  if (!wasmRuntime_) {
    Journal wasmJournal;
    bool present;
    wasmJournalBlocked_ = !mastRecord("wasm-source", &wasmJournal, sizeof(wasmJournal), false, present) ||
        journalError(wasmJournal) ||
        wasmJournal.deployment.active != Bundled;
    if (wasmJournalBlocked_)
      Serial.println("Mast source: retained Wasm selection/journal unavailable; restore Wasm-enabled firmware");
  }
#endif
  const bool loaded = beginDeployment();
  publishReadiness();
  return loaded;
}
bool MastSource::beginDeployment() {
  startupPending_ = true;
  liveActive_ = false;
  livePending_ = false;
  sealed_ = false;
  bool present;
  Journal journal;
  const bool loaded = mastRecord(journalKey(), &journal, sizeof(journal), false, present);
  deployed_ = journal.deployment;
  upload_ = journal.upload;
  const char *error = loaded ? journalError(journal) : "source journal invalid";
  if (error) {
    sealed_ = true; deployed_ = {}; upload_ = {};
    char message[128]{};
    snprintf(message, sizeof(message), "%s; use source%s retry or explicit source%s remove",
             error, wasmRuntime_ ? " wasm" : "", wasmRuntime_ ? " wasm" : "");
    fail(message);
    return false;
  }
  MastPackageMetadata active;
  char metadataError[96]{};
  if (!metadata(deployed_.active, active, metadataError, sizeof(metadataError))) {
    liveFailed(metadataError);
    return true;
  }
  livePending_ = deployed_.active != Bundled;
  liveActive_ = !livePending_;
  startupPending_ = livePending_;
  if (livePending_) strcpy(outcome_, "durable source waiting for bot startup");
  return true;
}
void MastSource::publishReadiness() {
  if (wasmRuntime_) return;
  const bool luaReady = !sealed_ && liveActive_ && !livePending_ && phase_ == Phase::Idle;
  bool wasmReady = !wasmJournalBlocked_, wasmBlocked = wasmJournalBlocked_;
  const char *fault = (sealed_ || startupPending_) ? outcome_ : "";
#if ONCHIP_BOT_WASM
  wasmReady = wasmSource_ && !wasmSource_->sealed_ && wasmSource_->liveActive_ &&
      !wasmSource_->livePending_ && wasmSource_->phase_ == Phase::Idle;
  wasmBlocked = !wasmSource_ || wasmSource_->sealed_ || wasmSource_->startupPending_;
  if (!fault[0] && wasmBlocked)
    fault = wasmSource_ ? wasmSource_->outcome_ : "Error: Wasm source state unavailable; restart to recover";
#else
  if (!fault[0] && wasmBlocked)
    fault = "Error: retained Wasm selection/journal unavailable; restore Wasm-enabled firmware";
#endif
  commandBotService().setSourceDeploymentState(luaReady, sealed_ || startupPending_,
                                               wasmReady, wasmBlocked, fault);
}
bool MastSource::save(const Deployment &deployment, const Upload &upload) {
  Journal next{deployment, upload};
  bool present;
  return mastRecord(journalKey(), &next, sizeof(next), true, present);
}
bool MastSource::offlineLuaPublicationCurrent() const {
  // A disabled bot has no live namespace. Only an empty Wasm journal permits
  // Lua's existing offline-save path without loading another guest program.
  return !wasmRuntime_ && !commandBotService().publicKey() &&
      (!wasmSource_ || (!wasmSource_->sealed_ && wasmSource_->deployed_.active == Bundled &&
       wasmSource_->phase_ == Phase::Idle && !wasmSource_->livePending_ && !wasmSource_->publication_));
}
void MastSource::stop() {
  if (packageFetch.pending && !packageFetch.cancelling) {
    commandBotService().cancelPackageFetch();
    packageFetch.cancelling = true;
  }
  stopValidation();
  if (publication_) commandBotService().releaseSourcePublication(publication_);
  publication_ = 0; publicationDurable_ = false; offlinePublication_ = false;
  delete wasmSource_; wasmSource_ = nullptr;
}
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
bool MastSource::ready() const {
  return !sealed_ && phase_ == Phase::Idle && !livePending_ &&
         liveActive_ && commandBotService().sourceReady() &&
         (!wasmSource_ || wasmSource_->ready()) &&
         commandBotService().sourceDeploymentReady();
}
bool MastSource::recoveryRequired() const {
  return sealed_ || wasmJournalBlocked_ || (phase_ == Phase::Idle && !livePending_ && !liveActive_) ||
      (wasmSource_ && wasmSource_->recoveryRequired());
}
#endif
bool MastSource::startCopy(const char *from, const char *to, size_t size, Phase phase) {
#if ONCHIP_BOT_SINGLE_SESSION
  commandBotService().setRuntimeAdmission(wasmRuntime_, false);
  commandBotService().cancelJobs();
  snprintf(copyFrom_, sizeof(copyFrom_), "%s", from ? from : "");
  snprintf(copyTo_, sizeof(copyTo_), "%s", to);
  copySize_ = size; copyPhase_ = phase;
  copyDeadline_ = millis() + 5000;
  phase_ = Phase::AwaitCopy;
#else
  if (!validator().begin()) { fail("copy worker unavailable"); return false; }
  if (!validator().copyFile(from, to, size)) {
    stopValidation(); fail("copy admission failed"); return false;
  }
  phase_ = phase;
#endif
  strcpy(outcome_, phase == Phase::CopyLive ? "source selected; live copy pending" :
                                            "copying bounded source; activation not committed");
  return true;
}
bool MastSource::prepareFile(uint8_t slot, Phase phase) {
  return startCopy(slot < Bundled ? slotPath(slot) : nullptr, BotStagedSourcePath,
                   slot < Bundled ? deployed_.sizes[slot] : bundledSize(), phase);
}
bool MastSource::startValidation(uint8_t candidate) {
  candidate_ = candidate;
#if !ONCHIP_BOT_SINGLE_SESSION
  if (!validator().begin()) { fail("validation worker unavailable"); return false; }
#endif
  uint8_t hash[32];
  size_t size;
  if (candidate == Bundled) {
    size = strlen(BotDefaultSource);
    mesh::Utils::sha256(hash, sizeof(hash), reinterpret_cast<const uint8_t *>(BotDefaultSource), size);
  } else if (candidate == deployed_.previous) {
    size = deployed_.sizes[candidate];
    memcpy(hash, deployed_.hashes[candidate], 32);
  } else {
    size = upload_.size;
    memcpy(hash, upload_.hash, 32);
  }
  if (!validator().stageFile(size, hash)) {
    stopValidation(); fail("validation admission failed"); return false;
  }
  phase_ = Phase::Validating;
  strcpy(outcome_, "verifying source; activation not committed");
  return true;
}
bool MastSource::metadata(uint8_t slot, MastPackageMetadata &value, char *error, size_t capacity) const {
  value = {};
  if (slot == Bundled) {
    if (wasmRuntime_) {
      value.bundled = true; strcpy(value.schema, "none"); strcpy(value.rollback, "none"); return true;
    }
    if (!packageHeader(BotDefaultSource, strlen(BotDefaultSource), value, error, capacity)) return false;
    value.bundled = true;
    if (!value.present) {
      strcpy(value.schema, "none");
      strcpy(value.rollback, "none");
    }
    return true;
  }
  if (slot >= 3 || !deployed_.sizes[slot]) {
    snprintf(error, capacity, "source metadata slot unavailable");
    return false;
  }
  char detail[96]{};
  if (!inspectMetadataFile(slotPath(slot), deployed_.sizes[slot], value, detail, sizeof(detail))) {
    snprintf(error, capacity, "%.27s: %.55s", slotPath(slot), detail);
    return false;
  }
  if (wasmRuntime_ != !strcmp(value.runtime, "wamr-2.4.1")) {
    snprintf(error, capacity, "durable package runtime does not match source selector");
    return false;
  }
  if (!value.present) {
    strcpy(value.schema, "none");
    strcpy(value.rollback, "none");
  }
  return true;
}
bool MastSource::compatible(const MastPackageMetadata &active, const MastPackageMetadata &candidate,
                            bool rollback, char *error, size_t capacity) const {
  if (!candidate.bundled && (wasmRuntime_ != !strcmp(candidate.runtime, "wamr-2.4.1"))) {
    snprintf(error, capacity, "package runtime does not match source selector"); return false;
  }
  if (rollback) {
    if (active.present) {
      if ((candidate.present || candidate.bundled) &&
          (!strcmp(active.schema, candidate.schema) ||
           !strcmp(active.rollback, candidate.schema)))
        return true;
      snprintf(error, capacity, "rollback schema is not declared compatible by the active package");
      return false;
    }
    if (candidate.present && !active.bundled) {
      snprintf(error, capacity, "plain Lua source has no schema contract for package rollback");
      return false;
    }
    return true;
  }
  if (!candidate.present) {
    if (active.present) {
      snprintf(error, capacity, "plain Lua source cannot replace a metadata package");
      return false;
    }
    return true;
  }
  if (!active.present && !active.bundled) {
    snprintf(error, capacity, "plain Lua source has unknown data schema; restore bundled source first");
    return false;
  }
  if (strcmp(active.schema, candidate.schema) && strcmp(candidate.rollback, active.schema)) {
    snprintf(error, capacity, "schema change requires rollback=%.39s", active.schema);
    return false;
  }
  return true;
}
bool MastSource::acceptPackageFetch(const BotHttpsFetchResult &result) {
#if ONCHIP_BOT_HTTPS
  if (result.state != BotHttpsFetchResult::Complete || !result.bytes ||
      result.bytes > BotSourceLimit ||
      memcmp(result.sha256, packageFetch.hash, sizeof(packageFetch.hash)) ||
      !packageSourceSink.committed(result.bytes, result.sha256)) {
    if (!abortPackageSource("package sink completion mismatch"))
      fail("package staging cleanup failed; active source retained");
    else
      fail("streamed package bytes/hash do not match the accepted fetch");
    return false;
  }
  if (upload_.size) {
    if (!abortPackageSource("source upload already exists"))
      fail("package staging cleanup failed; active source retained");
    else
      fail("cancel the existing source upload before package fetch");
    return false;
  }
  MastPackageMetadata active, candidate;
  char error[96]{};
  if (!metadata(deployed_.active, active, error, sizeof(error)) ||
      !inspectMetadataFile(uploadPath(), result.bytes, candidate, error, sizeof(error)) ||
      !candidate.present ||
      !compatible(active, candidate, false, error, sizeof(error))) {
    if (!abortPackageSource("incompatible package metadata"))
      fail("package staging cleanup failed; active source retained");
    else
      fail(error[0] ? error : "streamed source requires a compatible package manifest");
    return false;
  }
  for (candidate_ = 0; candidate_ < Bundled; ++candidate_)
    if (candidate_ != deployed_.active && candidate_ != deployed_.previous) break;
  if (candidate_ >= Bundled) {
    if (!abortPackageSource("no source slot available"))
      fail("package staging cleanup failed; active source retained");
    else
      fail("no source slot available for streamed package");
    return false;
  }
  Upload next;
  next.size = next.received = result.bytes;
  memcpy(next.hash, result.sha256, sizeof(next.hash));
  for (unsigned i = 0; i < 8; ++i)
    snprintf(next.id + i * 2, sizeof(next.id) - i * 2, "%02x", result.sha256[i]);
  if (!save(deployed_, next)) {
    if (!abortPackageSource("source journal commit failed"))
      fail("package staging cleanup failed; active source retained");
    else
      fail("streamed package source journal commit failed");
    return false;
  }
  upload_ = next;
  removing_ = false;
  if (!startCopy(uploadPath(), BotStagedSourcePath, result.bytes, Phase::CopyCandidate))
    return false;
  snprintf(outcome_, sizeof(outcome_), "package %.16s received; verifying before activation",
           upload_.id);
  return true;
#else
  (void)result;
  fail("streaming package fetch unavailable in this build");
  return false;
#endif
}
void MastSource::execute(const char *input, char *reply, size_t capacity) {
  const auto respond = [&](const char *text) { snprintf(reply, capacity, "%s", text); };
  if (!wasmRuntime_ && !strncmp(input, "wasm ", 5)) {
    if (!wasmSource_) {
      respond(wasmJournalBlocked_ ?
          "Error: retained Wasm selection/journal unavailable; restore Wasm-enabled firmware" :
          "Error: Wasm runtime unavailable in this build");
      return;
    }
    const char *command = input + 5;
    const bool readOnly = !strcmp(command, "status") || !strncmp(command, "api", 3) ||
        !strcmp(command, "hash") || !strcmp(command, "metadata") || !strcmp(command, "help");
    if (!readOnly && (phase_ != Phase::Idle || livePending_ || publication_ ||
        (!liveActive_ && strcmp(command, "remove") && strcmp(command, "retry")))) {
      respond("Error: Lua source activation busy or pending; apply or remove it first"); return;
    }
    wasmSource_->execute(command, reply, capacity);
    publishReadiness();
    return;
  }
  if (!wasmRuntime_ && !strcmp(input, "api runtimes")) {
#if ONCHIP_BOT_WASM
    respond("Runtimes lua-5.5.1/named-commands-v1 wamr-2.4.1/meshcore-v1 portable=wasm32 memory=65536 stack=8192 fuel=10000");
#else
    respond("Runtimes lua-5.5.1/named-commands-v1");
#endif
    return;
  }
  const bool readOnly = !strcmp(input, "status") || !strncmp(input, "api", 3) ||
      !strcmp(input, "hash") || !strcmp(input, "metadata") || !strcmp(input, "help");
  const bool packageCancel = !strcmp(input, "cancel") && packageFetch.pending &&
      packageFetch.wasm == wasmRuntime_;
  if (!readOnly && !packageCancel && !wasmRuntime_ && wasmSource_ &&
      (wasmSource_->phase_ != Phase::Idle || wasmSource_->livePending_ || wasmSource_->publication_ ||
       (!wasmSource_->liveActive_ && strcmp(input, "remove") && strcmp(input, "retry")))) {
    respond("Error: Wasm source activation busy or pending; inspect source wasm status; use source wasm retry/remove"); return;
  }
  if (sealed_) {
    if (!strcmp(input, "retry")) {
      liveRetries_ = 0; retryAt_ = 0;
      const bool loaded = beginDeployment();
      publishReadiness();
      respond(loaded ? "Accepted source journal retry; inspect source status" : outcome_);
      return;
    }
    if (strcmp(input, "remove")) { respond(outcome_); return; }
    Deployment reset;
    reset.generation = 1;
    Upload empty;
    if (!save(reset, empty)) {
      respond("Error: source recovery commit failed"); return;
    }
    deployed_ = reset; upload_ = empty; sealed_ = false; livePending_ = true;
    strcpy(outcome_, "source durably saved; live activation pending");
    liveActive_ = false;
    publishReadiness();
    respond(wasmRuntime_ ? "Removed corrupt deployment journal; removing Wasm handlers" :
            "Removed corrupt deployment journal; restoring bundled handlers"); return;
  }
  if (!strcmp(input, "status")) {
    snprintf(reply, capacity, "gen=%u active=%u prev=%u size=%u upload=%s next=%u/%u; %s",
             deployed_.generation, deployed_.active, deployed_.previous,
             unsigned(deployed_.active == Bundled ? bundledSize() : deployed_.sizes[deployed_.active]),
             upload_.size ? upload_.id : "none", (upload_.received + 47) / 48,
             (upload_.size + 47) / 48, outcome_);
    return;
  }
  if (!strcmp(input, "hash")) {
    uint8_t hash[32];
    if (deployed_.active == Bundled)
      mesh::Utils::sha256(hash, sizeof(hash), reinterpret_cast<const uint8_t *>(BotDefaultSource), bundledSize());
    else memcpy(hash, deployed_.hashes[deployed_.active], sizeof(hash));
    snprintf(reply, capacity, "SHA256 ");
    for (unsigned i = 0; i < 32; ++i) snprintf(reply + 7 + 2 * i, capacity - 7 - 2 * i, "%02x", hash[i]);
    snprintf(reply + 71, capacity - 71, " gen=%u", deployed_.generation);
    return;
  }
  if (!strcmp(input, "help")) {
    respond("api; api fetch; metadata; fetch package SHA256; begin ID16 SIZE SHA256; chunk ID16 INDEX HEX; commit; status; read INDEX; rollback; remove; retry; cancel");
    return;
  }
  if (!strcmp(input, "api")) {
    if (wasmRuntime_) {
      snprintf(reply, capacity,
               "API meshcore-v1 runtime=wamr-2.4.1 commands=8 arguments=4 source-bytes=4096 jobs=%u memory=65536 stack=8192 fuel=10000",
               BotJobLimit);
      return;
    }
    static constexpr char api[] = "API named-commands-v1 lua=5.5.1 commands=8 arguments=4 source-bytes=4096 runtime=shared-v1 jobs=%u kv=2 sleep=1 mesh=dm,wait,trace,advert rpc=1";
    static_assert(sizeof(api) <= 146, "Source API must fit a nonce-tagged native reply");
    snprintf(reply, capacity, api, BotJobLimit);
    return;
  }
  if (!strcmp(input, "api package")) {
    if (wasmRuntime_) {
#if ONCHIP_BOT_HTTPS
      respond("Package runtime=wamr-2.4.1 api=meshcore-v1 caps=cmdmeta,events,https,kv,kv.atomic,mesh,mesh-chan,mesh-dest,reminders,timers,utilities");
#else
      respond("Package runtime=wamr-2.4.1 api=meshcore-v1 caps=cmdmeta,events,kv,kv.atomic,mesh,mesh-chan,mesh-dest,reminders,timers,utilities");
#endif
      return;
    }
#if ONCHIP_BOT_HTTPS
    static constexpr char packageApi[] =
        "Package api=named-commands-v1 caps=cmdmeta,events,https,kv,kv.atomic,mesh,mesh-chan,mesh-dest,modules,reminders,timers,utilities";
#else
    static constexpr char packageApi[] =
        "Package api=named-commands-v1 caps=cmdmeta,events,kv,kv.atomic,mesh,mesh-chan,mesh-dest,modules,reminders,timers,utilities";
#endif
    static_assert(sizeof(packageApi) <= 146, "Package API must fit a nonce-tagged native reply");
    respond(packageApi);
    return;
  }
  if (!strcmp(input, "api fetch")) {
#if ONCHIP_BOT_HTTPS
    if (wasmRuntime_) {
      respond("Package GET runtime=wamr-2.4.1 alias=package raw=1..4096 type=application/wasm|application/octet-stream SHA256=source"); return;
    }
    respond("Owner GET JSON=2048; Package GET alias=package raw=1..4096 type=text/plain|application/octet-stream|application/x-lua SHA256=source");
#else
    respond("Owner fetch unavailable");
#endif
    return;
  }
  if (!strcmp(input, "metadata")) {
    MastPackageMetadata value;
    char error[96]{};
    if (!metadata(deployed_.active, value, error, sizeof(error))) {
      snprintf(reply, capacity, "Error: %.100s; %s source%s remove", error,
               livePending_ ? "wait for retries, then use" : "use", wasmRuntime_ ? " wasm" : "");
      return;
    }
    if (!value.present) {
      respond(value.bundled ? (wasmRuntime_ ? "EMPTY schema=none" : "BUNDLED schema=none") : "PLAIN schema=unknown"); return;
    }
    snprintf(reply, capacity, "META name=%.24s ver=%.23s schema=%.39s rollback=%.39s",
             value.name, value.version, value.schema, value.rollback);
    return;
  }
  if (!strcmp(input, "api storage")) {
    respond("Storage kv=2 scopes=caller,conversation,bot,channel timer=set,get,cancel,wait autonomous-reminders=1");
    return;
  }
  if (!strcmp(input, "api atomic")) {
    snprintf(reply, capacity,
             "Atomic cas=1 transaction=%u keys=distinct scope=single outcome=committed,conflict,unknown,rejected recovery=redo",
             BotTransactionLimit);
    return;
  }
  if (!strcmp(input, "api data")) {
    respond("Data kv=1 timers=1 reminders=1 scope=single bytes=2422 version=1 owner=required restore=stage,commit scheduler=no-rearm credentials=excluded");
    return;
  }
  if (!strcmp(input, "api modules")) {
    if (wasmRuntime_) { respond("Error: Wasm module linking is unavailable in ABI v1"); return; }
    respond("Modules declare=module require=declared-only count=8 name-bytes=24 envelope=4096 init=bounded source-only=1 shared-vm=1");
    return;
  }
  if (!strcmp(input, "api overrides")) {
    if (wasmRuntime_) { respond("Error: Builtin overrides require Lua"); return; }
    respond("Overrides override_command(name,export) call_original(name[,text]) builtin-schema/policy=retained ctx=current-command slots=8");
    return;
  }
  if (!strcmp(input, "api events")) {
    respond(wasmRuntime_ ?
        "Events startup,connectivity,message,node_status grant=bot-events default=off recurring=unsupported" :
        "Events startup,connectivity,message,node_status; every(seconds,function) recurring=16 min=15s grant=bot-events restart=source skip-missed=1");
    return;
  }
  if (!strcmp(input, "api sources")) {
    if (wasmRuntime_) { respond("Error: Named Lua sources require the Lua runtime"); return; }
    respond("Sources format=meshcore-sources/1 count=8 envelope=4096 namespace=shared collisions=reject commit-base=sha256 atomic=1");
    return;
  }
  if (!strcmp(input, "api bundled")) {
    if (wasmRuntime_) { respond("Error: Bundled commands require Lua"); return; }
    uint8_t hash[32]{};
    mesh::Utils::sha256(hash, sizeof(hash), reinterpret_cast<const uint8_t *>(BotDefaultSource), strlen(BotDefaultSource));
    snprintf(reply, capacity, "SHA256 ");
    for (unsigned i = 0; i < 32; ++i) snprintf(reply + 7 + 2 * i, capacity - 7 - 2 * i, "%02x", hash[i]);
    return;
  }
  if (!strcmp(input, "api repeaters")) {
    if (wasmRuntime_) { respond("Error: Repeater monitoring requires Lua"); return; }
    snprintf(reply, capacity, "Repeaters next,status,login peers=%u owner=required ACL=read passwords=none period-min=60s global-min=30s discovery=60..86400s default=900s", BotRepeaterLimit);
    return;
  }
  if (!strcmp(input, "api mesh")) {
    static constexpr char meshApi[] =
        "Mesh compose=dm,channel,trace dm=caller+owner-fullkey[1-4] "
        "wait=kind:ack/text/channel/trace chan-wait=owner-gated/default-off "
        "fwd=pair-dm multi=3 raw=0";
    static_assert(sizeof(meshApi) <= 163, "Mesh API must fit a native reply");
    respond(meshApi);
    return;
  }
  if (!strcmp(input, "api paths")) {
    respond("Paths ordinary_mode=0/1/2->1/2/3B bot path/role-path args=bytes(1..3) TRACE explicit=1/2/4/8B inferred=1/2B TRACE width3=reject");
    return;
  }
  if (!strcmp(input, "api reminders")) {
    respond("Reminders after,list,cancel private-dm=1 autonomous=1 slots=8 caller=2 text=120 max-seconds=86400 overdue=60 grant=bot-reminders");
    return;
  }
  if (!strcmp(input, "api notes")) {
    respond("Notes remember,recall,forget,notes,list-memories private-dm=1 kv-list=1 keys=8 key-bytes=32 text=120 overwrite=explicit");
    return;
  }
  if (!strcmp(input, "api utility")) {
    respond("Utility calc,convert,roll,choose calls=8 digits=10 arithmetic-depth=8 tokens=64 ops=32 dice=12d1000 choices=8 channel=verified");
    return;
  }
  if (!strcmp(input, "api services")) {
    respond("Services weather,service home=health,echo,weather private-dm=1 grant=bot-home place=required place-bytes=80 echo-bytes=120");
    return;
  }
  if (!strcmp(input, "api board")) {
    respond("Board put,get,list,delete channel=verified grant=bot-shared dm=denied prefix=board: key-bytes=26 text=120 quota=shared-8");
    return;
  }
  if (!strcmp(input, "api diagnostics")) {
    respond("Diagnostics about,version,uptime,status,signal,air readonly=1 channel=verified air-pages=4 battery=unavailable counters=u32");
    return;
  }
  if (!strcmp(input, "cancel") && packageFetch.pending && packageFetch.wasm == wasmRuntime_) {
    if (!packageFetch.cancelling) {
      commandBotService().cancelPackageFetch();
      packageFetch.cancelling = true;
      strcpy(outcome_, "package fetch cancellation pending");
    }
    respond("Package fetch cancellation pending; inspect source status"); return;
  }
  if (!strcmp(input, "retry") && phase_ == Phase::Idle && !packageFetch.pending) {
    auto &bot = commandBotService();
    if (bot.publicKey() && !bot.sourceReady() && !bot.retrySourceInitialization()) {
      respond("Error: command VM initialization busy/unavailable"); return;
    }
    liveRetries_ = 0; retryAt_ = 0; livePending_ = true;
    publishReadiness();
    respond(wasmRuntime_ ? "Accepted live retry; inspect source wasm status and hash" :
                           "Accepted live retry; inspect source status and hash");
    return;
  }
  if (phase_ != Phase::Idle || livePending_ || packageFetch.pending) {
    respond("Error: source worker/apply busy"); return;
  }
  if (!strncmp(input, "help ", 5)) {
    if (strlen(input + 5) > 96) { respond("Error: help limit is 96 bytes"); return; }
    auto next = deployed_;
    if (next.generation == UINT32_MAX) { respond("Error: deployment generation exhausted"); return; }
    ++next.generation;
    strcpy(next.help[next.active], !strcmp(input + 5, "-") ? "" : input + 5);
    if (!save(next, upload_)) {
      respond("Error: help commit failed"); return;
    }
    deployed_ = next; respond("Saved source help"); return;
  }
  if (!strcmp(input, "helptext")) {
    const auto *help = deployed_.help[deployed_.active];
    respond(help[0] ? help : "(no source help)"); return;
  }
  if (!strncmp(input, "fetch ", 6)) {
    char endpoint[BotNameLimit + 1]{}, digest[65]{}, extra;
    uint8_t hash[32];
    if (sscanf(input, "fetch %24s %64s %c", endpoint, digest, &extra) != 2 ||
        !decode(digest, hash, sizeof(hash))) {
      respond("Error: fetch requires package and lowercase SHA256"); return;
    }
    if (strcmp(endpoint, "package")) {
      respond("Error: package fetch uses the fixed package endpoint alias"); return;
    }
    if (upload_.size) {
      respond("Error: cancel the existing source upload before package fetch"); return;
    }
    if (packageFetch.nextId == UINT32_MAX) {
      respond("Error: package fetch request ID exhausted"); return;
    }
    BotHttpsFetchRequest request{};
    request.id = ++packageFetch.nextId;
    memcpy(request.expectedSha256, hash, sizeof(hash));
    request.maxBytes = BotSourceLimit;
    request.sink = &packageSourceSink;
    packageSourceSink.select(uploadPath());
    if (!commandBotService().fetchPackage(request)) {
      respond("Error: native package GET unavailable, denied or busy"); return;
    }
    packageFetch.id = request.id;
    packageFetch.wasm = wasmRuntime_;
    memcpy(packageFetch.hash, hash, sizeof(hash));
    packageFetch.pending = true;
    packageFetch.cancelling = false;
    strcpy(outcome_, "package fetch queued; active source unchanged");
    snprintf(reply, capacity, "Accepted package fetch %.16s; inspect source status",
             digest);
    return;
  }
  char id[17]{}, digest[65]{}, extra;
  unsigned size = 0;
  if (!strncmp(input, "begin ", 6)) {
    uint8_t hash[32], idBytes[8];
    if (sscanf(input, "begin %16s %u %64s %c", id, &size, digest, &extra) != 3 ||
        !size || size > BotSourceLimit || !decode(digest, hash, 32) || !decode(id, idBytes, 8)) {
      respond("Error: begin requires ID16, 1..4096 bytes and SHA256"); return;
    }
    if (upload_.size) {
      if (strcmp(id, upload_.id) || size != upload_.size || memcmp(hash, upload_.hash, 32)) {
        respond("Error: another upload exists; cancel explicitly"); return;
      }
    } else {
      Upload next;
      next.size = size; strcpy(next.id, id); memcpy(next.hash, hash, 32);
      auto file = SPIFFS.open(uploadPath(), "w");
      if (!file) { respond("Error: upload file unavailable"); return; }
      file.flush(); file.close();
      if (!save(deployed_, next)) {
        respond("Error: upload admission commit failed"); return;
      }
      upload_ = next;
    }
    snprintf(reply, capacity, "ACK %s next=%u", id, (upload_.received + 47) / 48);
    return;
  }
  if (!strcmp(input, "cancel")) {
    Upload empty;
    if (!save(deployed_, empty)) {
      respond("Error: upload cancel commit failed"); return;
    }
    upload_ = empty;
    if (SPIFFS.exists(uploadPath()) && !SPIFFS.remove(uploadPath())) {
      respond("Error: cancelled but staging cleanup failed"); return;
    }
    respond("Upload cancelled; active source retained"); return;
  }
  if (!strncmp(input, "chunk ", 6)) {
    unsigned index;
    char encoded[97];
    if (sscanf(input, "chunk %16s %u %96s %c", id, &index, encoded, &extra) != 3 ||
        !upload_.size || strcmp(id, upload_.id) || index > 85) {
      respond("Error: invalid upload/chunk"); return;
    }
    const size_t offset = size_t(index) * 48;
    if (offset > upload_.received || offset >= upload_.size) {
      snprintf(reply, capacity, "Error: expected chunk %u", (upload_.received + 47) / 48); return;
    }
    const size_t n = upload_.size - offset < 48 ? upload_.size - offset : 48;
    uint8_t bytes[48], check[48];
    if (!decode(encoded, bytes, n)) { respond("Error: invalid chunk hex/length"); return; }
    auto file = SPIFFS.open(uploadPath(), offset < upload_.received ? "r" : "r+");
    if (!file || !file.seek(offset)) { respond("Error: staging seek failed"); return; }
    if (offset < upload_.received) {
      if (file.read(check, n) != n || memcmp(check, bytes, n)) {
        respond("Error: conflicting duplicate chunk"); return;
      }
    } else {
      if (file.write(bytes, n) != n) { respond("Error: incomplete chunk write"); return; }
      file.flush(); file.close();
      file = SPIFFS.open(uploadPath(), "r");
      if (!file || !file.seek(offset) || file.read(check, n) != n || memcmp(check, bytes, n)) {
        respond("Error: chunk readback failed"); return;
      }
      auto next = upload_;
      next.received = offset + n;
      if (!save(deployed_, next)) {
        respond("Error: chunk progress commit failed; retry same chunk"); return;
      }
      upload_ = next;
    }
    snprintf(reply, capacity, "ACK %s next=%u", id, (upload_.received + 47) / 48);
    return;
  }
  if (!strncmp(input, "read ", 5)) {
    unsigned index;
    if (sscanf(input, "read %u %c", &index, &extra) != 1 || index > 85) {
      respond("Error: read index must be 0..85"); return;
    }
    const size_t offset = size_t(index) * 48;
    uint8_t bytes[48];
    size_t n;
    if (deployed_.active == Bundled) {
      const size_t size = bundledSize();
      n = offset >= size ? 0 : std::min(size - offset, sizeof(bytes));
      if (n) memcpy(bytes, BotDefaultSource + offset, n);
    } else {
      auto file = SPIFFS.open(slotPath(deployed_.active), "r");
      if (!file || file.size() != deployed_.sizes[deployed_.active]) {
        respond("Error: active source unavailable"); return;
      }
      n = offset >= file.size() ? 0 : std::min(file.size() - offset, sizeof(bytes));
      if (n && (!file.seek(offset) || file.read(bytes, n) != n)) {
        respond("Error: source read failed"); return;
      }
    }
    if (!n) { respond("EOF"); return; }
    snprintf(reply, capacity, "DATA ");
    for (size_t i = 0; i < n; ++i) snprintf(reply + 5 + 2 * i, capacity - 5 - 2 * i, "%02x", bytes[i]);
    return;
  }
  uint8_t candidate = Bundled;
  removing_ = false;
  if (!strncmp(input, "commit ", 7)) {
    char id[17]{}, base[65]{};
    const int fields = sscanf(input + 7, "%16s %64s %c", id, base, &extra);
    if ((fields != 1 && fields != 2) || !upload_.size ||
        strcmp(id, upload_.id) || upload_.received != upload_.size) {
      respond("Error: upload incomplete or wrong ID"); return;
    }
    if (fields == 2) {
      uint8_t expected[32]{}, actual[32]{};
      if (!decode(base, expected, 32)) { respond("Error: source commit base requires SHA256"); return; }
      if (deployed_.active == Bundled)
        mesh::Utils::sha256(actual, sizeof(actual), reinterpret_cast<const uint8_t *>(BotDefaultSource), bundledSize());
      else memcpy(actual, deployed_.hashes[deployed_.active], sizeof(actual));
      if (memcmp(expected, actual, sizeof(actual))) {
        respond("Error: active Lua sources changed; download and reconcile before installing"); return;
      }
    }
    for (candidate = 0; candidate < 3; ++candidate)
      if (candidate != deployed_.active && candidate != deployed_.previous) break;
    if (!startCopy(uploadPath(), BotStagedSourcePath, upload_.size, Phase::CopyCandidate)) {
      respond(outcome_); return;
    }
  } else if (!strcmp(input, "rollback") || !strcmp(input, "remove")) {
    removing_ = !strcmp(input, "remove");
    candidate = !strcmp(input, "rollback") ? deployed_.previous : Bundled;
    if (!removing_) {
      MastPackageMetadata active, target;
      char error[96]{};
      if (!metadata(deployed_.active, active, error, sizeof(error)) ||
          !metadata(candidate, target, error, sizeof(error))) {
        snprintf(reply, capacity, "Error: %.100s; use source%s remove",
                 error, wasmRuntime_ ? " wasm" : "");
        return;
      }
      if (!compatible(active, target, true, error, sizeof(error))) {
        snprintf(reply, capacity, "Error: %.100s", error); return;
      }
    }
    if (candidate == deployed_.active) {
      respond("Error: no different recoverable source"); return;
    }
    if (wasmRuntime_ && candidate == Bundled) phase_ = Phase::AwaitPublication;
    else if (!prepareFile(candidate, Phase::CopyCandidate)) {
      respond(outcome_); return;
    }
  } else { respond("Error: unknown source command"); return; }
  candidate_ = candidate;
  respond("Accepted verification; source status reports durable activation outcome");
}
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
uint32_t MastSource::hostWaitMs() {
  if (sealed_) return UINT32_MAX;
  auto &bot = commandBotService();
  uint32_t wait = UINT32_MAX;
  const uint32_t now = millis();
  if (wasmSource_ && phase_ == Phase::Idle && !livePending_)
    wait = wasmSource_->hostWaitMs();
  if (phase_ == Phase::Commit || phase_ == Phase::AwaitStageLive) return 0;
  if (phase_ == Phase::AwaitPublication && !bot.jobsInUse() && bot.sourceReady()) return 0;
  if ((phase_ == Phase::CopyCandidate || phase_ == Phase::CopyDurable ||
       phase_ == Phase::CopyLive || phase_ == Phase::Validating || phase_ == Phase::Recover) &&
      validator().resultReady()) return 0;
  if ((phase_ == Phase::StageLive || phase_ == Phase::ActivateLive) &&
      bot.hostSourceResultReady()) return 0;
#if ONCHIP_BOT_SINGLE_SESSION
  if (phase_ == Phase::AwaitCopy) {
    if (!validator().busy()) return 0;
    botEarlier(wait, now, copyDeadline_);
  }
#endif
  if (phase_ == Phase::Idle && livePending_ && bot.sourceReady())
    botEarlier(wait, now, retryAt_ ? retryAt_ : now);
  return wait;
}
#endif
void MastSource::loop() {
  serviceLoop();
  publishReadiness();
}
void MastSource::serviceLoop() {
  if (wasmSource_ && phase_ == Phase::Idle && !livePending_) {
    wasmSource_->loop();
    if (wasmSource_->phase_ != Phase::Idle || wasmSource_->livePending_) return;
  }
  auto &bot = commandBotService();
  if (sealed_) { bot.setRuntimeAdmission(wasmRuntime_, false); return; }
  if (packageFetch.pending && packageFetch.wasm == wasmRuntime_) {
    BotHttpsFetchResult result{};
    if (!bot.pollPackageFetch(result)) {
      const bool cleaned = abortPackageSource("package fetch result unavailable");
      clearPackageFetch();
      fail(cleaned ? "native package fetch result unavailable; active source retained" :
                     "native package fetch result unavailable and staging cleanup failed");
      return;
    }
    if (result.id != packageFetch.id) {
      const bool cleaned = abortPackageSource("unexpected package fetch ID");
      clearPackageFetch();
      fail(cleaned ? "package fetch completion ID mismatch; active source retained" :
                     "package fetch ID mismatch and staging cleanup failed");
      return;
    }
    if (result.state == BotHttpsFetchResult::Queued) {
      strcpy(outcome_, "package fetch queued; active source unchanged");
      return;
    }
    if (result.state == BotHttpsFetchResult::Running) {
      strcpy(outcome_, packageFetch.cancelling ?
          "package fetch cancellation pending" : "package fetch running");
      return;
    }
    const bool cancelled = packageFetch.cancelling &&
        strnlen(result.rpcCode, sizeof(result.rpcCode)) == sizeof("cancelled") - 1 &&
        !memcmp(result.rpcCode, "cancelled", sizeof("cancelled") - 1);
    if (result.state == BotHttpsFetchResult::Unknown) {
      const bool cleaned = abortPackageSource("package fetch failed");
      clearPackageFetch();
      if (cleaned && cancelled)
        strcpy(outcome_,
               "package fetch cancelled; active source retained; remote GET outcome unknown");
      else
        fail(cleaned ?
            "package GET outcome unknown after submission; inspect status before retry" :
            "package GET outcome unknown; staging cleanup failed; inspect status");
      return;
    }
    if (result.state == BotHttpsFetchResult::Failed) {
      char fetchError[sizeof(result.error) + 1]{};
      if (result.error[0])
        snprintf(fetchError, sizeof(fetchError), "%.*s",
                 int(sizeof(result.error)), result.error);
      const bool cleaned = abortPackageSource("package fetch failed");
      clearPackageFetch();
      if (!cleaned)
        fail("package staging cleanup failed; active source retained");
      else if (cancelled)
        strcpy(outcome_, "package fetch cancelled; active source retained");
      else
        fail(fetchError[0] ? fetchError : "native package GET failed");
      return;
    }
    if (packageFetch.cancelling) {
      const bool cleaned = abortPackageSource("package fetch cancelled");
      clearPackageFetch();
      if (!cleaned)
        fail("package fetch cancelled but staging cleanup failed; active source retained");
      else
        strcpy(outcome_, "package fetch cancelled; active source retained");
      return;
    }
    if (result.state != BotHttpsFetchResult::Complete) {
      const bool cleaned = abortPackageSource("invalid package fetch state");
      clearPackageFetch();
      fail(cleaned ? "invalid native package fetch completion state" :
                     "invalid package fetch state and staging cleanup failed");
      return;
    }
    packageFetch.pending = false;
    const bool accepted = acceptPackageFetch(result);
    clearPackageFetch();
    if (!accepted) return;
  }
#if ONCHIP_BOT_SINGLE_SESSION
  if (phase_ == Phase::Recover) {
    BotWorker::Result result;
    if (!pollValidation(result)) return;
    phase_ = Phase::Idle;
    if (publication_ && !bot.sourcePublicationCurrent(publication_)) {
      bot.releaseSourcePublication(publication_);
      publication_ = 0;
    }
    bot.setRuntimeAdmission(wasmRuntime_, result.ok);
    if (!result.ok) {
      livePending_ = false;
      liveActive_ = false;
      startupPending_ = true;
      snprintf(outcome_, sizeof(outcome_), "Error: prior Lua reload failed: %.125s; source retry or reboot", result.error);
    } else {
      char previous[128];
      snprintf(previous, sizeof(previous), "%s", outcome_);
      snprintf(outcome_, sizeof(outcome_), "%s; prior Lua source reloaded; jobs/globals restarted", previous);
    }
    return;
  }
  if (phase_ == Phase::AwaitCopy) {
    if (!validator().copyFile(copyFrom_[0] ? copyFrom_ : nullptr, copyTo_, copySize_, publication_)) {
      if (int32_t(millis() - copyDeadline_) >= 0) {
        if (copyPhase_ == Phase::CopyLive)
          liveFailed("Lua live source copy busy for 5s; prior source retained");
        else fail("Lua source copy busy for 5s; prior source retained");
      }
      return;
    }
    phase_ = copyPhase_;
  }
#endif
  if (phase_ == Phase::CopyCandidate || phase_ == Phase::CopyDurable || phase_ == Phase::CopyLive) {
    BotWorker::Result result;
    if (!pollValidation(result)) return;
    stopValidation();
    Serial.printf("Mast source copy/readback: %u ms, ok=%u\n", result.sourceReadMs, result.ok);
    const auto copied = phase_;
    phase_ = Phase::Idle;
    if (!result.ok) {
      if (copied == Phase::CopyLive) liveFailed(result.error);
      else fail(result.error);
      return;
    }
    if (copied == Phase::CopyCandidate) {
      if (!removing_) {
        MastPackageMetadata active, candidate;
        char error[96]{};
        const size_t candidateSize = candidate_ == Bundled ? strlen(BotDefaultSource) :
            candidate_ == deployed_.previous ? deployed_.sizes[candidate_] : upload_.size;
        if (!metadata(deployed_.active, active, error, sizeof(error)) ||
            !inspectMetadataFile(BotStagedSourcePath, candidateSize, candidate, error, sizeof(error))) {
          fail(error[0] ? error : "package metadata compatibility failed");
          return;
        }
        if (candidate_ == Bundled) {
          candidate.bundled = true;
          if (!candidate.present) {
            strcpy(candidate.schema, "none");
            strcpy(candidate.rollback, "none");
          }
        }
        if (!compatible(active, candidate, candidate_ == deployed_.previous,
                        error, sizeof(error))) {
          fail(error[0] ? error : "package metadata compatibility failed");
          return;
        }
      }
      startValidation(candidate_);
      return;
    }
    phase_ = copied == Phase::CopyDurable ? Phase::Commit : Phase::AwaitStageLive;
  }
  if (phase_ == Phase::Validating) {
    BotWorker::Result result;
    if (!pollValidation(result)) return;
    Serial.printf("Mast source validation: total=%llu load=%llu init=%llu cleanup=%llu us; peak=%u stack=%u\n",
                  static_cast<unsigned long long>(result.stats.elapsedUs),
                  static_cast<unsigned long long>(result.stats.loadUs),
                  static_cast<unsigned long long>(result.stats.initUs),
                  static_cast<unsigned long long>(result.stats.cleanupUs),
                  unsigned(result.stats.peakBytes), result.stats.stackHighWaterBytes);
    if (!result.ok) { fail(result.error); return; }
    phase_ = Phase::AwaitPublication;
  }
  if (phase_ == Phase::AwaitPublication) {
    if (publication_ && !bot.sourcePublicationCurrent(publication_)) {
      bot.releaseSourcePublication(publication_);
      publication_ = 0;
    }
    offlinePublication_ = offlineLuaPublicationCurrent();
    if (!offlinePublication_) {
      char error[128]{};
      const uint32_t token = bot.reserveSourcePublication(
          wasmRuntime_ && candidate_ == Bundled ? nullptr : &validator(),
          wasmRuntime_, error, sizeof(error), publication_);
      if (!token) {
        if (error[0]) fail(error);
        return;
      }
      publication_ = token;
    }
    stopValidation();
    if (candidate_ < Bundled && candidate_ != deployed_.previous) {
      startCopy(BotStagedSourcePath, slotPath(candidate_), upload_.size, Phase::CopyDurable);
      return;
    }
    phase_ = Phase::Commit;
  }
  if (phase_ == Phase::Commit) {
    phase_ = Phase::Idle;
    if (offlinePublication_ ? !offlineLuaPublicationCurrent() : !bot.sourcePublicationCurrent(publication_)) {
      bot.releaseSourcePublication(publication_);
      publication_ = 0;
      fail("Source publication generation changed; journal and prior source retained");
      return;
    }
    auto next = deployed_;
    if (candidate_ < Bundled && candidate_ != deployed_.previous) {
      next.sizes[candidate_] = upload_.size;
      memcpy(next.hashes[candidate_], upload_.hash, 32);
      next.help[candidate_][0] = 0;
    }
    if (removing_) next.help[Bundled][0] = 0;
    if (next.generation == UINT32_MAX) { fail("deployment generation exhausted"); return; }
    ++next.generation; next.previous = next.active; next.active = candidate_;
    Upload empty;
    if (!save(next, empty)) {
      fail("activation journal commit failed; old source retained"); return;
    }
    deployed_ = next;
    publicationDurable_ = true;
    upload_ = empty;
    livePending_ = true;
    liveActive_ = false;
    liveRetries_ = 0; retryAt_ = 0;
    strcpy(outcome_, "source durably saved; live activation pending");
  }
  if (phase_ == Phase::Idle && livePending_) {
    if (retryAt_ && int32_t(millis() - retryAt_) < 0) return;
    if (!bot.publicKey()) {
      livePending_ = false;
      strcpy(outcome_, "source saved; bot disabled; loads on next enabled boot");
      return;
    }
    bot.setRuntimeAdmission(wasmRuntime_, false);
    if (!bot.sourceReady()) return;
    if (startupPending_) {
      MastPackageMetadata active;
      char error[96]{};
      if (!metadata(deployed_.active, active, error, sizeof(error))) {
        liveFailed(error);
        return;
      }
    }
    if (wasmRuntime_ && deployed_.active == Bundled) {
      if (!bot.removeWasm(publication_)) {
        liveFailed("Wasm live removal refused; saved source retained"); return;
      }
      livePending_ = false; phase_ = Phase::ActivateLive; return;
    }
    livePending_ = false;
    if (!prepareFile(deployed_.active, Phase::CopyLive)) liveFailed("live copy unavailable");
    return;
  }
  if (phase_ == Phase::AwaitStageLive) {
    uint8_t hash[32];
    const size_t size = deployed_.active == Bundled ? strlen(BotDefaultSource) : deployed_.sizes[deployed_.active];
    if (deployed_.active == Bundled)
      mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(BotDefaultSource), size);
    else memcpy(hash, deployed_.hashes[deployed_.active], 32);
    if (!bot.stageSourceFile(size, hash, publication_)) {
      liveFailed(wasmRuntime_ ? "Wasm live staging refused; saved source retained" :
                               "Lua live staging refused; saved source retained");
      return;
    }
    phase_ = Phase::StageLive;
  }
  if (phase_ == Phase::StageLive || phase_ == Phase::ActivateLive) {
    BotWorker::Result result;
    if (!bot.pollSourceResult(result)) return;
    if (!result.ok) {
      liveFailed(result.error); return;
    }
    if (phase_ == Phase::StageLive) {
      if (!bot.activateStaged(publication_)) { liveFailed("live activation busy"); return; }
      phase_ = Phase::ActivateLive;
    } else {
      phase_ = Phase::Idle; strcpy(outcome_, "source durably saved and active");
      liveRetries_ = 0; retryAt_ = 0;
      bot.releaseSourcePublication(publication_);
      publication_ = 0; publicationDurable_ = false; offlinePublication_ = false;
      liveActive_ = true;
      startupPending_ = false;
      bot.setRuntimeAdmission(wasmRuntime_, true);
    }
  }
}
} // namespace onchip
#endif
