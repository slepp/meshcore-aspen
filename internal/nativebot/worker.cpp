// SPDX-License-Identifier: Apache-2.0
#include "CommandBot.h"
#include "MastSource.h"
#include "BotSignal.h"
#include "SPIFFS.h"
#include "nvs.h"
#include "NativeClock.h"
#include "Scopes.h"
#include <QueuedTxProtocol.h>
#include <Utils.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>

namespace onchip {
CommandBot &commandBotService();
class HostBotOwner {
  static inline std::chrono::steady_clock::time_point lastAdvert_{};
public:
  enum class AdvertResult { Queued, RateLimited, Unavailable };
  static AdvertResult advertise(CommandBot &bot) {
    const auto now = std::chrono::steady_clock::now();
    if (lastAdvert_ != std::chrono::steady_clock::time_point{} &&
        now - lastAdvert_ < std::chrono::minutes(1))
      return AdvertResult::RateLimited;
    if (!bot.advertiseOwnerZeroHop()) return AdvertResult::Unavailable;
    lastAdvert_ = now;
    return AdvertResult::Queued;
  }
};
}

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t MaxFrame = 4096;
constexpr size_t HelloSize = 1 + 2 + 64 + 4 + 4 + 3 + 2 + 256 * 4;
bool output_fault = false;

bool load_scopes(const char *root) {
  const std::string path = std::string(root) + "/scopes";
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT;
  struct stat st{};
  uint8_t data[37]{};
  const bool valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
      (st.st_mode & 0777) == 0600 && st.st_size == 36 &&
      read(fd, data, sizeof(data)) == 36 && !memcmp(data, "SCP1", 4);
  close(fd);
  if (!valid) return false;
  memcpy(onchip::nativeHostScopes.home.key, data + 4, 16);
  memcpy(onchip::nativeHostScopes.fallback.key, data + 20, 16);
  onchip::nativeHostScopes.configured = true;
  explicit_bzero(data, sizeof(data));
  return true;
}

bool apply_host_name(const char *root) {
  const std::string path = std::string(root) + "/host-name";
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return errno == ENOENT;
  struct stat st{};
  char data[36]{};
  const bool valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
      (st.st_mode & 07777) == 0600 && st.st_nlink == 1 && st.st_size >= 5 && st.st_size <= 35 &&
      read(fd, data, sizeof(data)) == st.st_size && !memcmp(data, "HNM1", 4) &&
      !memchr(data + 4, 0, size_t(st.st_size - 4));
  close(fd);
  if (!valid) return false;
  onchip::BotMeshPolicy policy;
  if (!onchip::loadBotMeshPolicy(policy)) return false;
  if (!strcmp(policy.name, data + 4)) return true;
  strcpy(policy.name, data + 4);
  return policy.valid() && onchip::saveBotMeshPolicy(policy);
}

bool write_all(const uint8_t *data, size_t length) {
  const auto deadline = Clock::now() + std::chrono::seconds(1);
  while (length) {
    const ssize_t count = ::write(STDOUT_FILENO, data, length);
    if (count > 0) { data += count; length -= size_t(count); continue; }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && errno == EAGAIN) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) return false;
      pollfd fd{STDOUT_FILENO, POLLOUT, 0};
      if (poll(&fd, 1, int(left)) > 0 && (fd.revents & POLLOUT)) continue;
    }
    return false;
  }
  return true;
}
bool send(uint8_t tag, const uint8_t *body, size_t length) {
  if (output_fault || length + 1 > MaxFrame) return false;
  std::array<uint8_t, MaxFrame + 4> frame{};
  queued_tx::put32(frame.data(), uint32_t(length + 1));
  frame[4] = tag;
  if (length) std::memcpy(frame.data() + 5, body, length);
  if (!write_all(frame.data(), length + 5)) output_fault = true;
  return !output_fault;
}
int fail(uint8_t code, const char *message) {
  send(0x84, &code, 1);
  std::fprintf(stderr, "Host bot: %s\n", message);
  return 1;
}
void secure_random(uint8_t *data, size_t size) {
  while (size) {
    const ssize_t count = getrandom(data, size, 0);
    if (count > 0) { data += count; size -= size_t(count); }
    else if (count < 0 && errno == EINTR) continue;
    else { std::fputs("Host bot: CSPRNG unavailable\n", stderr); std::abort(); }
  }
}
bool valid_profile(const RadioConfig &profile, int noise, const uint32_t airtime[256]) {
  if (profile.freq_hz < 150000000 || profile.freq_hz > 960000000 ||
      profile.bw_hz < 7800 || profile.bw_hz > 500000 ||
      profile.sf < 7 || profile.sf > 12 || profile.cr < 5 || profile.cr > 8 ||
      profile.tx_power > 22 || noise < -150 || noise > 0) return false;
  for (unsigned n = 1; n < 256; ++n)
    if (!airtime[n] || airtime[n] > 3600000 ||
        (n > 1 && airtime[n] < airtime[n - 1])) return false;
  return true;
}
bool parse_hello(const uint8_t *frame, size_t length, uint8_t identity[64],
                 RadioConfig &profile, int &noise, uint32_t airtime[256]) {
  if (length != HelloSize || frame[0] != 0x01 ||
      queued_tx::get16(frame + 1) != 1) return false;
  std::memcpy(identity, frame + 3, 64);
  profile.freq_hz = queued_tx::get32(frame + 67);
  profile.bw_hz = queued_tx::get32(frame + 71);
  profile.sf = frame[75]; profile.cr = frame[76]; profile.tx_power = frame[77];
  noise = int16_t(queued_tx::get16(frame + 78));
  for (unsigned n = 0; n < 256; ++n)
    airtime[n] = queued_tx::get32(frame + 80 + 4 * n);
  return valid_profile(profile, noise, airtime);
}
int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool decode_hex(const char *text, size_t count, uint8_t *output) {
  for (size_t i = 0; i < count; ++i) {
    const int high = hex_digit(text[2 * i]), low = hex_digit(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    output[i] = uint8_t(high * 16 + low);
  }
  return true;
}
bool reminder_clock() {
  uint32_t earliest = 0, latest = 0;
  return onchip::trustedNetworkTime(earliest, latest);
}
void grant_status(onchip::CommandBot &bot, char *reply, size_t capacity) {
  bool shared = false, reminders = false;
  uint8_t events = 0;
  if (!onchip::loadBotSharedState(shared) ||
      !onchip::loadBotReminderAccess(reminders) ||
      !onchip::loadBotEventAccess(events)) {
    std::snprintf(reply, capacity, "Error: saved bot grants unavailable; inspect worker diagnostics");
    return;
  }
  std::snprintf(reply, capacity,
      "Grants shared saved=%u applied=%u; reminders saved=%u applied=%u clock=%u; events saved=%u applied=%u subscribed=%u",
      shared, bot.sharedState(), reminders, bot.reminderAccess(), reminder_clock(), events,
      bot.eventAccess(), bot.eventMask());
}
void source_grants(onchip::CommandBot &bot, onchip::MastSource &source, const char *command,
                   char *reply, size_t capacity) {
  bool saved = false;
  unsigned applied = 0;
  const auto flag = [&](const char *field, bool enabled) {
    if (char *value = std::strstr(reply, field))
      value[std::strlen(field)] = enabled ? '1' : '0';
  };
  if (!std::strcmp(command, "api events")) {
    uint8_t mask = 0;
    if (!onchip::loadBotEventAccess(mask)) {
      std::snprintf(reply, capacity, "Error: saved event grant unavailable");
      return;
    }
    // The source API describes support; host readback also identifies live grants.
    flag("active=", source.ready() && bot.eventMask());
    const size_t size = std::strlen(reply);
    std::snprintf(reply + size, capacity - size, " saved=%u applied=%u subscribed=%u",
                  mask, bot.eventAccess(), bot.eventMask());
    return;
  }
  if (!std::strcmp(command, "api storage") || !std::strcmp(command, "api board")) {
    if (!onchip::loadBotSharedState(saved)) {
      std::snprintf(reply, capacity, "Error: saved shared-state grant unavailable");
      return;
    }
    applied = bot.sharedState();
    if (!std::strcmp(command, "api storage"))
      flag("autonomous-reminders=", bot.reminderAccess() && reminder_clock());
  } else if (!std::strcmp(command, "api reminders")) {
    if (!onchip::loadBotReminderAccess(saved)) {
      std::snprintf(reply, capacity, "Error: saved reminder grant unavailable");
      return;
    }
    applied = bot.reminderAccess();
    flag("autonomous=", applied && reminder_clock());
  } else return;
  const size_t size = std::strlen(reply);
  std::snprintf(reply + size, capacity - size, " saved=%u applied=%u", saved, applied);
  if (!std::strcmp(command, "api reminders")) {
    const size_t size = std::strlen(reply);
    std::snprintf(reply + size, capacity - size, " clock=%u", reminder_clock());
  }
}
bool admin(onchip::CommandBot &bot, onchip::MastSource &source,
           const uint8_t *frame, size_t length) {
  if (length < 6 || length > 165) return false;
  const size_t command_size = length - 5;
  char command[161]{}, reply[257]{};
  for (size_t i = 0; i < command_size; ++i)
    if (frame[5 + i] < 32 || frame[5 + i] > 126) return false;
  std::memcpy(command, frame + 5, command_size);
  if (!std::strcmp(command, "help")) {
    std::strcpy(reply, "Native commands: help grants; status; policy; clock; shared; reminders; events; cancel; source help; data help; https status; home; config; discovery; name; channel; path; airtime; adaptive; advert.zerohop; key bot (Go owner). Grants: help grants");
  } else if (!std::strcmp(command, "help grants")) {
    std::strcpy(reply, "shared/reminders [status|on|off]; events [status|MASK 0..15] (1 startup,2 connectivity,4 message,8 node_status); policy reads saved/applied; cancel stops commands/events, not reminders. Defaults off; package capabilities never authorize scripts.");
  } else if (!std::strcmp(command, "status")) {
    const auto &stats = bot.counters();
    std::snprintf(reply, sizeof(reply),
        "Native bot ready=%u jobs=%u; events queued=%u dropped=%u completed=%u failed=%u; grants: policy",
        source.ready(), bot.jobsInUse(), stats.eventsQueued, stats.eventsDropped,
        stats.eventsCompleted, stats.eventsFailed);
#if ONCHIP_BOT_SINGLE_SESSION
    const size_t used = strlen(reply);
    std::snprintf(reply + used, sizeof(reply) - used, " runtime_epoch=%u source_generation=%u",
                  bot.sourceWorker().generation(), bot.sourceWorker().sourceGeneration());
#endif
  } else if (!std::strcmp(command, "clock")) {
    onchip::NativeClockSample clock;
    bool scheduler = onchip::nativeBotClockSample(clock);
    uint32_t earliest = 0, latest = 0;
    if (scheduler && !onchip::trustedNetworkTime(earliest, latest)) {
      scheduler = false;
      clock.reason = "scheduler-publication-untrusted";
    }
    std::snprintf(reply, sizeof(reply),
        "Clock https=%u scheduler=%u uncertainty-us=%llu scheduler-limit-us=%llu reason=%s",
        clock.httpsTrusted, scheduler, static_cast<unsigned long long>(clock.errorBoundUs),
        static_cast<unsigned long long>(onchip::NativeSchedulerErrorLimitUs), clock.reason);
  } else if (!std::strcmp(command, "policy") || !std::strcmp(command, "policy status") ||
             !std::strcmp(command, "source api grants")) {
    grant_status(bot, reply, sizeof(reply));
  } else if (!std::strcmp(command, "shared") || !std::strncmp(command, "shared ", 7)) {
    const char *argument = command[6] ? command + 7 : "";
    if (!std::strcmp(command, "shared") || !std::strcmp(argument, "status")) {
      bool saved = false;
      if (!onchip::loadBotSharedState(saved)) std::strcpy(reply, "Error: saved shared-state grant unavailable");
      else std::snprintf(reply, sizeof(reply), "Shared state saved=%u applied=%u; bot/channel KV and timers; private caller scopes unchanged",
                        saved, bot.sharedState());
    } else if (!std::strcmp(argument, "on") || !std::strcmp(argument, "off")) {
      std::strcpy(reply, bot.setSharedState(!std::strcmp(argument, "on")) ?
          "Saved/applied shared-state grant; inspect shared status" :
          "Error: shared-state grant save/apply failed; inspect shared status and worker diagnostics");
    } else std::strcpy(reply, "Error: shared [status|on|off]; exact on/off required; policy unchanged");
  } else if (!std::strcmp(command, "reminders") || !std::strncmp(command, "reminders ", 10)) {
    const char *argument = command[9] ? command + 10 : "";
    if (!std::strcmp(command, "reminders") || !std::strcmp(argument, "status")) {
      bool saved = false;
      if (!onchip::loadBotReminderAccess(saved)) std::strcpy(reply, "Error: saved reminder grant unavailable");
      else std::snprintf(reply, sizeof(reply), "Personal reminders saved=%u applied=%u clock=%u; authenticated private DM, trusted UTC, direct route, no retries",
                        saved, bot.reminderAccess(), reminder_clock());
    } else if (!std::strcmp(argument, "on") || !std::strcmp(argument, "off")) {
      std::strcpy(reply, bot.setReminderAccess(!std::strcmp(argument, "on")) ?
          "Saved/applied reminder grant; off suspends pending reminders; inspect reminders status" :
          "Error: reminder grant save/apply failed; live access disabled; inspect reminders status");
    } else std::strcpy(reply, "Error: reminders [status|on|off]; exact on/off required; policy unchanged");
  } else if (!std::strcmp(command, "events") || !std::strncmp(command, "events ", 7)) {
    const char *argument = command[6] ? command + 7 : "";
    if (!std::strcmp(command, "events") || !std::strcmp(argument, "status")) {
      uint8_t saved = 0;
      if (!onchip::loadBotEventAccess(saved)) std::strcpy(reply, "Error: saved event grant unavailable");
      else std::snprintf(reply, sizeof(reply), "Events saved=%u applied=%u subscribed=%u; mask 1 startup,2 connectivity,4 message,8 node_status",
                        saved, bot.eventAccess(), bot.eventMask());
    } else if (*argument && std::strlen(argument) <= 2 &&
               std::strspn(argument, "0123456789") == std::strlen(argument) &&
               std::strtoul(argument, nullptr, 10) <= 15) {
      std::strcpy(reply, bot.setEventAccess(uint8_t(std::strtoul(argument, nullptr, 10))) ?
          "Saved/applied event grant; inspect events status for source subscriptions" :
          "Error: event grant save/apply failed; live events disabled; inspect events status");
    } else std::strcpy(reply, "Error: events [status|MASK 0..15]; 1 startup,2 connectivity,4 message,8 node_status; policy unchanged");
  } else if (!std::strcmp(command, "cancel")) {
    bot.cancelJobs();
    std::strcpy(reply, "Cancelled running bot commands/events; admitted RF/storage effects may be unknown; personal reminders unchanged");
  } else if (!std::strncmp(command, "source ", 7) && command[7]) {
    source.execute(command + 7, reply, sizeof(reply));
    if (std::strncmp(reply, "Error:", 6)) source_grants(bot, source, command + 7, reply, sizeof(reply));
  } else if (!std::strncmp(command, "data ", 5) && command[5]) {
    bot.dataCommand(command + 5, reply, sizeof(reply));
  } else if (!std::strcmp(command, "https") || !std::strncmp(command, "https ", 6)) {
    if (!onchip::botHttpsAdmin(command[5] ? command + 6 : "", reply, sizeof(reply))) {
      char reason[sizeof(reply)];
      std::snprintf(reason, sizeof(reason), "%s",
                    reply[0] ? reply : "Invalid configured HTTPS command");
      std::snprintf(reply, sizeof(reply), "Error: %.*s", int(sizeof(reply) - 8), reason);
    }
  } else if (!std::strcmp(command, "home")) {
    bool saved = false;
    if (!onchip::loadBotHomeAccess(saved)) std::strcpy(reply, "Error: saved network grant unavailable");
    else std::snprintf(reply, sizeof(reply), "Home configured=%u saved=%u applied=%u clock=%u",
        onchip::botHttpsConfigured(), saved, bot.homeAccess(), onchip::botHttpsClockTrusted());
  } else if (!std::strcmp(command, "home on") || !std::strcmp(command, "home off")) {
    std::strcpy(reply, bot.setHomeAccess(!std::strcmp(command, "home on")) ?
        "Saved/applied home network grant" : "Error: home grant requires configured HTTPS and synchronized clock");
  } else if (!std::strcmp(command, "config")) {
    std::strcpy(reply, "role=bot name=persistent/live key=Go-owner/staged-apply channel-slots=1 channel-key-bits=128 RF=shared");
  } else if (!std::strcmp(command, "discovery") || !std::strncmp(command, "discovery ", 10)) {
    bot.discoveryCommand(command[9] ? command + 10 : "", reply, sizeof(reply));
  } else if (!std::strcmp(command, "name")) {
    onchip::BotMeshPolicy policy;
    if (!onchip::loadBotMeshPolicy(policy)) std::strcpy(reply, "Error: saved bot name unavailable");
    else std::snprintf(reply, sizeof(reply), "Name: %s", policy.name);
  } else if (!std::strcmp(command, "advert.zerohop")) {
    const auto result = source.ready() ? onchip::HostBotOwner::advertise(bot) :
        onchip::HostBotOwner::AdvertResult::Unavailable;
    std::strcpy(reply, result == onchip::HostBotOwner::AdvertResult::Queued ?
        "Bot zero-hop advert queued; verify RF" :
        result == onchip::HostBotOwner::AdvertResult::RateLimited ?
        "Error: owner advert rate limited (1 minute)" :
        "Error: bot source unavailable, radio or queue busy, or airtime exhausted");
  } else if (!std::strncmp(command, "name ", 5)) {
    const char *name = command + 5;
    onchip::BotMeshPolicy policy;
    if (!onchip::loadBotMeshPolicy(policy)) {
      std::strcpy(reply, "Error: saved bot name unavailable");
    } else if (std::strlen(name) > 31) {
      std::strcpy(reply, "Error: name requires 1..31 printable bytes without colon");
    } else {
      std::strcpy(policy.name, name);
      std::strcpy(reply, policy.valid() ?
          (bot.setMeshPolicy(policy) ? "Saved and applied bot name; identity unchanged" :
                                      "Error: name persistence unknown; inspect bot mesh") :
          "Error: name requires 1..31 printable bytes without colon");
    }
  } else if (!std::strcmp(command, "adaptive") || !std::strncmp(command, "adaptive ", 9)) {
    char action[32];
    const int length = std::snprintf(action, sizeof(action), "bot %s", command);
    if (length < 0 || size_t(length) >= sizeof(action))
      std::strcpy(reply, "Error: use adaptive [on|off]");
    else bot.adaptiveCommand(action, reply, sizeof(reply));
  } else if (!std::strcmp(command, "channel status")) {
    onchip::BotRadioPolicy policy;
    if (!onchip::loadBotRadioPolicy(policy)) {
      std::strcpy(reply, "Error: saved bot channel unavailable");
    } else {
      char key_id[17] = "none";
      if (policy.channelKeySet) {
        uint8_t digest[8];
        mesh::Utils::sha256(digest, sizeof(digest), policy.channelKey, 16);
        for (unsigned i = 0; i < sizeof(digest); ++i)
          std::snprintf(key_id + 2 * i, 3, "%02x", digest[i]);
      } else if (policy.channel[0]) {
        std::strcpy(key_id, "hashtag");
      }
      std::snprintf(reply, sizeof(reply), "Channel 0 name=%s key-id=%s; saved; reboot applies",
                    policy.channel[0] ? policy.channel : "off", key_id);
    }
  } else if (!std::strncmp(command, "channel ", 8)) {
    onchip::BotRadioPolicy policy;
    if (!onchip::loadBotRadioPolicy(policy)) {
      std::strcpy(reply, "Error: saved bot channel unavailable");
    } else if (!std::strcmp(command + 8, "off")) {
      policy.channel[0] = 0;
      policy.channelKeySet = false;
      explicit_bzero(policy.channelKey, sizeof(policy.channelKey));
      std::strcpy(reply, onchip::saveBotRadioPolicy(policy) ?
          "Saved bot channel/key; reboot required" :
          "Error: channel persistence failed; saved state may be uncertain");
    } else {
      const char *encoded = command + 8;
      const char *space = std::strchr(encoded, ' ');
      const size_t name_bytes = space ? size_t(space - encoded) / 2 : 0;
      if (!space || size_t(space - encoded) != name_bytes * 2 ||
          name_bytes < 1 || name_bytes > 31 || std::strlen(space + 1) != 32 ||
          !decode_hex(encoded, name_bytes, reinterpret_cast<uint8_t *>(policy.channel)) ||
          !decode_hex(space + 1, 16, policy.channelKey)) {
        std::strcpy(reply, "Error: channel requires NAMEHEX KEY32; name 1..31 bytes, key exactly 16 bytes");
      } else {
        policy.channel[name_bytes] = 0;
        policy.channelKeySet = true;
        bool printable = true;
        for (size_t i = 0; i < name_bytes; ++i)
          printable = printable && policy.channel[i] >= 32 && policy.channel[i] <= 126;
        std::strcpy(reply, printable && onchip::saveBotRadioPolicy(policy) ?
            "Saved bot channel/key; reboot required" :
            "Error: invalid channel or channel persistence failed; saved state may be uncertain");
      }
      explicit_bzero(policy.channelKey, sizeof(policy.channelKey));
    }
  } else if (!std::strcmp(command, "path status")) {
    onchip::BotRadioPolicy policy;
    if (!onchip::loadBotRadioPolicy(policy)) std::strcpy(reply, "Error: bot path policy unavailable");
    else std::snprintf(reply, sizeof(reply), "path_hash_mode=%u width=%u", unsigned(policy.pathWidth - 1), unsigned(policy.pathWidth));
  } else if (!std::strncmp(command, "path ", 5)) {
    onchip::BotRadioPolicy policy;
    if (command[5] < '0' || command[5] > '2' || command[6] ||
        !onchip::loadBotRadioPolicy(policy)) {
      std::strcpy(reply, "Error: path mode must be 0, 1 or 2 and policy must be readable");
    } else {
      policy.pathWidth = uint8_t(command[5] - '0' + 1);
      std::strcpy(reply, onchip::saveBotRadioPolicy(policy) ?
          "Saved bot path mode; reboot required" :
          "Error: path persistence failed; saved state may be uncertain");
    }
  } else if (!std::strcmp(command, "airtime status")) {
    onchip::BotRadioPolicy policy;
    if (!onchip::loadBotRadioPolicy(policy)) std::strcpy(reply, "Error: saved bot airtime policy unavailable");
    else std::snprintf(reply, sizeof(reply), "Bot airtime saved=%u ms/min; reboot applies", policy.airtimeMs);
  } else if (!std::strncmp(command, "airtime ", 8)) {
    const char *number = command + 8;
    const unsigned long value = std::strtoul(number, nullptr, 10);
    if (!*number || std::strspn(number, "0123456789") != std::strlen(number) ||
        value < 360 || value > 3600) {
      std::strcpy(reply, "Error: bot airtime requires 360..3600 ms/min");
    } else {
      onchip::BotRadioPolicy policy;
      if (!onchip::loadBotRadioPolicy(policy)) {
        std::strcpy(reply, "Error: saved bot airtime policy unavailable");
      } else {
        policy.airtimeMs = uint16_t(value);
        std::strcpy(reply, onchip::saveBotRadioPolicy(policy) ?
            "Saved bot airtime; reboot required" :
            "Error: airtime persistence failed; saved state may be uncertain");
      }
    }
  } else if (!std::strncmp(command, "identity", 8) ||
             !std::strncmp(command, "role key", 8) ||
             !std::strncmp(command, "key", 3)) {
    std::strcpy(reply, "Error: use the Go owner socket for key bot [pending|cancel|apply]");
  } else {
    std::strcpy(reply, "Error: unsupported native admin command; use help, help grants, source help or data help");
  }
  uint8_t response[4 + 256]{};
  std::memcpy(response, frame + 1, 4);
  const size_t reply_size = std::strlen(reply);
  if (reply_size > 256) return false;
  std::memcpy(response + 4, reply, reply_size);
  return send(0x85, response, 4 + reply_size);
}

bool initialize_bot_path() {
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    onchip::BotRadioPolicy policy;
    policy.pathWidth = 3;
    return onchip::saveBotRadioPolicy(policy);
  }
  if (result != ESP_OK) return false;
  size_t size = 0;
  result = nvs_get_blob(handle, "bot-radio", nullptr, &size);
  nvs_close(handle);
  if (result == ESP_OK) {
    onchip::BotRadioPolicy policy;
    return onchip::loadBotRadioPolicy(policy);
  }
  if (result != ESP_ERR_NVS_NOT_FOUND) return false;
  onchip::BotRadioPolicy policy;
  policy.pathWidth = 3;
  return onchip::saveBotRadioPolicy(policy);
}
void owner_send_status(const uint8_t *id, uint8_t state, uint8_t flags, uint8_t reason) {
  uint8_t body[19];
  memcpy(body, id, 16); body[16] = state; body[17] = flags; body[18] = reason;
  send(0x88, body, sizeof(body));
}
void owner_inbox(const uint8_t *sender, uint32_t timestamp, const uint8_t *text, size_t size) {
  if (!sender) { send(0x89, nullptr, 0); return; }
  uint8_t body[36 + onchip::BotReplyLimit];
  memcpy(body, sender, 32); queued_tx::put32(body + 32, timestamp);
  memcpy(body + 36, text, size);
  send(0x89, body, 36 + size);
}
bool process(onchip::CommandBot &bot, onchip::MastSource &source,
             const uint8_t *frame, size_t length) {
  if (frame[0] == 0x08 && length == 2 && frame[1] <= 1) {
    bot.setOwnerSinks(frame[1] ? owner_send_status : nullptr, frame[1] ? owner_inbox : nullptr);
    return true;
  }
  if (frame[0] == 0x07 && length >= 50 && length <= 49 + onchip::BotReplyLimit) {
    bot.ownerSend(frame + 1, frame + 17, frame + 49, length - 49);
    return true;
  }
  if (frame[0] == 0x04) return admin(bot, source, frame, length);
  if (length == 43 && frame[0] == 0x05 && frame[42] <= 1) {
    mesh::QueuedRadioStats stats{};
    stats.generation = queued_tx::get32(frame + 1);
    stats.configuration_generation = queued_tx::get32(frame + 5);
    stats.aggregate_credit_ms = queued_tx::get32(frame + 9);
    stats.source_credit_ms = queued_tx::get32(frame + 13);
    stats.aggregate_rf_ms = queued_tx::get32(frame + 17);
    stats.source_rf_ms = queued_tx::get32(frame + 21);
    stats.aggregate_successes = queued_tx::get32(frame + 25);
    stats.aggregate_failures = queued_tx::get32(frame + 29);
    stats.source_successes = queued_tx::get32(frame + 33);
    stats.source_failures = queued_tx::get32(frame + 37);
    stats.aggregate_queued = frame[41];
    stats.aggregate_transmitting = frame[42] != 0;
    if (!stats.generation || !stats.configuration_generation) return false;
    bot.hostStatistics(stats);
    return true;
  }
  if (length >= 11 && frame[0] == 0x02 && length <= 10 + 255) {
    const float rssi = queued_tx::getFloat(frame + 1), snr = queued_tx::getFloat(frame + 5);
    const bool local = frame[9] == 1;
    if (frame[9] > 2 ||
          (frame[9] == 0 && (!std::isfinite(rssi) || rssi < -150 || rssi > 0 ||
                    !std::isfinite(snr) || snr < -32 || snr > 32)))
      return false;
    if (!source.ready()) return true;
    // A full RX ring drops a packet, not the host RF connection.
    bot.hostReceive(frame + 10, length - 10, frame[9] == 2 ? NAN : rssi,
                    frame[9] == 2 ? NAN : snr, local);
    return true;
  }
  if (length == 20 && frame[0] == 0x03 && frame[19] <= 1)
    return bot.hostTransmitResult(queued_tx::get32(frame + 1), frame[5], frame[6],
                                  queued_tx::get32(frame + 7), queued_tx::get32(frame + 11),
                                  queued_tx::get32(frame + 15), frame[19] != 0);
  return false;
}
class Input {
  std::array<uint8_t, MaxFrame + 4> bytes_{};
  size_t used_ = 0, need_ = 4;
  Clock::time_point started_{};
public:
  // 1 = complete frame, 0 = partial, -1 = EOF, -2 = invalid/fault.
  int next(uint8_t *&frame, size_t &size) {
    for (;;) {
      if (used_ == need_) {
        if (need_ == 4) {
          const auto length = queued_tx::get32(bytes_.data());
          if (length < 1 || length > MaxFrame) return -2;
          need_ = 4 + length;
        } else {
          frame = bytes_.data() + 4;
          size = need_ - 4;
          used_ = 0;
          need_ = 4;
          return 1;
        }
      }
      if (used_ && Clock::now() - started_ > std::chrono::seconds(5)) return -2;
      const ssize_t count = ::read(STDIN_FILENO, bytes_.data() + used_, need_ - used_);
      if (count > 0) {
        if (!used_) started_ = Clock::now();
        used_ += size_t(count);
        continue;
      }
      if (count == 0) return used_ ? -2 : -1;
      return errno == EAGAIN || errno == EINTR ? 0 : -2;
    }
  }
};
bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
} // namespace

unsigned long millis() {
  static const auto start = Clock::now();
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}
void delay(unsigned long ms) { usleep(ms * 1000); }
void randomSeed(long) {}
long random(long low, long high) {
  if (high <= low) return low;
  const uint64_t span = uint64_t(high) - uint64_t(low);
  uint64_t value = 0;
  const uint64_t limit = UINT64_MAX - UINT64_MAX % span;
  do { secure_random(reinterpret_cast<uint8_t *>(&value), sizeof(value)); } while (value >= limit);
  return low + long(value % span);
}
void esp_fill_random(void *data, size_t size) { secure_random(static_cast<uint8_t *>(data), size); }

namespace onchip {
CommandBot &commandBotService() {
  static CommandBot bot;
  return bot;
}
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  nvs_handle_t handle;
  auto result = nvs_open("mc-mast-admin", write ? NVS_READWRITE : NVS_READONLY, &handle);
  present = false;
  if (!write && result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t length = size;
  if (write) {
    result = nvs_set_blob(handle, key, data, size);
    if (result == ESP_OK) result = nvs_commit(handle);
  } else {
    result = nvs_get_blob(handle, key, data, &length);
  }
  nvs_close(handle);
  if (!write && result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || length != size) return false;
  present = true;
  return true;
}
bool nativebot_send_tx(uint32_t token, uint8_t priority, uint32_t delay,
                       uint32_t expiry, const uint8_t *packet, size_t length) {
  if (!length || length > 255 || priority > 7 ||
      delay > queued_tx::MAX_DELAY_MS || expiry > queued_tx::MAX_DELAY_MS)
    return false;
  uint8_t body[13 + 255]{};
  queued_tx::put32(body, token);
  body[4] = priority;
  queued_tx::put32(body + 5, delay);
  queued_tx::put32(body + 9, expiry);
  std::memcpy(body + 13, packet, length);
  return send(0x81, body, 13 + length);
}
} // namespace onchip

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if ((argc != 2 && !(argc == 3 && !std::strcmp(argv[2], "--dormant"))) ||
      !set_nonblocking(STDIN_FILENO) || !set_nonblocking(STDOUT_FILENO)) {
    std::fputs("Host bot: usage worker ABSOLUTE_PRIVATE_STATE_DIRECTORY [--dormant]\n", stderr);
    return 2;
  }
  struct stat state{};
  if (lstat(argv[1], &state) || !S_ISDIR(state.st_mode) ||
      (state.st_mode & 07777) != 0700 || state.st_uid != geteuid()) {
    std::fputs("Host bot: state directory must be private and pre-existing\n", stderr);
    return 2;
  }
  const std::string nvs_path = std::string(argv[1]) + "/nvs";
  if (!load_scopes(argv[1])) return fail(2, "invalid private scopes file; expected SCP1 home/default keys, mode 0600");
  const std::string spiffs_path = std::string(argv[1]) + "/spiffs";
  Input input;
  uint8_t *frame = nullptr;
  size_t size = 0;
  uint8_t identity[64]{};
  RadioConfig profile{};
  int noise = 0;
  uint32_t airtime[256]{};
  bool started = false, ready_sent = false;
  bool dormant = argc == 3;
  auto &bot = onchip::commandBotService();
  struct StopBot {
    onchip::CommandBot &bot;
    ~StopBot() { bot.stop(); }
  } stopBot{bot};
  onchip::MastSource source;
  auto &wake = onchip::BotWake::shared();
  auto last_status = Clock::now();
  auto ready_deadline = Clock::time_point::max();
  for (;;) {
    wake.drain();
    onchip::NativeClockSample clock;
    if (onchip::nativeBotClockSample(clock))
      onchip::publishHostClock(clock.earliestUtcMs, clock.latestUtcMs, clock.sampledMonotonicNs, clock.epoch);
    else onchip::publishHostClock(0, 0, 0, 0);
    int received = 0;
    for (unsigned n = 0; n < 64 && (received = input.next(frame, size)) == 1; ++n) {
      if (!started) {
        if (!parse_hello(frame, size, identity, profile, noise, airtime)) {
          return fail(1, "invalid HELLO");
        }
        if (native_nvs_init(nvs_path.c_str()) != ESP_OK) {
          return fail(2, "NVS startup failed");
        }
        if (!native_spiffs_init(spiffs_path.c_str())) {
          return fail(2, "SPIFFS startup failed");
        }
        if (native_nvs_bind_bot_identity(identity, sizeof(identity)) != ESP_OK) {
          return fail(3, "HELLO identity invalid or native identity binding unavailable");
        }
        explicit_bzero(identity, sizeof(identity));
        explicit_bzero(frame, size);
        if (!apply_host_name(argv[1])) {
          return fail(2, "invalid host-name override or name commit failed; expected private HNM1 name");
        }
        if (!initialize_bot_path()) {
          return fail(2, "bot path preference unavailable");
        }
        if (!bot.beginHost(profile, airtime, noise)) {
          return fail(4, "CommandBot startup failed");
        }
        source.begin();
        uint8_t management[33]{};
        std::memcpy(management, bot.publicKey(), 32);
        management[32] = 1;
        if (!send(0x86, management, sizeof(management))) {
          return fail(6, "private management channel unavailable");
        }
        started = true;
        ready_deadline = Clock::now() + std::chrono::seconds(30);
      } else if (size == 1 && frame[0] == 0x06) {
        if (!dormant || !ready_sent || !source.ready()) return fail(5, "ACTIVATE requires dormant READY bot");
        dormant = false;
        bot.setCommandAdmission(true);
        const uint8_t queued = onchip::HostBotOwner::advertise(bot) ==
            onchip::HostBotOwner::AdvertResult::Queued ? 1 : 0;
        if (!send(0x87, &queued, 1)) return fail(6, "ACTIVATE reply unavailable");
      } else if (dormant && frame[0] == 0x02) {
        return fail(5, "dormant bot cannot receive RF");
      } else if (!process(bot, source, frame, size)) {
        return fail(5, "invalid RX or TX result frame");
      } else if (frame[0] == 0x04) {
        explicit_bzero(frame, size);
      }
    }
    nvs_stats_t storage{};
    if (received == -2) return fail(5, "invalid or incomplete input frame");
    if (started && (native_spiffs_faulted() || nvs_get_stats(nullptr, &storage) != ESP_OK))
      return fail(6, "storage failed");
    if (output_fault) return fail(6, "output pipe failed");
    if (received == -1) return started ? 0 : 1;
    if (started) {
      if (dormant) bot.setCommandAdmission(false);
      bot.loop();
      source.loop();
      if (dormant) bot.setCommandAdmission(false);
      if (!ready_sent && !source.recoveryRequired() && Clock::now() > ready_deadline) {
        return fail(7, "durable CommandBot source did not become ready");
      }
      if (!ready_sent && source.ready()) {
        if (!dormant) {
          bot.setCommandAdmission(true);
          bot.advertise();
        }
        uint8_t body[33]{};
        std::memcpy(body, bot.publicKey(), 32);
        body[32] = 1;
        if (!send(0x82, body, sizeof(body))) return 1;
        ready_sent = true;
      }
      if (Clock::now() - last_status >= std::chrono::seconds(1)) {
        const auto &c = bot.counters();
        uint8_t body[30]{};
        body[0] = source.ready() ? 1 : 0;
        onchip::BotNodeSnapshot node;
        bot.nodeSnapshot(node);
        body[1] = node.fault || source.recoveryRequired() ? 1 : 0;
        const uint32_t values[7] = {c.observationsDropped, c.malformed, c.duplicates,
                                    c.rejected, c.vmFailures, c.replies, c.traces};
        for (unsigned n = 0; n < 7; ++n) queued_tx::put32(body + 2 + n * 4, values[n]);
        if (!send(0x83, body, sizeof(body))) return 1;
        last_status = Clock::now();
      }
    }
    uint32_t wait = received == 1 ? 0 : UINT32_MAX;
    if (started) {
      wait = std::min(wait, std::min(bot.hostWaitMs(), source.hostWaitMs()));
      const auto now = Clock::now();
      const auto deadline = !ready_sent && !source.recoveryRequired() ?
          std::min(last_status + std::chrono::seconds(1), ready_deadline) :
          last_status + std::chrono::seconds(1);
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      wait = std::min(wait, uint32_t(std::max<int64_t>(0, left)));
    }
    pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wake.descriptor(), POLLIN, 0}};
    const int timeout = wait == UINT32_MAX ? -1 : int(std::min(wait, uint32_t(INT32_MAX)));
    if (poll(fds, 2, timeout) < 0 && errno != EINTR) return fail(6, "input or worker event wait failed");
  }
}
