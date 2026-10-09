// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <string.h>

namespace packet_engine {
constexpr uint16_t Capacity = 255;
constexpr uint8_t EngineLimit = 2;
constexpr uint8_t EmissionLimit = 2;
constexpr uint32_t MaxDelayMs = 0x3fffffff;
enum class Stage : uint8_t {
  Receive, LocalDelivery, Admission, Transmit, Reflection, Relay,
  PlainReceive, PlainCompose
};
constexpr uint32_t stageMask(Stage stage) { return 1u << unsigned(stage); }
enum class Decision : uint8_t { Continue, Drop, Failed };
enum class Fault : uint8_t {
  None, Execution, Fuel, Deadline, Bounds, EmissionLimit, EmissionRejected,
  InvalidDecision, Reentrant, EmissionOrigin
};
inline const char *faultText(Fault fault) {
  switch (fault) {
    case Fault::None: return "no packet engine fault";
    case Fault::Execution: return "packet engine execution failed";
    case Fault::Fuel: return "packet engine instruction budget exhausted";
    case Fault::Deadline: return "packet engine time budget exceeded";
    case Fault::Bounds: return "packet engine packet bounds invalid";
    case Fault::EmissionLimit: return "packet engine emission limit exceeded";
    case Fault::EmissionRejected: return "packet engine scheduler admission failed";
    case Fault::InvalidDecision: return "packet engine returned an invalid decision";
    case Fault::Reentrant: return "packet engine recursively entered the pipeline";
    case Fault::EmissionOrigin: return "packet engine cannot emit from generated packets or local reflections";
  }
  return "unknown packet engine fault";
}
struct Metadata {
  Stage stage = Stage::Receive;
  uint8_t source = UINT8_MAX, destination = UINT8_MAX, payloadType = UINT8_MAX;
  bool local = false, authenticated = false, engineOrigin = false;
  uint32_t generation = 0, job = 0;
  int16_t rssi = 0, snrQuarterDb = 0;
};
struct Emission {
  uint8_t bytes[Capacity]{};
  uint16_t length = 0;
  uint8_t priority = 4;
  uint32_t delayMs = 0, expiryMs = 0;
};
struct Budget {
  const char *name = nullptr;
  uint32_t stages = 0, fuel = 0, microseconds = 0;
};
enum class Registration : uint8_t { Attached, Invalid, Duplicate, Full, Busy };
inline const char *registrationText(Registration result) {
  switch (result) {
    case Registration::Attached: return "registered";
    case Registration::Invalid: return "invalid packet engine name, stage mask or execution budget";
    case Registration::Duplicate: return "packet engine object or name is already registered";
    case Registration::Full: return "packet engine registry is full";
    case Registration::Busy: return "packet engine pipeline is running";
  }
  return "unknown packet engine registration result";
}

class Host {
public:
  virtual ~Host() = default;
  virtual uint32_t microsNow() = 0;
  virtual void fault(const char *engine, const Metadata &, Fault) = 0;
  // Dispatch-thread only. Accept the entire batch or nothing. Generated packets
  // retain an engine-origin tag and use the normal scheduler/airtime budgets.
  virtual bool admit(const Metadata &, const Emission *, uint8_t count) = 0;
};

class Call {
  friend class Pipeline;
  Host &host_;
  uint8_t *bytes_;
  uint16_t length_, capacity_;
  Emission *emissions_;
  uint8_t &emissionCount_;
  uint32_t fuel_, started_, limit_;
  bool allowEmit_;
  Fault fault_ = Fault::None;
  Call(Host &host, uint8_t *bytes, uint16_t length, uint16_t capacity,
       Emission *emissions, uint8_t &count, const Budget &budget, bool allowEmit)
      : host_(host), bytes_(bytes), length_(length), capacity_(capacity),
        emissions_(emissions), emissionCount_(count), fuel_(budget.fuel),
        started_(host.microsNow()), limit_(budget.microseconds), allowEmit_(allowEmit) {}
public:
  void fail(Fault fault) {
    if (fault_ == Fault::None && fault != Fault::None) fault_ = fault;
  }
  bool consume(uint32_t amount = 1) {
    if (fault_ != Fault::None) return false;
    if (amount > fuel_) { fail(Fault::Fuel); return false; }
    fuel_ -= amount;
    if (uint32_t(host_.microsNow() - started_) >= limit_) {
      fail(Fault::Deadline); return false;
    }
    return true;
  }
  uint16_t size() const { return length_; }
  uint16_t capacity() const { return capacity_; }
  bool read(uint16_t offset, uint8_t *output, uint16_t length) {
    if (!consume()) return false;
    if ((!output && length) || offset > length_ || length > length_ - offset) {
      fail(Fault::Bounds); return false;
    }
    if (length) memcpy(output, bytes_ + offset, length);
    return true;
  }
  bool write(uint16_t offset, const uint8_t *input, uint16_t length) {
    if (!consume()) return false;
    if ((!input && length) || offset > length_ || length > length_ - offset) {
      fail(Fault::Bounds); return false;
    }
    if (length) memmove(bytes_ + offset, input, length);
    return true;
  }
  bool replace(const uint8_t *input, uint16_t length) {
    if (!consume()) return false;
    if (!input || !length || length > capacity_) {
      fail(Fault::Bounds); return false;
    }
    memmove(bytes_, input, length);
    if (length < length_) memset(bytes_ + length, 0, length_ - length);
    length_ = length;
    return true;
  }
  bool emit(const uint8_t *input, uint16_t length, uint8_t priority = 4,
            uint32_t delayMs = 0, uint32_t expiryMs = 0) {
    if (!consume()) return false;
    if (!allowEmit_) { fail(Fault::EmissionOrigin); return false; }
    if (!input || !length || length > Capacity ||
        delayMs > MaxDelayMs || expiryMs > MaxDelayMs) {
      fail(Fault::Bounds); return false;
    }
    if (emissionCount_ == EmissionLimit) {
      fail(Fault::EmissionLimit); return false;
    }
    auto &emission = emissions_[emissionCount_++];
    memcpy(emission.bytes, input, length);
    emission.length = length;
    emission.priority = priority;
    emission.delayMs = delayMs;
    emission.expiryMs = expiryMs;
    return true;
  }
};

class Engine {
public:
  virtual ~Engine() = default;
  virtual Decision process(const Metadata &, Call &) = 0;
};

class Pipeline {
  struct Entry {
    Engine *engine = nullptr;
    Budget budget;
    bool enabled = true;
    uint32_t calls = 0, faults = 0, drops = 0;
  };
  Host &host_;
  Entry entries_[EngineLimit];
  uint8_t count_ = 0;
  bool running_ = false, recursion_ = false;
  uint8_t candidate_[Capacity]{};
  Emission emissions_[EmissionLimit];
public:
  explicit Pipeline(Host &host) : host_(host) {}
  Pipeline(const Pipeline &) = delete;
  Pipeline &operator=(const Pipeline &) = delete;
  // All calls and registration run on the radio dispatch task. Engine objects
  // and names must outlive the pipeline.
  Registration attach(Engine &engine, const Budget &budget) {
    if (running_) return Registration::Busy;
    const auto validStages = (stageMask(Stage::PlainCompose) << 1) - 1;
    if (!budget.name || !budget.name[0] || strnlen(budget.name, 32) == 32 ||
        !budget.stages || (budget.stages & ~validStages) ||
        !budget.fuel || budget.fuel > 100000 ||
        !budget.microseconds || budget.microseconds > 20000)
      return Registration::Invalid;
    for (const char *c = budget.name; *c; ++c)
      if (*c < 33 || *c > 126) return Registration::Invalid;
    for (uint8_t i = 0; i < count_; ++i)
      if (entries_[i].engine == &engine || !strcmp(entries_[i].budget.name, budget.name))
        return Registration::Duplicate;
    if (count_ == EngineLimit) return Registration::Full;
    auto &entry = entries_[count_++];
    entry.engine = &engine;
    entry.budget = budget;
    return Registration::Attached;
  }
  uint8_t size() const { return count_; }
  bool enabled(uint8_t slot) const { return slot < count_ && entries_[slot].enabled; }
  uint32_t faults(uint8_t slot) const { return slot < count_ ? entries_[slot].faults : 0; }
  uint32_t calls(uint8_t slot) const { return slot < count_ ? entries_[slot].calls : 0; }
  uint32_t drops(uint8_t slot) const { return slot < count_ ? entries_[slot].drops : 0; }
  bool enable(uint8_t slot, bool enabled) {
    if (running_ || slot >= count_) return false;
    entries_[slot].enabled = enabled;
    return true;
  }
  Decision process(const Metadata &metadata, uint8_t *bytes, uint16_t &length,
                   uint16_t capacity) {
    if (running_) {
      recursion_ = true;
      host_.fault("pipeline", metadata, Fault::Reentrant);
      return Decision::Continue;
    }
    if (!bytes || !length || capacity > Capacity || length > capacity ||
        unsigned(metadata.stage) > unsigned(Stage::PlainCompose)) {
      host_.fault("pipeline", metadata, Fault::Bounds);
      return Decision::Continue;
    }
    running_ = true;
    recursion_ = false;
    memcpy(candidate_, bytes, length);
    uint16_t candidateLength = length;
    uint8_t emissionCount = 0;
    Decision result = Decision::Continue;
    for (uint8_t i = 0; i < count_; ++i) {
      auto &entry = entries_[i];
      if (!entry.enabled || !(entry.budget.stages & stageMask(metadata.stage))) continue;
      ++entry.calls;
      Call call(host_, candidate_, candidateLength, capacity, emissions_, emissionCount,
                entry.budget, !metadata.engineOrigin && !metadata.local);
      result = entry.engine->process(metadata, call);
      call.consume(0);
      if (recursion_) call.fail(Fault::Reentrant);
      if (result == Decision::Failed) call.fail(Fault::Execution);
      if (result != Decision::Continue && result != Decision::Drop && result != Decision::Failed)
        call.fail(Fault::InvalidDecision);
      if (call.fault_ != Fault::None) {
        entry.enabled = false;
        ++entry.faults;
        host_.fault(entry.budget.name, metadata, call.fault_);
        running_ = false;
        return Decision::Continue;
      }
      candidateLength = call.length_;
      if (result == Decision::Drop) { ++entry.drops; break; }
    }
    if (emissionCount && !host_.admit(metadata, emissions_, emissionCount)) {
      host_.fault("pipeline", metadata, Fault::EmissionRejected);
      running_ = false;
      return Decision::Continue;
    }
    // No effects are published until all engines and scheduler admission pass.
    if (result == Decision::Continue) {
      memcpy(bytes, candidate_, candidateLength);
      length = candidateLength;
    }
    running_ = false;
    return result;
  }
};
} // namespace packet_engine
