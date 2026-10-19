// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

// Grammar of the shared-room lifecycle verbs sent to the Management console.
// Portable: no Arduino headers, no allocation, no I/O. Every field is copied
// into the caller's Command; the caller wipes the Command (it can hold a code
// or a room password) on every exit path.
namespace cloudroom {

enum class Verb : uint8_t { NotNew, List, Probe, Join, Create, Convert, Invite, Nodes, Revoke,
                            Password, Unlink, Share, Result, Apply };
enum class Parse : uint8_t { Ok, NotCloudroom, Usage, BadCode, BadHost, BadName, TooLong };

// A whole "cloudroom ..." request, prefix included, stays within one tagged reply.
constexpr size_t RequestLimit = 145;
constexpr size_t HostLimit = 36;   // longest HOST accepted in a DM
constexpr size_t NameLimit = 31;
constexpr size_t PasswordLimit = 15;

struct Command {
  Verb verb;
  char code[17];              // canonical 16 symbols, or "OPEN" (create against an open server)
  char host[64];              // lowercase; empty = default host (also the host for probe)
  char name[32];              // create: rest of the line
  char alias[65];             // unlink: alias id or 1-based index, as typed; empty = the only alias
  char arg[17];               // list/nodes: decimal page or slot; revoke: 8..16 lowercase hex id;
                              // password: the new password when neither open nor generate
  char yesHost[37];           // convert: host retyped after "yes"
  bool yes, owner, remove /*delete*/, local, force, on, open, generate;
};

// `line` is the text after the word "cloudroom"; a leading "cloudroom " is tolerated.
// Returns Ok with Verb::NotNew for status|error|advertise|config|enable and for anything
// else not listed, so the caller falls through to its existing handlers. On every
// result other than NotCloudroom, `out.verb` names the verb that was recognised.
Parse parse(const char *line, Command &out);

// One-line usage for a verb ("Usage: cloudroom join CODE[@HOST]"), at most 145 bytes.
const char *usage(Verb verb);
// Fixed reply for a failed parse; Usage maps to usage(verb). Never echoes input.
const char *parseMessage(Parse result, Verb verb);

// Canonical form of a code token (2.1): uppercase, '-' removed, O->0, I and L->1; false
// when the length or a symbol is wrong. `out` receives 16 symbols and a NUL.
bool canonicalCode(const char *text, size_t length, char out[17]);
// Lowercases and validates a DNS host name of 1..maxLength bytes; false otherwise.
bool canonicalHost(const char *text, size_t length, size_t maxLength, char *out);
}
