// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PineAdmin.h"
#include "FirmwareIdentity.h"
#include "onchip/CommandBot.h"
#if MESHCORE_NODE_BACKUP
#include "onchip/NodeBackup.h"
#endif
#include "onchip/BotVm.h"
#include "onchip/Clock.h"
#include "onchip/BotRegistry.h"
#include "PineRuntimePlatform.h"
#include "platform/nvs.h"
#include <Utils.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace onchip {
namespace {
CommandBot bot;
MastAdmin admin;
bool decodeKey(const char *text, uint8_t key[32]) {
  if (!text || strlen(text) != 64) return false;
  bool nonzero = false;
  for (unsigned i = 0; i < 32; ++i) {
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    const int a = digit(text[2*i]), b = digit(text[2*i+1]);
    if (a < 0 || b < 0) return false;
    key[i] = a * 16 + b; nonzero |= key[i] != 0;
  }
  return nonzero;
}
bool number(const char *text, uint32_t &value, uint32_t maximum = UINT32_MAX) {
  if (!text || !*text) return false;
  uint64_t result = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || (result = result * 10 + unsigned(*p - '0')) > maximum) return false;
  }
  value = result; return true;
}
}
CommandBot &commandBotService() { return bot; }
MastAdmin *MastAdmin::service() { return &admin; }
bool MastAdmin::begin() {
  bool present = false;
  Owner loaded;
  bool ownerValid = mastRecord("pine-owner", &loaded, sizeof(loaded), false, present);
  if (ownerValid && present) {
    uint8_t hash[32];
    mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(&loaded), offsetof(Owner, digest));
    ownerValid = !memcmp(loaded.magic, "PLO\1", 4) && !memcmp(hash, loaded.digest, 32);
    if (ownerValid) owner_ = loaded;
  }
  if (!ownerValid) owner_ = {};
  ready_ = true;
  const bool sourceReady = source_.begin();
  return ownerValid && sourceReady;
}
void MastAdmin::stop() { ready_ = false; source_.stop(); }
void MastAdmin::loop() {
  if (ready_) source_.loop();
  if (rebootAt_ && int32_t(millis() - rebootAt_) >= 0) NVIC_SystemReset();
}
bool MastAdmin::trusted(const uint8_t key[32]) const {
  bool set = false; for (auto byte : owner_.key) set |= byte != 0;
  return ready_ && set && key && !memcmp(owner_.key, key, 32);
}
bool MastAdmin::saveOwner(const Owner &next) {
  Owner sealed = next;
  mesh::Utils::sha256(sealed.digest, 32, reinterpret_cast<const uint8_t *>(&sealed), offsetof(Owner, digest));
  bool present;
  if (!mastRecord("pine-owner", &sealed, sizeof(sealed), true, present)) return false;
  Owner check;
  if (!mastRecord("pine-owner", &check, sizeof(check), false, present) || !present ||
      memcmp(&sealed, &check, sizeof(check))) return false;
  owner_ = check; return true;
}
bool MastAdmin::rememberTimestamp(const uint8_t key[32], uint32_t timestamp) {
  if (!trusted(key) || !timestamp || timestamp <= owner_.timestamp) return false;
  Owner next = owner_; next.timestamp = timestamp; return saveOwner(next);
}
void MastAdmin::acknowledged(uint32_t ticket, bool transmitted) {
  if (ticket && ticket == rebootTicket_) {
    rebootTicket_ = 0;
    if (transmitted) rebootAt_ = millis() + 1000;
  }
}
void MastAdmin::execute(const char *text, Reply &reply, uint32_t invokingJob,
                        Transport transport, const uint8_t *, size_t replyCapacity) {
  reply = {};
  auto say = [&](const char *message) { snprintf(reply.text, sizeof(reply.text), "%s", message); };
  if (!text) { say("Error: empty Pine admin command"); return; }
  if (strlen(text) > sizeof(reply.text) - 1) { say("Error: Pine admin command exceeds 162 bytes"); return; }
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
    if (*p < 32 || *p > 126) { say("Error: printable Pine admin text required"); return; }
  if (!strcmp(text, "ver")) { say("v" MESHCORE_SLP_PINE_VERSION); return; }
#if MESHCORE_NODE_BACKUP
  if (!strcmp(text, "backup") || !strcmp(text, "help backup") || !strncmp(text, "backup ", 7)) {
    if (invokingJob) {
      say("Error: node backups require direct authenticated administration"); return;
    }
    nodeBackup().command(!strncmp(text, "backup ", 7) ? text + 7 : "help",
                         reply.text, std::min(replyCapacity, sizeof(reply.text)), transport == Transport::NativeEncrypted);
    return;
  }
#endif
  if (!strcmp(text, "help") || !strcmp(text, "help bot") || !strcmp(text, "bot")) {
    say("bot help; source help; backup help; help wifi; bot status|mesh|policy|owner|time|adaptive; source status|hash|api; repeater role settings");
    return;
  }
  if (!strcmp(text, "help source") || !strcmp(text, "source")) {
    say("source help; source status|hash|metadata|helptext|api; source begin|chunk|commit|rollback|remove ..."); return;
  }
  if (!strcmp(text, "help wifi") || !strcmp(text, "wifi help") ||
      !strncmp(text, "get wifi.", 9) || !strncmp(text, "set wifi.", 9)) {
    say("Error: slp-pine has no WiFi; use native repeater settings and encrypted RF source administration"); return;
  }
  if (!strncmp(text, "source ", 7)) { source_.execute(text + 7, reply.text, sizeof(reply.text)); return; }
  if (!strncmp(text, "bot data ", 9)) { bot.dataCommand(text + 9, reply.text, sizeof(reply.text)); return; }
  if (!strcmp(text, "bot help")) {
    say("bot status|stats|radio|diagnostics|limits|memory|admission|adaptive|policy|mesh|discovery|name|advert|shared|reminders|events|forward|owner|time|cancel; source help");
  } else if (!strcmp(text, "bot status")) {
    RadioDashboard::RoleStatus status; bot.dashboardStatus(status);
    snprintf(reply.text, sizeof(reply.text), "Lua %s ready=%u generation=%lu runtime_epoch=%lu jobs=%u/%u cooldown=0%s%s",
             status.state, status.ready, (unsigned long)bot.sourceWorker().sourceGeneration(),
             (unsigned long)bot.sourceWorker().generation(), bot.jobsInUse(), BotJobLimit,
             status.fault[0] ? " last-error=" : "", status.fault);
  } else if (!strcmp(text, "bot stats")) {
    const auto &s = bot.counters();
    snprintf(reply.text, sizeof(reply.text), "replies=%lu rejected=%lu vm_failures=%lu peak=%uB instructions=%lu stack=%luB events=%lu/%lu",
             (unsigned long)s.replies, (unsigned long)s.rejected, (unsigned long)s.vmFailures,
             unsigned(s.lastVm.peakBytes), (unsigned long)s.lastVm.instructions,
             (unsigned long)s.lastVm.stackHighWaterBytes, (unsigned long)s.eventsCompleted, (unsigned long)s.eventsFailed);
  } else if (!strcmp(text, "bot radio")) {
    mesh::QueuedRadioStats queue{};
    if (!bot.radio_.getQueuedRadioStats(queue)) {
      say("Error: Lua radio measurements unavailable"); return;
    }
    snprintf(reply.text, sizeof(reply.text),
             "Lua queued=%u confirmed=%lu failed=%lu RF=%lums; shared queued=%u transmitting=%u confirmed=%lu failed=%lu RF=%lums",
             bot.radio_.queuedCount(), (unsigned long)queue.source_successes,
             (unsigned long)queue.source_failures, (unsigned long)queue.source_rf_ms,
             queue.aggregate_queued, queue.aggregate_transmitting,
             (unsigned long)queue.aggregate_successes, (unsigned long)queue.aggregate_failures,
             (unsigned long)queue.aggregate_rf_ms);
  } else if (!strcmp(text, "bot diagnostics") || !strcmp(text, "bot log")) {
    bot.diagnosticStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(text, "bot admission")) {
    bot.admissionStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(text, "bot adaptive") || !strncmp(text, "bot adaptive ", 13)) {
    bot.adaptiveCommand(text, reply.text, sizeof(reply.text));
  } else if (!strcmp(text, "bot limits")) {
    const BotVmLimits limits;
    snprintf(reply.text, sizeof(reply.text), "Lua5.5.1 source=%u heap=%u jobs=%u exports=%u modules=%u KV=16+8/128B tx=%u; load/init/active=%u/%u/%ums; HTTPS/WiFi unsupported; cooldown=0",
             unsigned(BotSourceLimit), unsigned(limits.heapBytes), BotJobLimit, BotCommandLimit, BotModuleLimit,
             BotTransactionLimit, unsigned(limits.loadWallUs / 1000), unsigned(limits.initWallUs / 1000),
             unsigned(limits.wallUs / 1000));
  } else if (!strcmp(text, "bot memory")) {
    snprintf(reply.text, sizeof(reply.text), "heap_free=%dB VM_storage=%uB worker=%uB bot=%uB stacks=VM16KiB/storage6KiB/dispatch10KiB; source update serial",
             dbgHeapFree(), unsigned(BotSession::StorageBytes), unsigned(BotWorker::StorageBytes), unsigned(CommandBot::StorageBytes));
  } else if (!strcmp(text, "bot name")) {
    say(bot.botName() ? bot.botName() : "Error: Lua bot unavailable");
  } else if (!strcmp(text, "bot advert") || !strcmp(text, "bot advert zero")) {
    say(bot.advertise(!strcmp(text, "bot advert zero")) ? "Bot advert queued; check companion reception over RF" : "Error: bot advert radio admission failed");
  } else if (!strcmp(text, "bot home") || !strncmp(text, "bot home ", 9) ||
             !strncmp(text, "bot https", 9) || !strncmp(text, "wifi", 4)) {
    say("Error: HTTPS and WiFi are unsupported on Pine; use mesh.send/kv/timers and authenticated RF source administration");
  } else if (!strcmp(text, "bot cancel")) {
    bot.cancelJobs(invokingJob); say("Cancelled other bot jobs; admitted effects may have committed");
  } else if (!strcmp(text, "bot shared")) {
    say(bot.sharedState() ? "Shared KV granted" : "Shared KV denied; caller KV remains available");
  } else if (!strcmp(text, "bot shared on") || !strcmp(text, "bot shared off")) {
    say(bot.setSharedState(!strcmp(text, "bot shared on")) ? "Saved/applied shared KV grant" : "Error: shared KV grant persistence failed");
  } else if (!strcmp(text, "bot reminders")) {
    say(bot.reminderAccess() ? "Reminder grant on; authenticated UTC required" : "Reminder grant off");
  } else if (!strcmp(text, "bot reminders on") || !strcmp(text, "bot reminders off")) {
    say(bot.setReminderAccess(!strcmp(text, "bot reminders on")) ? "Saved/applied reminder grant" : "Error: reminder grant persistence failed");
  } else if (!strcmp(text, "bot events")) {
    snprintf(reply.text, sizeof(reply.text), "Events granted=%u subscribed=%u; 1 startup,2 connectivity,4 message,8 node_status,16 recurring", bot.eventAccess(), bot.eventMask());
  } else if (!strncmp(text, "bot events ", 11)) {
    uint32_t mask;
    say(number(text + 11, mask, 31) && bot.setEventAccess(mask) ? "Saved/applied event grants" : "Error: bot events MASK 0..31");
  } else if (!strcmp(text, "bot repeaters") || !strncmp(text, "bot repeaters ", 14)) {
    bot.repeaterCommand(text[13] ? text + 14 : "", reply.text, sizeof(reply.text));
  } else if (!strncmp(text, "bot discovery", 13)) {
    bot.discoveryCommand(text + 13 + (text[13] == ' '), reply.text, sizeof(reply.text));
  } else if (!strcmp(text, "bot mesh")) {
    bot.meshPolicyStatus(reply.text, sizeof(reply.text));
  } else if (!strncmp(text, "bot name ", 9) || !strncmp(text, "bot destination ", 16) ||
             !strncmp(text, "bot channel-wait ", 17)) {
    BotMeshPolicy policy;
    if (!loadBotMeshPolicy(policy)) { say("Error: saved mesh policy unavailable"); return; }
    if (!strncmp(text, "bot name ", 9)) {
      const char *name = text + 9;
      const size_t size = strlen(name);
      bool valid = size && size <= 31 && name[0] != ' ' && name[size - 1] != ' ';
      for (size_t i = 0; i < size; ++i) valid &= name[i] >= 32 && name[i] <= 126;
      if (!valid) { say("Error: bot name requires 1-31 printable ASCII bytes without edge spaces"); return; }
      strcpy(policy.name, text + 9);
    } else if (!strncmp(text, "bot channel-wait ", 17)) {
      if (strcmp(text + 17, "on") && strcmp(text + 17, "off")) { say("Error: bot channel-wait on|off"); return; }
      policy.channelWait = !strcmp(text + 17, "on");
    } else {
      uint32_t slot; char index[2]{text[16], 0};
      if (text[17] != ' ' || !number(index, slot, 3)) { say("Error: bot destination SLOT 0..3 KEY64|off"); return; }
      if (!strcmp(text + 18, "off")) memset(policy.destinations[slot], 0, 32);
      else if (!decodeKey(text + 18, policy.destinations[slot])) { say("Error: destination requires full public key"); return; }
    }
    if (!bot.setMeshPolicy(policy)) {
      say("Error: invalid mesh policy or commit/readback failed");
    } else if (!strncmp(text, "bot name ", 9) && !nrfmast::saveLuaCarrierName(policy.name)) {
      say("Error: Lua name saved; native companion name commit failed; inspect bot name and retry");
    } else say("Saved/applied mesh policy; existing grants fenced");
  } else if (!strcmp(text, "bot forward") || !strcmp(text, "bot forward from") || !strcmp(text, "bot forward to")) {
    BotForwardPolicy policy;
    if (!loadBotForwardPolicy(policy)) { say("Error: forward policy unavailable"); return; }
    if (!strcmp(text, "bot forward")) say(policy.enabled() ? "Forward grant on; use bot forward from|to" : "Forward grant off");
    else mesh::Utils::toHex(reply.text, !strcmp(text, "bot forward from") ? policy.from : policy.to, 32);
  } else if (!strncmp(text, "bot forward ", 12)) {
    BotForwardPolicy policy;
    if (strcmp(text + 12, "off")) {
      char from[65]{};
      if (strlen(text + 12) != 129 || text[76] != ':') { say("Error: bot forward FROM64:TO64|off"); return; }
      memcpy(from, text + 12, 64);
      if (!decodeKey(from, policy.from) || !decodeKey(text + 77, policy.to)) { say("Error: forward requires two full public keys"); return; }
    }
    say(bot.setForwardPolicy(policy) ? "Saved/applied forward grant" : "Error: forward grant rejected or persistence unknown");
  } else if (!strcmp(text, "bot membership") || !strncmp(text, "bot membership ", 15) ||
             !strcmp(text, "bot access") || !strncmp(text, "bot access ", 11) ||
             !strcmp(text, "bot thread") || !strncmp(text, "bot thread ", 11)) {
    bot.radioPolicyCommand(text + 4, reply.text, sizeof(reply.text));
  } else if (!strcmp(text, "bot policy") || !strncmp(text, "bot channel ", 12) ||
             !strncmp(text, "bot path ", 9) || !strncmp(text, "bot airtime ", 12)) {
    BotRadioPolicy policy;
    if (!loadBotRadioPolicy(policy)) { say("Error: saved radio policy unavailable"); return; }
    if (!strcmp(text, "bot policy")) {
      snprintf(reply.text, sizeof(reply.text), "Saved channel=%s path-bytes=%u airtime-ms/min=%u; applied on boot",
               policy.channel[0] ? policy.channel : "off", policy.pathWidth, policy.airtimeMs); return;
    }
    if (!strncmp(text, "bot channel ", 12)) {
      BotRadioPolicy::Membership membership;
      if (!strcmp(text + 12, "off")) membership.name[0] = 0;
      else if (strlen(text + 12) < sizeof(membership.name)) strcpy(membership.name, text + 12);
      else { say("Error: channel must be at most 32 bytes"); return; }
      policy.setMembership(0, membership);
    } else {
      uint32_t value; const bool path = !strncmp(text, "bot path ", 9);
      if (!number(text + (path ? 9 : 12), value, path ? 3 : 3600) || value < (path ? 1 : 360)) {
        say("Error: bot path 1..3; bot airtime 360..3600 ms/min"); return;
      }
      if (path) policy.pathWidth = value; else policy.airtimeMs = value;
    }
    say(saveBotRadioPolicy(policy) ? "Saved bot radio policy; reboot to apply" : "Error: invalid channel or radio policy commit failed");
  } else if (!strcmp(text, "bot owner")) {
    mesh::Utils::toHex(reply.text, owner_.key, 32);
  } else if (!strncmp(text, "bot owner ", 10)) {
    if (transport != Transport::NativeEncrypted || invokingJob) { say("Error: owner changes require native repeater admin"); return; }
    Owner next;
    if (strcmp(text + 10, "off") && !decodeKey(text + 10, next.key)) { say("Error: bot owner KEY64|off"); return; }
    bot.cancelJobs(invokingJob);
    say(saveOwner(next) ? "Saved trusted bot owner; identities unchanged" : "Error: owner commit/readback failed; inspect bot owner");
  } else if (!strcmp(text, "bot time")) {
    uint32_t earliest, latest; const bool trusted = trustedNetworkTime(earliest, latest);
    snprintf(reply.text, sizeof(reply.text), "UTC trusted=%u source=%s bounds=%lu..%lu; GPS/paired BLE/admin or saved bot.time.provider; RF refresh=15min; no RTC fallback",
             trusted, nrfmast::luaTimeSource(), (unsigned long)earliest, (unsigned long)latest);
  } else if (!strncmp(text, "bot time ", 9)) {
    uint32_t utc;
    if (!number(text + 9, utc) || utc < 1715770351u || utc > 4102444800u) { say("Error: bot time requires UTC seconds 1715770351..4102444800"); return; }
    nrfmast::trustLuaTime(utc); say("Authenticated UTC accepted for 1h; pending deadlines retained; completed/claimed jobs never rearmed");
  } else if (!strcmp(text, "reboot") || !strcmp(text, "apply")) {
    rebootTicket_ = 1; reply.ticket = 1;
    if (transport == Transport::NativeEncrypted && !invokingJob) { rebootTicket_ = 0; rebootAt_ = millis() + 3000; }
    say("Pine reboot scheduled after admin response; identities/source/state retained");
  } else {
    say("Error: unknown Pine Lua admin command; use help, help bot or help source");
  }
}
}
#endif
