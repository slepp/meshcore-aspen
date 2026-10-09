// SPDX-License-Identifier: Apache-2.0
#include "PacketPipeline.h"
#include <cassert>
#include <vector>
using namespace packet_engine;
struct TestHost final : Host {
  uint32_t now = 0;
  bool accept = true;
  std::vector<Fault> errors;
  std::vector<Emission> sent;
  uint32_t microsNow() override { return now; }
  void fault(const char *name, const Metadata &, Fault fault) override {
    assert(name && name[0]);
    errors.push_back(fault);
  }
  bool admit(const Metadata &, const Emission *emissions, uint8_t count) override {
    if (!accept) return false;
    sent.insert(sent.end(), emissions, emissions + count);
    return true;
  }
};
struct TestEngine final : Engine {
  TestHost &host;
  Pipeline *recursive = nullptr;
  Decision decision = Decision::Continue;
  bool emit = false, badBounds = false, exhaust = false, timeout = false;
  uint8_t value = 0x71, emissions = 1;
  explicit TestEngine(TestHost &h) : host(h) {}
  Decision process(const Metadata &metadata, Call &call) override {
    assert(metadata.stage == Stage::Receive);
    const uint8_t bytes[] = {value, 0x42, 0x43};
    assert(call.replace(bytes, sizeof(bytes)));
    if (emit)
      for (unsigned i = 0; i < emissions; ++i) call.emit(bytes, sizeof(bytes), 1, 12, 23);
    if (badBounds) call.write(UINT16_MAX, bytes, 1);
    if (exhaust) while (call.consume()) {}
    if (timeout) host.now += 100;
    if (recursive) {
      uint8_t raw[Capacity] = {1, 2};
      uint16_t length = 2;
      assert(recursive->process(metadata, raw, length, sizeof(raw)) == Decision::Continue);
      assert(length == 2 && raw[0] == 1 && raw[1] == 2);
    }
    return decision;
  }
};
static Budget budget(const char *name) { return {name, stageMask(Stage::Receive), 100, 100}; }
static Decision run(Pipeline &pipeline, uint8_t (&bytes)[Capacity], uint16_t &length,
                    Stage stage = Stage::Receive) {
  Metadata metadata;
  metadata.stage = stage;
  return pipeline.process(metadata, bytes, length, sizeof(bytes));
}
static void registration_and_stages() {
  TestHost host;
  Pipeline pipeline(host);
  TestEngine first(host), second(host), third(host);
  for (auto invalid : {Budget{}, Budget{"bad name", 1, 100, 100},
       Budget{"engine", 0, 100, 100}, Budget{"engine", 256, 100, 100},
       Budget{"engine", 1, 100001, 100}, Budget{"engine", 1, 100, 20001}})
    assert(pipeline.attach(first, invalid) == Registration::Invalid);
  assert(!pipeline.size());
  assert(pipeline.attach(first, budget("first")) == Registration::Attached);
  assert(pipeline.attach(first, budget("other")) == Registration::Duplicate);
  assert(pipeline.attach(second, budget("first")) == Registration::Duplicate);
  assert(pipeline.attach(second, budget("second")) == Registration::Attached);
  assert(pipeline.attach(third, budget("third")) == Registration::Full);
  uint8_t bytes[Capacity] = {1, 2};
  uint16_t length = 2;
  assert(run(pipeline, bytes, length, Stage::Transmit) == Decision::Continue);
  assert(bytes[0] == 1 && length == 2 && !pipeline.calls(0));
  second.value = 0x72;
  assert(run(pipeline, bytes, length) == Decision::Continue);
  assert(bytes[0] == 0x72 && length == 3 && pipeline.calls(0) == 1 &&
         pipeline.calls(1) == 1);
  assert(pipeline.enable(1, false));
  assert(run(pipeline, bytes, length) == Decision::Continue && bytes[0] == 0x71);
  assert(!pipeline.enable(2, true) && !pipeline.enabled(2));
}
static void transaction_and_faults() {
  for (Fault expected : {Fault::Execution, Fault::Fuel, Fault::Deadline, Fault::Bounds,
                         Fault::EmissionLimit, Fault::InvalidDecision, Fault::Reentrant}) {
    TestHost host;
    Pipeline pipeline(host);
    TestEngine first(host), bad(host);
    first.emit = true;
    if (expected == Fault::Execution) bad.decision = Decision::Failed;
    if (expected == Fault::Fuel) bad.exhaust = true;
    if (expected == Fault::Deadline) { bad.timeout = true; host.now = UINT32_MAX - 50; }
    if (expected == Fault::Bounds) bad.badBounds = true;
    if (expected == Fault::EmissionLimit) { bad.emit = true; bad.emissions = 2; }
    if (expected == Fault::InvalidDecision) bad.decision = Decision(99);
    if (expected == Fault::Reentrant) bad.recursive = &pipeline;
    assert(pipeline.attach(first, budget("first")) == Registration::Attached);
    assert(pipeline.attach(bad, budget("bad")) == Registration::Attached);
    uint8_t bytes[Capacity] = {1, 2};
    uint16_t length = 2;
    assert(run(pipeline, bytes, length) == Decision::Continue);
    assert(bytes[0] == 1 && bytes[1] == 2 && length == 2 && host.sent.empty());
    assert(host.errors.back() == expected && pipeline.enabled(0) &&
           !pipeline.enabled(1) && pipeline.faults(1) == 1);
    assert(run(pipeline, bytes, length) == Decision::Continue);
    assert(bytes[0] == 0x71 && length == 3 && host.sent.size() == 1 &&
           pipeline.calls(1) == 1);
    assert(pipeline.enable(1, true));
  }
}
static void emissions_and_drop() {
  TestHost host;
  Pipeline pipeline(host);
  TestEngine first(host), second(host);
  first.emit = true;
  second.emit = true;
  second.value = 0x72;
  assert(pipeline.attach(first, budget("first")) == Registration::Attached);
  assert(pipeline.attach(second, budget("second")) == Registration::Attached);
  uint8_t bytes[Capacity] = {1, 2};
  uint16_t length = 2;
  assert(run(pipeline, bytes, length) == Decision::Continue);
  assert(host.sent.size() == 2 && host.sent[0].bytes[0] == 0x71 &&
         host.sent[1].bytes[0] == 0x72 && host.sent[1].length == 3 &&
         host.sent[1].priority == 1 && host.sent[1].delayMs == 12 &&
         host.sent[1].expiryMs == 23 && bytes[0] == 0x72 && length == 3);
  first.decision = Decision::Drop;
  const auto secondCalls = pipeline.calls(1);
  assert(run(pipeline, bytes, length) == Decision::Drop);
  assert(pipeline.calls(1) == secondCalls && pipeline.drops(0) == 1 &&
         host.sent.size() == 3);
  first.decision = Decision::Continue;
  host.accept = false;
  bytes[0] = 1;
  length = 2;
  assert(run(pipeline, bytes, length) == Decision::Continue);
  assert(bytes[0] == 1 && length == 2 && host.sent.size() == 3 &&
         host.errors.back() == Fault::EmissionRejected && pipeline.enabled(0) &&
         pipeline.enabled(1));
  for (unsigned origin = 0; origin < 3; ++origin) {
    Metadata metadata;
    metadata.local = !origin;
    metadata.engineOrigin = origin == 1;
    metadata.reflectionOrigin = origin == 2;
    assert(pipeline.process(metadata, bytes, length, sizeof(bytes)) == Decision::Continue);
    assert(host.errors.back() == Fault::EmissionOrigin && bytes[0] == 1 &&
           length == 2 && !pipeline.enabled(0));
    assert(pipeline.enable(0, true));
  }
}
static void protocol_validation_is_transactional() {
  for (bool emission : {false, true}) {
    TestHost host;
    Pipeline pipeline(host);
    TestEngine first(host), invalid(host);
    first.emit = true;
    invalid.value = 0x72;
    assert(pipeline.attach(first, budget("first")) == Registration::Attached);
    assert(pipeline.attach(invalid, budget("invalid")) == Registration::Attached);
    uint8_t bytes[Capacity] = {1, 2};
    uint16_t length = 2;
    Metadata metadata;
    const auto validateEdit = [](const Metadata &, const uint8_t *data, uint16_t, bool) {
      return data[0] == 0x72 ? Fault::InvalidPacket : Fault::None;
    };
    const auto validateEmission = [](const Metadata &, const uint8_t *, uint16_t, bool effect) {
      return effect ? Fault::InvalidPacket : Fault::None;
    };
    assert(pipeline.process(metadata, bytes, length, sizeof(bytes),
                            emission ? validateEmission : validateEdit) == Decision::Continue);
    assert(bytes[0] == 1 && length == 2 && host.sent.empty() &&
           host.errors.back() == Fault::InvalidPacket);
    assert(!pipeline.enabled(emission ? 0 : 1));
  }
}
int main() {
  registration_and_stages();
  transaction_and_faults();
  emissions_and_drop();
  protocol_validation_is_transactional();
}
