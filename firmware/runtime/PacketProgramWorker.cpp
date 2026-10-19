// SPDX-License-Identifier: Apache-2.0
#if !defined(NRF52_PLATFORM)
#include "PacketProgramWorker.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#else
#include <chrono>
#include <thread>
#endif

namespace onchip {
namespace {
void pause() {
#ifdef ARDUINO_ARCH_ESP32
  vTaskDelay(1);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}
uint64_t clockUs() {
#ifdef ARDUINO_ARCH_ESP32
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
}
struct PacketProgramWorker::Impl {
  enum State { Idle, Pending, Running, Ready, Held, Reclaim };
  std::atomic<State> state{Idle};
  // The loader task runs for one load and its reclaim, then exits so its stack
  // is not held while packet programs are idle. `live` is cleared before the
  // final Idle publication, so submit() can start the next task.
  std::atomic<bool> stopping{false}, live{false};
  PacketProgramStore &store;
  Factory factory;
  Request request;
  Result result;
  PacketProgram *retired = nullptr;
#ifdef ARDUINO_ARCH_ESP32
  TaskHandle_t task = nullptr;
#else
  std::thread task;
#endif
  Impl(PacketProgramStore &s, Factory f) : store(s), factory(f) {}
  ~Impl() { delete result.program; delete retired; }
  void load() {
    result = {};
    const auto fail = [&](const char *text) {
      snprintf(result.error, sizeof(result.error), "%s", text);
    };
    const auto &r = request;
    const uint64_t started = clockUs();
    constexpr uint64_t InstallDeadlineUs = 2000000;
    const size_t size = r.source.size;
    if (r.slot >= packet_engine::EngineLimit || r.from > PacketProgramUpload ||
        r.to >= PacketProgramEmpty || !size || size > PacketProgramSourceLimit ||
        unsigned(r.source.runtime) > unsigned(PacketProgramRuntime::Wasm)) {
      fail("packet loader request invalid"); return;
    }
    size_t actual;
    if (!store.length(r.slot, r.from, actual) || actual != size) {
      fail("packet source file size differs from saved size"); return;
    }
#ifdef ARDUINO_ARCH_ESP32
    auto freeSource = [](uint8_t *bytes) { heap_caps_free(bytes); };
    std::unique_ptr<uint8_t, decltype(freeSource)> source(
        static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), freeSource);
#else
    std::unique_ptr<uint8_t[]> source(new (std::nothrow) uint8_t[size]);
#endif
    if (!source) { fail("packet source memory allocation failed"); return; }
    if (!store.read(r.slot, r.from, 0, source.get(), size)) {
      fail("packet source read failed"); return;
    }
    if (clockUs() - started > InstallDeadlineUs) {
      fail("packet source lookup/read exceeded the 2 s installation limit"); return;
    }
    uint8_t digest[32]; store.digest(source.get(), size, digest);
    if (memcmp(digest, r.source.hash, sizeof(digest))) {
      fail("packet source SHA256 differs from requested hash"); return;
    }
    result.program = factory ? factory() : nullptr;
    if (!result.program) { fail("packet runtime memory allocation failed"); return; }
    if (!result.program->load(r.source.runtime, source.get(), size,
                              result.error, sizeof(result.error))) return;
    if (r.copy) {
      if (!store.truncate(r.slot, r.to) || !store.append(r.slot, r.to, source.get(), size)) {
        fail("packet durable source write failed"); return;
      }
      if (!store.length(r.slot, r.to, actual) || actual != size ||
          !store.read(r.slot, r.to, 0, source.get(), size)) {
        fail("packet durable source readback failed"); return;
      }
      store.digest(source.get(), size, digest);
      if (memcmp(digest, r.source.hash, sizeof(digest))) {
        fail("packet durable source readback SHA256 differs"); return;
      }
    }
    if (clockUs() - started > InstallDeadlineUs) { fail("packet installation exceeded 2 s"); return; }
    result.ok = true;
  }
  void run() {
    while (!stopping.load(std::memory_order_acquire)) {
      const auto current = state.load(std::memory_order_acquire);
      if (current == Pending) {
        state.store(Running, std::memory_order_release);
        load();
        state.store(Ready, std::memory_order_release);
      } else if (current == Reclaim) {
        delete retired; retired = nullptr;
        live.store(false, std::memory_order_release);
        state.store(Idle, std::memory_order_release);
        leave();
        return;
      } else pause();
    }
    live.store(false, std::memory_order_release);
    leave();
  }
  static void leave() {
#ifdef ARDUINO_ARCH_ESP32
    vTaskDelete(nullptr);
#endif
  }
  static void entry(void *context) { static_cast<Impl *>(context)->run(); }
};
PacketProgramWorker::~PacketProgramWorker() {
  if (!impl_) return;
  impl_->stopping.store(true, std::memory_order_release);
#ifdef ARDUINO_ARCH_ESP32
  while (impl_->live.load(std::memory_order_acquire)) pause();
#else
  if (impl_->task.joinable()) impl_->task.join();
#endif
  delete impl_;
}
bool PacketProgramWorker::submit(const Request &request) {
  if (!impl_) {
    impl_ = new (std::nothrow) Impl(store_, factory_);
    if (!impl_) return false;
  }
  if (busy()) return false;
  // Idle: any earlier task has already cleared `live` and made its last
  // access to the shared state.
  impl_->request = request;
  impl_->live.store(true, std::memory_order_release);
  impl_->state.store(Impl::Pending, std::memory_order_release);
#ifdef ARDUINO_ARCH_ESP32
  if (xTaskCreate(Impl::entry, "mesh-packet-load", 16384, impl_, 1, &impl_->task) != pdPASS) {
    impl_->live.store(false, std::memory_order_release);
    impl_->state.store(Impl::Idle, std::memory_order_release);
    return false;
  }
#else
  if (impl_->task.joinable()) impl_->task.join();
  impl_->task = std::thread(Impl::entry, impl_);
#endif
  return true;
}
bool PacketProgramWorker::poll(Result &result) {
  if (!impl_ || impl_->state.load(std::memory_order_acquire) != Impl::Ready) return false;
  result = impl_->result;
  impl_->result.program = nullptr;
  impl_->state.store(Impl::Held, std::memory_order_release);
  return true;
}
bool PacketProgramWorker::taskActive() const {
  return impl_ && impl_->live.load(std::memory_order_acquire);
}
bool PacketProgramWorker::busy() const {
  return impl_ && impl_->state.load(std::memory_order_acquire) != Impl::Idle;
}
void PacketProgramWorker::release(PacketProgram *program) {
  if (!impl_ || impl_->state.load(std::memory_order_acquire) != Impl::Held) {
    store_.report(0, "Error: packet loader ownership release outside publication");
    delete program; return;
  }
  impl_->retired = program;
  impl_->state.store(Impl::Reclaim, std::memory_order_release);
}
} // namespace onchip
#endif
