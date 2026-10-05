// SPDX-License-Identifier: Apache-2.0
#include "TelemetryService.h"
#if defined(ARDUINO_ARCH_ESP32) && defined(ONCHIP_BOT_HTTPS) && ONCHIP_BOT_HTTPS
#include "Telemetry.h"
#include "Runtime.h"
#include "CommandBot.h"
#include "RoleStorage.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <inttypes.h>

namespace onchip {
namespace {
class NativeSink final : public TelemetrySink {
public:
  bool submit(const char *body, size_t size) override { return commandBotService().submitTelemetry(body, size); }
  bool poll(TelemetryCompletion &result) override { return commandBotService().pollTelemetry(result); }
  void cancel() override { commandBotService().cancelTelemetry(); }
};
NativeSink sink;
TelemetryPublisher publisher(sink);
bool initialized = false;
bool identityReady = false;
char device[19]{};
struct Workspace {
  TelemetrySample sample;
  BotRepeaterSnapshot repeaters[BotRepeaterLimit]{};
  char body[TelemetryBodyLimit + 1]{};
};
unsigned repeaterCursor = 0;
Workspace *workspace = nullptr;
uint64_t uptime() { return uint64_t(esp_timer_get_time()) / 1000; }
void initialize() {
  if (initialized) return;
  initialized = true;
  uint8_t mac[6]{};
  if (esp_efuse_mac_get_default(mac) == ESP_OK) {
    snprintf(device, sizeof(device), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    identityReady = true;
  } else Serial.println("Telemetry hardware identity unavailable; no samples will be sent");
  publisher.begin(uptime());
  telemetryEndpointConfigured();
}
void collect(TelemetrySample &s, const RadioDashboard::RadioStatus &radio,
             WifiKissMultiplexer &mux, mesh::MainBoard &board) {
  s.radio = radio;
  s.largestHeap = ESP.getMaxAllocHeap();
  s.psramTotal = ESP.getPsramSize();
  if (s.psramTotal) {
    s.psramFree = ESP.getFreePsram();
    s.psramMinimum = ESP.getMinFreePsram();
    s.psramLargest = ESP.getMaxAllocPsram();
  }
  s.batteryMv = board.getBattMilliVolts();
  s.temperatureC = board.getMCUTemperature();
  for (size_t i = 0; i < radio.role_count && i < RadioDashboard::ROLE_CAPACITY; ++i) {
    auto &r = s.role[i];
    const int slot = radio.roles[i].source_slot;
    mesh::QueuedRadioStats stats;
    if (slot < 0 || slot > UINT8_MAX || !mux.getQueuedRadioStats(uint8_t(slot), stats)) continue;
    r.available = true;
    r.generation = stats.generation;
    r.creditMs = stats.source_credit_ms;
    r.rfMs = stats.source_rf_ms;
    r.succeeded = stats.source_successes;
    r.failed = stats.source_failures;
    r.queued = mux.sourceQueuedCount(uint8_t(slot));
    r.transmitting = mux.sourceTransmitting(uint8_t(slot));
  }
  if (!commandBotService().publicKey()) return;
  const auto &c = commandBotService().counters();
  auto &lua = s.lua;
  lua.available = true;
  lua.jobsInUse = commandBotService().jobsInUse();
  lua.rejected = c.rejected;
  lua.vmFailures = c.vmFailures;
  lua.replies = c.replies;
  lua.busy = c.busy;
  lua.senderLimited = c.senderLimited;
  lua.channelLimited = c.channelLimited;
  lua.globalLimited = c.globalLimited;
  lua.airtimeLimited = c.airtimeLimited;
  lua.eventsQueued = c.eventsQueued;
  lua.eventsDropped = c.eventsDropped;
  lua.eventsFailed = c.eventsFailed;
  lua.eventsCompleted = c.eventsCompleted;
  lua.vmAvailable = c.lastVm.peakBytes != 0;
  lua.peakBytes = c.lastVm.peakBytes;
  lua.instructions = c.lastVm.instructions;
  lua.elapsedUs = c.lastVm.elapsedUs;
  lua.loadUs = c.lastVm.loadUs;
  lua.initUs = c.lastVm.initUs;
  lua.invokeUs = c.lastVm.invokeUs;
  lua.cleanupUs = c.lastVm.cleanupUs;
  lua.freeInternalBytes = c.lastVm.freeInternalBytes;
  lua.freePsramBytes = c.lastVm.freePsramBytes;
  lua.stackHighWaterBytes = c.lastVm.stackHighWaterBytes;
}
}
void telemetryLoop(RadioDashboard &dashboard, const RadioDashboard::RadioStatus &radio,
                   WifiKissMultiplexer &mux, mesh::MainBoard &board) {
  initialize();
  const auto now = uptime();
  publisher.poll(now);
  if (!publisher.config().enabled && !publisher.status().pending) releaseRoleStorage(workspace);
  if (!publisher.due(now)) return;
  if (!radio.wifi_connected) { publisher.drop(now, TelemetryError::Wifi); return; }
  if (!botHttpsClockTrusted()) { publisher.drop(now, TelemetryError::Clock); return; }
  if (!telemetryEndpointConfigured()) { publisher.drop(now, TelemetryError::Endpoint); return; }
  if (!identityReady) { publisher.drop(now, TelemetryError::Snapshot); return; }
  if (!workspace) workspace = allocateRoleStorage<Workspace>("telemetry");
  if (!workspace) { publisher.drop(now, TelemetryError::Encoding); return; }
  workspace->sample = TelemetrySample{};
  if (!dashboard.totals(workspace->sample.totals)) { publisher.drop(now, TelemetryError::Snapshot); return; }
  char name[32]{};
  if (!kissName(name)) { publisher.drop(now, TelemetryError::Snapshot); return; }
  collect(workspace->sample, radio, mux, board);
  const unsigned peers = commandBotService().repeaterSnapshots(workspace->repeaters, BotRepeaterLimit);
  for (unsigned i = 0; i < std::min(peers, TelemetryRepeaterLimit); ++i)
    workspace->sample.repeaters[i] = workspace->repeaters[(repeaterCursor + i) % peers];
  if (peers) repeaterCursor = (repeaterCursor + TelemetryRepeaterLimit) % peers;
  const auto size = encodeTelemetry(workspace->sample, device, name,
                                    publisher.status(), workspace->body, sizeof(workspace->body));
  publisher.publish(now, workspace->body, size);
}
void telemetryCommand(const char *text, char *reply, size_t capacity) {
  initialize();
  if (!strcmp(text, "identity")) {
    snprintf(reply, capacity, "%s", identityReady ? device : "Error: hardware identity unavailable");
  } else if (!strcmp(text, "endpoint") || !strncmp(text, "endpoint ", 9)) {
    if (!strcmp(text, "endpoint commit")) publisher.configure(publisher.config(), uptime());
    telemetryEndpointCommand(text[8] ? text + 9 : "", reply, capacity);
  } else publisher.command(text, reply, capacity, uptime());
}
} // namespace onchip
#endif
