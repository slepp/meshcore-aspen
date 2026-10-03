// SPDX-License-Identifier: Apache-2.0
#pragma once
#if __has_include("WasmConfig.h")
#include "WasmConfig.h"
#endif
#ifndef ONCHIP_BOT_WASM
#define ONCHIP_BOT_WASM 0
#endif
#ifndef ONCHIP_BOT_COMPACT_PROFILE
#ifdef NRF52_PLATFORM
#define ONCHIP_BOT_COMPACT_PROFILE 1
#else
#define ONCHIP_BOT_COMPACT_PROFILE 0
#endif
#endif
#ifndef ONCHIP_BOT_SINGLE_SESSION
#define ONCHIP_BOT_SINGLE_SESSION ONCHIP_BOT_COMPACT_PROFILE
#endif
#ifndef ONCHIP_BOT_SEPARATE_DIAGNOSTICS
#if ONCHIP_BOT_COMPACT_PROFILE
#define ONCHIP_BOT_SEPARATE_DIAGNOSTICS 0
#else
#define ONCHIP_BOT_SEPARATE_DIAGNOSTICS 1
#endif
#endif
#if ONCHIP_BOT_WASM && !ONCHIP_BOT_SEPARATE_DIAGNOSTICS
#error "Mixed runtimes require an independent diagnostics session"
#endif
#include <MeshCore.h>
#include <stddef.h>
#include <stdint.h>

namespace onchip {
constexpr size_t BotSourceLimit = 4096;
// Pinned Mesh::createDatagram admission, minus timestamp and text flags.
constexpr size_t BotTextLimit =
    MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1) - 5;
constexpr size_t BotReplyLimit = BotTextLimit;
constexpr size_t BotNameLimit = 24;
constexpr size_t BotNetworkPayloadLimit = 1024;
constexpr size_t BotNetworkResponseLimit = 2048;
constexpr size_t BotArgumentsLimit = BotTextLimit - 3; // "!x " prefix.
constexpr size_t BotPathLimit = MAX_PATH_SIZE;
constexpr size_t BotTraceLimit = MAX_PACKET_PAYLOAD - 9;
constexpr size_t BotTraceHopLimit = MAX_PATH_SIZE - 1;
constexpr unsigned BotObservationLimit = 8;
#ifndef ONCHIP_BOT_JOB_LIMIT
#define ONCHIP_BOT_JOB_LIMIT 4
#endif
constexpr unsigned BotJobLimit = ONCHIP_BOT_JOB_LIMIT, BotIoLimit = 8;
static_assert(BotJobLimit >= 1 && BotJobLimit <= 4, "Bot worker job capacity is 1..4");
constexpr size_t BotKeyLimit = 32;
#if ONCHIP_BOT_COMPACT_PROFILE
constexpr size_t BotValueLimit = 128;
#else
constexpr size_t BotValueLimit = 256;
#endif
constexpr unsigned BotKeysPerScope = 8;
constexpr size_t BotIoTextLimit = BotValueLimit > BotTextLimit ? BotValueLimit : BotTextLimit;
#if ONCHIP_BOT_COMPACT_PROFILE
constexpr unsigned BotTransactionLimit = 2;
#else
constexpr unsigned BotTransactionLimit = 4;
#endif
constexpr unsigned BotReceiveLimit = 4, BotReceiveDedupLimit = 16;

struct BotPath {
  uint8_t width = 1, count = 0;
  uint8_t bytes[BotPathLimit]{};
  bool known = false;
  size_t size() const { return size_t(width) * count; }
  bool valid() const {
    return known && width >= 1 && width <= 3 && count <= 63 &&
           size() <= BotPathLimit;
  }
};

struct BotNodeSnapshot {
  bool available = false, hasIdentity = false, enabled = false, ready = false, fault = false;
  bool rolesKnown = false, wifiKnown = false, wifiConnected = false;
  uint8_t selectedRoles = 0, readyRoles = 0, publicKey[32]{};
  uint64_t uptimeMs = 0;
  char nativeRevision[13]{}, build[24]{}, name[33]{};
  uint32_t sourceGeneration = 0;
};
struct BotAirSnapshot {
  bool available = false, transmitting = false;
  uint16_t queued = 0;
  uint8_t aggregateQueued = 0;
  uint32_t generation = 0, configurationGeneration = 0, capturedMs = 0;
  uint32_t creditMs = 0, aggregateCreditMs = 0, rfMs = 0, aggregateRfMs = 0;
  uint32_t successes = 0, failures = 0, aggregateSuccesses = 0, aggregateFailures = 0;
  uint32_t reservedMs = 0, limitMs = 0, remainingMs = 0;
};

struct BotEvent {
  enum Kind : uint8_t { Command, Startup, Connectivity, Message, NodeStatus } kind = Command;
  uint32_t eventEpoch = 0;
  char message[BotTextLimit + 1]{};
  char name[BotNameLimit + 1]{}, arguments[BotArgumentsLimit + 1]{};
  char channel[33]{}, nickname[33]{};
  uint16_t replyLimit = BotReplyLimit;
  uint8_t sender[32]{}, request[16]{}, channelId[32]{};
  uint32_t timestamp = 0;
  BotPath path;
  uint8_t route[BotTraceLimit]{}, routeSize = 0, routeWidth = 1;
  bool routeExplicit = false, local = false, signal = false;
  float rssi = 0, snr = 0;
  uint32_t frequency = 0, bandwidth = 0;
  uint8_t sf = 0, cr = 0, routeType = 0;
  uint32_t windowMs = 0;
  BotPath observations[BotObservationLimit]{};
  uint8_t observationCount = 0;
  bool truncated = false;
  bool authenticated = false, channelVerified = false, sharedState = false, homeAccess = false,
       forwardAccess = false, reminderAccess = false, owner = false, targeted = false, channelWait = false;
  uint8_t destinations[4][32]{};
  uint32_t meshGrant = 0;
  uint32_t homeGrant = 0, sharedGrant = 0, forwardGrant = 0, reminderGrant = 0;
  uint32_t networkEpoch = 0;
  BotNodeSnapshot node;
  BotAirSnapshot air;
};

struct BotIoToken {
  uint32_t generation = 0, job = 0, operation = 0;
  bool operator==(const BotIoToken &other) const {
    return generation == other.generation && job == other.job && operation == other.operation;
  }
};
struct BotMutation {
  char key[BotKeyLimit + 1]{}, expected[BotValueLimit + 1]{}, value[BotValueLimit + 1]{};
  bool compare = false, present = false, remove = false;
};
struct BotIoRequest {
  enum Kind : uint8_t { Sleep, Get, Put, Delete, Send, Wait, Trace, Advert, Rpc,
                        TimerSet, TimerGet, TimerCancel, TimerWait, Forward,
                        ReminderSet, ReminderList, ReminderCancel, List, Cas, Transaction,
                        Inspect, Admin, HttpGet, HttpPost, PackageGet, Utility } kind = Sleep;
  enum Scope : uint8_t { Caller, Conversation, Bot, Channel } scope = Caller;
  BotIoToken token{};
  uint8_t principal[32]{};
  uint32_t delayMs = 0;
  uint32_t deadline = 0, grant = 0, revision = 0, delaySeconds = 0;
  enum PacketKind : uint8_t { TextPacket, ChannelPacket, TracePacket } packetKind = TextPacket;
  enum WaitKind : uint8_t { TextWait, AckWait, TraceWait, ChannelWait } waitKind = TextWait;
  bool waitAck = false, reply = false, exact = false, traceSendOnly = false;
  uint8_t pathWidth = 0, routeType = 0;
  uint32_t packetId = 0;
  uint8_t route[BotTraceLimit]{}, routeSize = 0, routeWidth = 1;
  char key[BotKeyLimit + 1]{}, value[BotIoTextLimit + 1]{};
  // An empty endpoint selects home only for legacy health/echo/weather RPC.
  char endpoint[BotNameLimit + 1]{};
#if ONCHIP_BOT_COMPACT_PROFILE
  char json[1]{};
#else
  char json[BotNetworkPayloadLimit + 1]{};
#endif
  uint32_t networkEpoch = 0;
  uint8_t mutations = 0;
  uint32_t eventEpoch = 0;
  BotMutation mutation[BotTransactionLimit]{};
  bool sharedScope() const { return scope == Bot || scope == Channel; }
  bool durableTimer() const { return kind >= TimerSet && kind <= TimerWait; }
  bool reminder() const { return kind >= ReminderSet && kind <= ReminderCancel; }
};
struct BotPacketInfo {
  uint32_t id = 0, timestamp = 0;
  BotPath path{};
  uint8_t routeType = 0;
  bool authenticated = false, forwardable = false, signal = false;
  float rssi = 0, snr = 0;
  uint8_t sender[32]{};
  char nickname[33]{};
  bool channel = false;
};
struct BotTraceInfo {
  uint8_t width = 0, count = 0;
  int8_t snr[BotTraceHopLimit]{};
};
struct BotKeyList {
  uint8_t count = 0;
  char keys[BotKeysPerScope][BotKeyLimit + 1]{};
};
enum class BotTimerState : uint8_t { Missing, Pending, Claimed, Cancelled, Overdue };
enum class BotReminderState : uint8_t { Missing, Pending, Unknown, Sent, Cancelled, Overdue };
const char *botReminderStateName(BotReminderState state);
const char *botTimerStateName(BotTimerState state);
struct BotIoResult {
  BotIoToken token{};
  bool ok = false, found = false;
  enum Outcome : uint8_t { Rejected, Committed, Conflict, Unknown } outcome = Rejected;
  bool queued = false, transmitted = false, acknowledged = false, truncated = false;
  uint32_t radioJob = 0;
  char value[BotIoTextLimit + 1]{}, error[128]{};
#if ONCHIP_BOT_COMPACT_PROFILE
  char json[1]{};
#else
  char json[BotNetworkResponseLimit + 1]{};
#endif
  uint16_t httpStatus = 0;
  bool networkSubmitted = false;
  char rpcCode[40]{}, country[81]{}, source[17]{}, observedAt[25]{};
  double temperatureC = 0;
  uint32_t sourceAgeSeconds = 0;
  uint8_t weatherCode = 0;
  BotTimerState timerState = BotTimerState::Missing;
  BotReminderState reminderState = BotReminderState::Missing;
  uint32_t deadlineUtc = 0, revision = 0;
  bool pending = false, replaced = false, timeTrusted = false;
  BotPacketInfo packet{};
  BotTraceInfo trace{};
  BotKeyList keys{};
};
void resetBotIoResult(BotIoResult &result);

struct BotAction {
  enum Kind : uint8_t { None, Reply, Trace } kind = None;
  char text[BotReplyLimit + 1]{};
};

struct BotVmStats {
  size_t peakBytes = 0;
  uint32_t wasmLinearBytes = 0, wasmPoolBytes = 0, wasmPoolHighWaterBytes = 0,
           wasmSessionBytes = 0;
  uint32_t instructions = 0, parserSteps = 0;
  uint64_t elapsedUs = 0;
  uint64_t loadUs = 0, initUs = 0, invokeUs = 0, cleanupUs = 0;
  uint32_t freeInternalBytes = 0, freePsramBytes = 0, stackHighWaterBytes = 0;
};

struct BotVmLimits {
#if ONCHIP_BOT_COMPACT_PROFILE
  size_t heapBytes = 48 * 1024;
#else
  size_t heapBytes = 96 * 1024;
#endif
  uint32_t instructions = 10000, parserSteps = 16384;
  uint64_t loadWallUs = 330000;
  uint64_t wallUs = 20000;
  uint64_t initWallUs = 50000;
};

constexpr uint64_t BotBundledLoadWallUs = 1000000;

// No MeshCore objects, secrets or borrowed packet buffers cross this boundary.
bool parseBotCommand(const char *text, size_t size, BotEvent &event,
                     char *error, size_t errorSize);
bool formatBotPath(const BotPath &path, char *output, size_t capacity);
bool formatBotDiagnostic(const BotEvent &event, const char *name, unsigned page,
                         const char *luaVersion, char *output, size_t capacity);
extern const char BotDefaultSource[];
extern const char BotNetworkSource[];
extern const char BotBoardSource[];
extern const char BotDiagnosticSource[];
} // namespace onchip
