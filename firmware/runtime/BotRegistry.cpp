// SPDX-License-Identifier: Apache-2.0
#include "BotRegistry.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <climits>
#include <initializer_list>

namespace onchip {
bool botSourceIsWasm(const char *source, size_t size) {
  if (!source) return false;
  if (size >= 4 && !memcmp(source, "\0asm", 4)) return true;
  constexpr char prefix[] = "--@meshcore-bot";
  if (size < sizeof(prefix) - 1 || memcmp(source, prefix, sizeof(prefix) - 1)) return false;
  char header[256]{};
  size_t length = 0;
  while (length < size && length < sizeof(header) - 1 && source[length] != '\n') ++length;
  memcpy(header, source, length);
  return strstr(header, ";runtime=wamr-") != nullptr;
}
bool botStorageScope(const BotEvent &event, BotIoRequest &request, char *error, size_t capacity) {
  const auto fail = [&](const char *text) { snprintf(error, capacity, "%s", text); return false; };
  if (request.scope > BotIoRequest::Channel) return fail("Storage scope unavailable");
  const bool subscription = event.kind != BotEvent::Command;
  if (subscription && request.scope != BotIoRequest::Bot && request.scope != BotIoRequest::Channel)
    return fail("Event metadata does not authorize private user storage");
  if (request.scope == BotIoRequest::Channel) {
    if (!event.channelVerified || !event.channel[0] || event.authenticated)
      return fail("Channel storage requires verified native channel authority");
    if (!subscription && !event.targeted && request.kind != BotIoRequest::Get &&
        request.kind != BotIoRequest::List && request.kind != BotIoRequest::TimerGet)
      return fail("Channel state changes require !@BOTKEY8 targeting");
    memcpy(request.principal, event.channelId, sizeof(request.principal));
  } else {
    if ((!subscription && !event.authenticated) || event.channel[0])
      return fail("Private storage requires authenticated request authority");
    if (request.scope != BotIoRequest::Bot)
      memcpy(request.principal, event.sender, sizeof(request.principal));
  }
  if (request.sharedScope() && !event.sharedState) return fail("Shared storage scope not granted");
  request.grant = request.sharedScope() ? event.sharedGrant : 0;
  return true;
}
namespace {
struct Parameter {
  char name[BotNameLimit + 1]{};
  BotValue::Type type = BotValue::String;
  bool optional = false, rest = false;
  int32_t minimum = 0, maximum = 0;
};
bool fail(char *error, size_t capacity, const char *message) {
  snprintf(error, capacity, "%s", message);
  return false;
}
bool integer(const char *text, int32_t &result) {
  if (!text || !*text || *text == '+' || *text == ' ') return false;
  const char *digit = *text == '-' ? text + 1 : text;
  if (!*digit) return false;
  for (const char *p = digit; *p; ++p) if (*p < '0' || *p > '9') return false;
  errno = 0;
  char *end;
  const long value = strtol(text, &end, 10);
  if (errno || *end || value < INT32_MIN || value > INT32_MAX) return false;
  result = int32_t(value);
  return true;
}
bool schema(const char *input, Parameter parameters[BotParameterLimit], unsigned &count,
            char *error, size_t capacity) {
  count = 0;
  if (!input || strlen(input) > 96) return fail(error, capacity, "Argument schema exceeds 96 bytes");
  if (!*input) return true;
  char copy[97];
  strcpy(copy, input);
  bool optional = false;
  char *part = copy;
  while (part) {
    if (count == BotParameterLimit) return fail(error, capacity, "At most four arguments are supported");
    auto &p = parameters[count++];
    char *next = strchr(part, ',');
    if (next) *next++ = 0;
    char *type = strchr(part, ':');
    if (!type) return fail(error, capacity, "Schema requires name:type");
    *type++ = 0;
    const size_t nameSize = strlen(part);
    p.optional = nameSize && part[nameSize - 1] == '?';
    if (p.optional) part[nameSize - 1] = 0;
    if (!botIdentifier(part) || (optional && !p.optional))
      return fail(error, capacity, "Invalid argument name or optional argument order");
    strcpy(p.name, part);
    optional = optional || p.optional;
    char *limit = strchr(type, ':');
    if (limit) *limit++ = 0;
    if (!strcmp(type, "bool") && !limit) {
      p.type = BotValue::Boolean;
    } else if ((!strcmp(type, "string") || !strcmp(type, "text")) && limit) {
      p.rest = !strcmp(type, "text");
      if (!integer(limit, p.maximum) || p.maximum < 1 || p.maximum > int(BotArgumentsLimit) ||
          (p.rest && next))
        return fail(error, capacity, "String/text bound invalid; text must be the last argument");
    } else if (!strcmp(type, "int") && limit) {
      p.type = BotValue::Integer;
      char *maximum = strchr(limit, ':');
      if (!maximum) return fail(error, capacity, "Integer schema requires minimum and maximum");
      *maximum++ = 0;
      if (!integer(limit, p.minimum) || !integer(maximum, p.maximum) || p.minimum > p.maximum)
        return fail(error, capacity, "Invalid integer bounds");
    } else {
      return fail(error, capacity, "Use string:N, text:N, int:MIN:MAX or bool");
    }
    part = next;
  }
  return true;
}
}
const BotCommand *BotManifestView::find(const char *name) const {
  for (unsigned i = 0; i < count; ++i)
    if (!strcmp(commands[i].name, name)) return &commands[i];
  return nullptr;
}
bool botIdentifier(const char *name) {
  const size_t size = name ? strlen(name) : 0;
  if (!size || size > BotNameLimit || (*name >= '0' && *name <= '9')) return false;
  for (const char *p = name; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
  return true;
}
bool botReservedCommand(const char *name) {
  for (const char *reserved : {"ping", "test", "path", "mt", "trace", "help", "remind", "reminders", "cancel",
                             "remember", "recall", "forget", "notes", "list-memories",
                             "calc", "convert", "roll", "choose", "weather", "service", "board",
                             "about", "version", "uptime", "status", "signal", "air", "plugins", "neighbors", "admin"})
    if (!strcmp(name, reserved)) return true;
  return false;
}
bool botReadOnlyQuery(const char *name) {
  for (const char *query : {"ping", "help", "plugins", "about", "version", "uptime", "status", "signal",
                           "path", "neighbors", "air", "test", "mt", "calc", "convert", "roll", "choose"})
    if (!strcmp(name, query)) return true;
  return false;
}
bool botCommandAllowed(const BotCommand &command, const BotEvent &event) {
  const bool privateDm = event.authenticated && !event.channel[0] && !event.local;
  auto permission = command.permission;
  if (!strcmp(command.name, "admin")) permission = BotCommand::Owner;
  else if (!strcmp(command.name, "board")) permission = BotCommand::Shared;
  else if (!strcmp(command.name, "weather") || !strcmp(command.name, "service")) permission = BotCommand::Home;
  else if (!strcmp(command.name, "remind")) permission = BotCommand::Reminder;
  else for (const char *name : {"reminders", "cancel", "remember", "recall", "forget", "notes", "list-memories"})
    if (!strcmp(command.name, name)) permission = BotCommand::Private;
  switch (permission) {
  case BotCommand::Public: return true;
  case BotCommand::Private: return privateDm;
  case BotCommand::Owner: return privateDm && event.owner;
  case BotCommand::Channel: return !event.authenticated && event.channelVerified && event.channel[0] && !event.local;
  case BotCommand::Shared: return !event.authenticated && event.channelVerified && event.channel[0] && !event.local && event.sharedState;
  case BotCommand::Reminder: return privateDm && event.reminderAccess;
  case BotCommand::Home: return privateDm && event.homeAccess;
  }
  return false;
}
bool botCommandName(const char *name) {
  if (!name || *name == '-' || strlen(name) > BotNameLimit) return false;
  char identifier[BotNameLimit + 1];
  strcpy(identifier, name);
  for (char *p = identifier; *p; ++p) if (*p == '-') *p = '_';
  return botIdentifier(identifier);
}
bool validateBotSchema(const char *input, char *error, size_t capacity) {
  Parameter parameters[BotParameterLimit]{};
  unsigned count;
  return schema(input, parameters, count, error, capacity);
}
bool parseBotArguments(const BotCommand &command, const char *input,
                       BotArguments &arguments, char *error, size_t capacity) {
  arguments = BotArguments{};
  if (!input || strlen(input) > BotArgumentsLimit) return fail(error, capacity, "Command arguments exceed capacity");
  Parameter parameters[BotParameterLimit]{};
  unsigned count;
  if (!schema(command.schema, parameters, count, error, capacity)) return false;
  arguments.count = count;
  const char *cursor = input;
  for (unsigned i = 0; i < count; ++i) {
    while (*cursor == ' ') ++cursor;
    const auto &p = parameters[i];
    auto &value = arguments.values[i];
    const auto invalid = [&](const char *reason) {
      snprintf(error, capacity, "%s: %s; use !help %s", p.name,
               reason, command.name);
      return false;
    };
    if (!*cursor) {
      if (!p.optional) return invalid("required argument missing");
      continue;
    }
    size_t size = 0;
    if (p.rest) {
      size = strlen(cursor);
      memcpy(value.text, cursor, size + 1);
      cursor += size;
    } else {
      const char quote = *cursor == '\'' || *cursor == '"' ? *cursor++ : 0;
      bool closed = !quote;
      while (*cursor) {
        if (quote && *cursor == quote) { ++cursor; closed = true; break; }
        if (!quote && *cursor == ' ') break;
        if (*cursor == '\\') {
          ++cursor;
          if (!*cursor) return invalid("incomplete escape");
        }
        value.text[size++] = *cursor++;
      }
      value.text[size] = 0;
      if (!closed || (*cursor && *cursor != ' '))
        return invalid("malformed quoted argument");
    }
    value.type = p.type;
    if (p.type == BotValue::String) {
      if (!size || size > unsigned(p.maximum)) {
        char reason[48];
        snprintf(reason, sizeof(reason), "requires 1..%u bytes", unsigned(p.maximum));
        return invalid(reason);
      }
    } else if (p.type == BotValue::Integer) {
      if (!integer(value.text, value.integer) || value.integer < p.minimum || value.integer > p.maximum)
      {
        char reason[64];
        snprintf(reason, sizeof(reason), "requires integer %ld..%ld", long(p.minimum), long(p.maximum));
        return invalid(reason);
      }
    } else {
      if (strcmp(value.text, "true") && strcmp(value.text, "false"))
        return invalid("requires true or false");
      value.integer = !strcmp(value.text, "true");
    }
  }
  while (*cursor == ' ') ++cursor;
  if (!*cursor) return true;
  snprintf(error, capacity, "too many arguments; use !help %s", command.name);
  return false;
}
} // namespace onchip
