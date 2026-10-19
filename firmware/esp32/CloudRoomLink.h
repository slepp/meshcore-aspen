// SPDX-License-Identifier: Apache-2.0
#pragma once
// Shared-room lifecycle verbs of the Management console: join, create, convert, invite,
// nodes, revoke, password, unlink, share, list, probe and result. Network jobs run on the
// shared native HTTPS task; record changes and restarts run on the dispatch task.
#include "CloudRoomNode.h"
#include <stddef.h>
#include <stdint.h>

#ifndef ONCHIP_CLOUD_ROOM_DEFAULT_HOST
#define ONCHIP_CLOUD_ROOM_DEFAULT_HOST "rooms.ve6slp.ca"
#endif
// Where an operator asks for a code on the default host (a short, human-typable pointer).
#ifndef ONCHIP_CLOUD_ROOM_REQUEST_POINTER
#define ONCHIP_CLOUD_ROOM_REQUEST_POINTER "github.com/slepp/meshcore-aspen"
#endif
#ifndef ONCHIP_CLOUD_ROOM_CONNECTIONS
#define ONCHIP_CLOUD_ROOM_CONNECTIONS 1
#endif

namespace onchip {
// Free internal heap and largest free block a link job needs before it opens TLS, on top of
// the 32 KiB radio reserve the TLS layer keeps. Provisional: set from the lab minimum-free
// log of a handshake (README of the 0.1.20 release checks).
constexpr size_t CloudLinkMinFreeInternal = 48 * 1024;
constexpr size_t CloudLinkMinLargestBlock = 16 * 1024;
constexpr uint32_t CloudLinkTimeoutMs = 20000;
constexpr unsigned CloudLinkFailureLimit = 5;
constexpr uint32_t CloudLinkRestartMs = 10000;
constexpr uint32_t CloudLinkSecretMs = 5 * 60 * 1000;

enum class CloudAliasState : uint8_t { Live, Down, Off, Revoked };
struct CloudRoomLinkPorts {
  CloudTransport *(*transport)() = nullptr;                  // one stream per job; the engine deletes it
  bool (*resolve)(const char *host, char address[16]) = nullptr;
  bool (*now)(uint32_t &utc) = nullptr;                      // trusted UTC seconds
  size_t (*freeInternal)() = nullptr, (*largestBlock)() = nullptr;
  uint32_t (*millis)() = nullptr;
  CloudAliasState (*aliasState)(unsigned runningAlias) = nullptr;
  bool (*wake)() = nullptr;                                  // a job was queued; false when it cannot run
  // Room role seams (Runtime.h): dispatch task only.
  bool (*roomName)(char name[32]) = nullptr;                 // false when the Room role is not running
  bool (*roomGuestPassword)(char out[16]) = nullptr;
  bool (*roomSetGuestPassword)(const char *password) = nullptr;
};
void cloudRoomLinkSetPorts(const CloudRoomLinkPorts &ports);
// A `cloudroom ...` request (text after "cloudroom"). False when the verb is not a lifecycle
// verb, so the existing handlers run.
bool cloudRoomLinkCommand(const char *line, char *reply, size_t capacity);
// A network job is queued or running: the service closes its alias sockets first.
bool cloudRoomLinkPending();
// A job or a restart request is waiting for the network task.
bool cloudRoomLinkWork();
// Network task: schedules a requested restart and runs a queued job. Scheduling waits for
// the dispatch task to flush state, so this never runs on the dispatch task.
void cloudRoomLinkRun();
// Dispatch task: applies a finished job (save, flush, restart), clears one-shot flags.
void cloudRoomLinkLoop();
// An alias sent its one-time advert; the dispatch task clears the saved flag.
void cloudRoomLinkAdvertised(unsigned runningAlias);
// " link=idle room=local node=ab12cd34" appended to `cloudroom status`.
void cloudRoomLinkStatus(char *out, size_t capacity);
// Default and test hooks.
unsigned cloudRoomLinkFailures();
void cloudRoomLinkResetForTest();
} // namespace onchip
