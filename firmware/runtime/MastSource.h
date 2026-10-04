// SPDX-License-Identifier: Apache-2.0
#pragma once
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "BotWorker.h"

namespace onchip {
struct MastPackageMetadata {
  bool present = false, bundled = false;
  char name[25]{}, version[24]{}, runtime[16]{}, api[32]{}, capabilities[97]{};
  char schema[40]{}, rollback[40]{};
};
class MastSource {
public:
  MastSource() = default;
  ~MastSource() { stop(); }
  bool begin();
  void execute(const char *command, char *reply, size_t capacity);
  void loop();
  void stop();
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  bool ready() const;
  bool recoveryRequired() const;
  uint32_t hostWaitMs();
#endif

private:
  explicit MastSource(bool wasm) : wasmRuntime_(wasm) {}
  bool wasmRuntime_ = false;
  bool wasmJournalBlocked_ = false;
  MastSource *wasmSource_ = nullptr;
  const char *slotPath(uint8_t slot) const;
  const char *uploadPath() const;
  const char *journalKey() const;
  size_t bundledSize() const;
  static constexpr uint8_t Bundled = 3;
  struct Deployment {
    uint32_t version = 1, generation = 0;
    uint8_t active = Bundled, previous = Bundled, reserved[2]{};
    uint32_t sizes[3]{};
    uint8_t hashes[3][32]{};
    char help[4][97]{};
  } deployed_;
  struct Upload {
    uint32_t version = 1, size = 0, received = 0;
    char id[17]{};
    uint8_t hash[32]{};
  } upload_;
  struct Journal { Deployment deployment; Upload upload; };
  static const char *journalError(const Journal &journal);
#if ONCHIP_BOT_SINGLE_SESSION
  BotWorker &validator();
  bool pollValidation(BotWorker::Result &result);
  void stopValidation() {}
  char copyFrom_[32]{}, copyTo_[32]{};
  size_t copySize_ = 0;
  uint32_t copyDeadline_ = 0;
#else
  BotWorker validator_;
  BotWorker &validator() { return validator_; }
  bool pollValidation(BotWorker::Result &result) { return validator_.poll(result); }
  void stopValidation() { validator_.stop(); }
#endif
  enum class Phase {
    Idle, CopyCandidate, Validating, AwaitPublication, CopyDurable, Commit,
    CopyLive, AwaitStageLive, StageLive, ActivateLive, AwaitCopy, Recover
  } phase_ = Phase::Idle;
  Phase copyPhase_ = Phase::Idle;
  uint8_t candidate_ = Bundled;
  bool livePending_ = false;
  bool sealed_ = false;
  bool removing_ = false;
  bool liveActive_ = false;
  bool startupPending_ = true;
  uint8_t liveRetries_ = 0;
  uint32_t retryAt_ = 0;
  uint32_t publication_ = 0;
  bool publicationDurable_ = false;
  bool offlinePublication_ = false;
  char outcome_[192] = "bundled source";
  bool prepareFile(uint8_t slot, Phase phase);
  bool startCopy(const char *from, const char *to, size_t size, Phase phase);
  bool startValidation(uint8_t candidate);
  bool metadata(uint8_t slot, MastPackageMetadata &value, char *error, size_t capacity) const;
  bool compatible(const MastPackageMetadata &active, const MastPackageMetadata &candidate,
                  bool rollback, char *error, size_t capacity) const;
  bool acceptPackageFetch(const BotHttpsFetchResult &result);
  bool save(const Deployment &deployment, const Upload &upload);
  bool offlineLuaPublicationCurrent() const;
  void fail(const char *message);
  void liveFailed(const char *message);
  bool beginDeployment();
  void serviceLoop();
  void publishReadiness();
};
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present);
} // namespace onchip
#endif
