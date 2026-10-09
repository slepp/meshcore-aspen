// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Lifecycle.h"
#include "Clock.h"
#include "LocalRadio.h"
#include "PacketServices.h"
#include "Syslog.h"
#include <FS.h>
#include <helpers/ArduinoHelpers.h>

namespace onchip {
class CompanionSessions;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
class CommandBot;
CommandBot &commandBotService();
#ifdef BOT_HOST_RUNNER
bool bindCommandBotForReplay(CommandBot *bot);
#endif
#endif
CompanionSessions &companionSessions();
template <class Prefs>
void reflectSharedConfiguration(Prefs &prefs, const LocalRadio &radio) {
  const auto config = radio.configuration();
  prefs.freq = config.freq_hz / 1000000.0;
  prefs.bw = config.bw_hz / 1000.0;
  prefs.sf = config.sf;
  prefs.cr = config.cr;
  prefs.tx_power_dbm = config.tx_power;
}
bool begin(WifiKissMultiplexer &mux, const mesh::Identity &bot_identity);
#ifdef COMPANION_SESSIONS_HOST
void stopManagementForTest();
#endif
void dashboardStatus(RadioDashboard::RadioStatus &status, bool kiss_listening);
void companionDashboardContacts(RadioDashboard::RadioStatus &status);
bool companionContactAdvert(const uint8_t *hash, unsigned &cursor,
                            uint8_t *key, uint8_t *advert, uint8_t &size);
bool localTransmitSource(uint8_t slot, uint32_t generation,
                         RadioDashboard::RoleStatus &status);
void loop();
bool packetSystemSnapshot(packet_engine::SystemInfo &);
packet_engine::Fault packetComposeOwned(const packet_engine::ComposeRequest &,
    const uint8_t *, uint16_t, uint8_t *, uint16_t &);
packet_engine::Fault nativeRoleComposePacket(Role, const packet_engine::ComposeRequest &,
    const uint8_t *, uint16_t, uint8_t *, uint16_t &);
// Only fixed status text, measurements and sanitized audit metadata; never arguments or secrets.
bool diagnosticEvent(const char *message, DiagnosticSubsystem subsystem = DiagnosticSubsystem::System);
bool beginDiagnostics();
bool diagnosticsCommand(const char *command, char *reply, size_t capacity);
void diagnosticLoopSample(uint32_t started, uint32_t finished);
void observerStatistics(char *reply, size_t capacity);
bool observerToken(const char *audience, char *output, size_t capacity, char *error, size_t errorCapacity);
bool sharedRadioReadCommand(const LocalRadio &radio, const char *command, char *reply, size_t capacity);
bool repeaterBegin(WifiKissMultiplexer &mux);
bool roomBegin(WifiKissMultiplexer &mux);
bool companionBegin(WifiKissMultiplexer &mux);
void repeaterLoop();
void roomLoop();
void companionLoop();
void repeaterStop();
void roomStop();
void companionStop();
bool repeaterFlush();
bool roomFlush();
bool companionFlush();
uint8_t repeaterPathWidth();
uint8_t roomPathWidth();
uint8_t companionPathWidth();
uint8_t managementPathWidth();
int roomGuestAccess();
bool repeaterSetPathWidth(uint8_t width);
bool roomSetPathWidth(uint8_t width);
bool companionSetPathWidth(uint8_t width);
bool setNativeRolePathWidth(uint8_t width);
bool nativeRoleName(Role role, char name[32]);
bool setNativeRoleName(Role role, const char *name);
enum class RolePasswordUpdate { SavedApplied, Unavailable, Uncertain };
RolePasswordUpdate setNativeRolePassword(Role role, const char *password);
bool nativeRoleAdvertise(Role role, bool zeroHop);
bool kissName(char name[32]);
bool setKissName(const char *name);
unsigned companionChannelCount();
bool companionChannelInfo(unsigned index, char name[32], uint8_t fingerprint[8]);
bool companionSetChannel(unsigned index, const char *name, const uint8_t key[16]);
bool repeaterEraseStep(bool &done);
bool roomEraseStep(bool &done);
bool companionEraseStep(bool &done);
void companionRequestFailed();
mesh::PacketManager &repeaterPackets();
mesh::PacketManager &roomPackets();
mesh::PacketManager &companionPackets();
mesh::MillisecondClock &companionMillis();
} // namespace onchip
