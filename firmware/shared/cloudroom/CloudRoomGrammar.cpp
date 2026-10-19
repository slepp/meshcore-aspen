// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomGrammar.h"
#include <string.h>

namespace cloudroom {
namespace {
constexpr size_t PrefixLength = 10;  // "cloudroom "

void wipe(void *data, size_t size) {
  volatile char *bytes = static_cast<volatile char *>(data);
  for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool equals(const char *a, const char *b) {
  while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
  return !*a && !*b;
}
bool digits(const char *s) {
  if (!*s) return false;
  for (; *s; ++s) if (*s < '0' || *s > '9') return false;
  return true;
}
bool atoi_positive(const char *s) {  // digits only, value >= 1
  for (; *s; ++s) if (*s != '0') return true;
  return false;
}
bool hex(const char *s) {
  for (; *s; ++s)
    if (!((*s >= '0' && *s <= '9') || (lower(*s) >= 'a' && lower(*s) <= 'f'))) return false;
  return true;
}
// Space-separated words are cut in place. `rest` follows the word with the spaces skipped.
char *word(char *&cursor) {
  while (*cursor == ' ') ++cursor;
  char *value = cursor;
  while (*cursor && *cursor != ' ') ++cursor;
  if (*cursor) *cursor++ = 0;
  while (*cursor == ' ') ++cursor;
  return value;
}
bool nameByte(unsigned char c) {
  return c >= 32 && c <= 126 && !strchr(":[]\\,?*", c);
}
// Splits "CODE@HOST". The code part is canonicalised (or "OPEN" when allowed).
Parse codeAndHost(char *token, bool allowOpen, Command &out, bool &open) {
  char *at = strchr(token, '@');
  size_t codeLength = at ? size_t(at - token) : strlen(token);
  open = false;
  if (allowOpen && codeLength == 4 && (token[0] | 32) == 'o' && (token[1] | 32) == 'p' &&
      (token[2] | 32) == 'e' && (token[3] | 32) == 'n') {
    open = true;
    strcpy(out.code, "OPEN");
  } else if (codeLength < 16 || codeLength > 19 || !canonicalCode(token, codeLength, out.code)) {
    return Parse::BadCode;
  }
  if (at && !canonicalHost(at + 1, strlen(at + 1), HostLimit, out.host)) return Parse::BadHost;
  return Parse::Ok;
}
}

bool canonicalCode(const char *text, size_t length, char out[17]) {
  static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  size_t count = 0;
  for (size_t i = 0; i < length; ++i) {
    char c = text[i];
    if (c == '-') continue;
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c == 'O') c = '0';
    else if (c == 'I' || c == 'L') c = '1';
    if (!c || !strchr(alphabet, c) || count == 16) return false;
    out[count++] = c;
  }
  if (count != 16) return false;
  out[16] = 0;
  return true;
}

bool canonicalHost(const char *text, size_t length, size_t maxLength, char *out) {
  if (length < 1 || length > maxLength) return false;
  size_t label = 0, lastStart = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = lower(text[i]);
    if (c == '.') {
      if (!label || out[i - 1] == '-') return false;
      label = 0;
      lastStart = i + 1;
    } else {
      const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      if (!valid || ++label > 63 || (label == 1 && c == '-')) return false;
    }
    out[i] = c;
  }
  if (!label || out[length - 1] == '-') return false;
  out[length] = 0;
  // A numeric last label is an IPv4 literal; no top-level domain is numeric.
  return !digits(out + lastStart);
}

const char *usage(Verb verb) {
  switch (verb) {
    case Verb::List: return "Usage: cloudroom list [N]";
    case Verb::Probe: return "Usage: cloudroom probe [HOST]";
    case Verb::Join: return "Usage: cloudroom join CODE[@HOST]";
    case Verb::Create: return "Usage: cloudroom create CODE[@HOST] NAME";
    case Verb::Convert: return "Usage: cloudroom convert CODE[@HOST] [yes [HOST]]";
    case Verb::Invite: return "Usage: cloudroom invite [owner]";
    case Verb::Nodes: return "Usage: cloudroom nodes [N]";
    case Verb::Revoke: return "Usage: cloudroom revoke ID (8..16 hex from nodes)";
    case Verb::Password: return "Usage: cloudroom password [PW|open|new]";
    case Verb::Unlink: return "Usage: cloudroom unlink [ALIAS] [delete|local] [force]";
    case Verb::Share: return "Usage: cloudroom share on|off [force]";
    case Verb::Result: return "Usage: cloudroom result";
    case Verb::Apply: return "Usage: cloudroom apply";
    default: return "Usage: cloudroom list|probe|join|create|convert|invite|nodes|revoke|password|unlink|share|result|apply";
  }
}

const char *parseMessage(Parse result, Verb verb) {
  switch (result) {
    case Parse::Ok: return "";
    case Parse::BadCode: return "Code not valid. Use 16 letters and digits, e.g. ABCD-EFGH-JKMN-PQRS.";
    case Parse::BadHost: return "Host not valid. Use a DNS name of up to 36 bytes, e.g. rooms.example.com.";
    case Parse::BadName: return "Room name not valid. Use 1..31 printable characters, none of : [ ] \\ , ? *";
    case Parse::TooLong: return "Command too long. The longest request is 145 bytes.";
    default: return usage(verb);
  }
}

Parse parse(const char *line, Command &out) {
  out = Command{};
  out.verb = Verb::NotNew;
  if (!line) return Parse::NotCloudroom;
  const Verb verb = verbOf(line);
  if (verb == Verb::NotNew) return Parse::Ok;
  line = detail::skipPrefix(line);
  const size_t length = strlen(line);
  char buffer[RequestLimit + 1];
  out.verb = verb;
  if (length + PrefixLength > RequestLimit) return Parse::TooLong;
  memcpy(buffer, line, length + 1);
  char *cursor = buffer;
  word(cursor);  // the verb
  // Trailing spaces never count as an argument or as part of a name or password.
  size_t end = strlen(cursor);
  while (end && cursor[end - 1] == ' ') cursor[--end] = 0;
  Parse result = Parse::Ok;
  bool open = false;
  switch (verb) {
    case Verb::Result:
    case Verb::Apply:
      if (*cursor) result = Parse::Usage;
      break;
    case Verb::List:
    case Verb::Nodes: {
      char *page = word(cursor);
      if (*cursor) result = Parse::Usage;
      else if (*page) {
        if (!digits(page) || strlen(page) > 3 || !atoi_positive(page)) result = Parse::Usage;
        else strcpy(out.arg, page);
      }
      break;
    }
    case Verb::Probe: {
      char *host = word(cursor);
      if (*cursor) result = Parse::Usage;
      else if (*host && !canonicalHost(host, strlen(host), HostLimit, out.host)) result = Parse::BadHost;
      break;
    }
    case Verb::Join:
    case Verb::Convert:
    case Verb::Create: {
      char *token = word(cursor);
      if (!*token) { result = Parse::Usage; break; }
      result = codeAndHost(token, verb == Verb::Create, out, open);
      if (result != Parse::Ok) break;
      out.open = open;
      if (verb == Verb::Join) {
        if (*cursor) result = Parse::Usage;
      } else if (verb == Verb::Create) {
        if (!*cursor) { result = Parse::Usage; break; }
        const size_t nameLength = strlen(cursor);
        if (nameLength > NameLimit) { result = Parse::BadName; break; }
        for (size_t i = 0; i < nameLength; ++i)
          if (!nameByte(static_cast<unsigned char>(cursor[i]))) { result = Parse::BadName; break; }
        if (result == Parse::Ok) memcpy(out.name, cursor, nameLength + 1);
      } else if (*cursor) {
        char *yes = word(cursor);
        if (!equals(yes, "yes")) { result = Parse::Usage; break; }
        out.yes = true;
        char *host = word(cursor);
        if (*cursor) result = Parse::Usage;
        else if (*host && !canonicalHost(host, strlen(host), HostLimit, out.yesHost)) result = Parse::BadHost;
        // The key leaves the node only when the host is retyped exactly: "yes HOST" for a
        // named host, a bare "yes" for the default. Anything else stays a preview.
        if (result == Parse::Ok && strcmp(out.host, out.yesHost)) {
          out.yes = false;
          out.yesHost[0] = 0;
        }
      }
      break;
    }
    case Verb::Invite: {
      char *role = word(cursor);
      if (*cursor || (*role && !equals(role, "owner"))) result = Parse::Usage;
      else out.owner = *role != 0;
      break;
    }
    case Verb::Revoke: {
      char *id = word(cursor);
      const size_t idLength = strlen(id);
      if (*cursor || idLength < 8 || idLength > 16 || !hex(id)) result = Parse::Usage;
      else for (size_t i = 0; i <= idLength; ++i) out.arg[i] = lower(id[i]);
      break;
    }
    case Verb::Password:
      // The password is the rest of the line, so it may hold spaces.
      if (!*cursor) break;
      if (equals(cursor, "open")) out.open = true;
      else if (equals(cursor, "new")) out.generate = true;
      else {
        const size_t passwordLength = strlen(cursor);
        if (passwordLength > PasswordLimit) { result = Parse::Usage; break; }
        memcpy(out.arg, cursor, passwordLength + 1);
      }
      break;
    case Verb::Unlink:
      while (*cursor && result == Parse::Ok) {
        char *token = word(cursor);
        if (equals(token, "delete")) out.remove = true;
        else if (equals(token, "local")) out.local = true;
        else if (equals(token, "force")) out.force = true;
        else if (!out.alias[0] && strlen(token) < sizeof(out.alias)) strcpy(out.alias, token);
        else result = Parse::Usage;
      }
      if (out.remove && out.local) result = Parse::Usage;
      break;
    case Verb::Share: {
      char *state = word(cursor);
      if (equals(state, "on")) out.on = true;
      else if (!equals(state, "off")) { result = Parse::Usage; break; }
      if (*cursor) {
        char *force = word(cursor);
        if (*cursor || !equals(force, "force")) result = Parse::Usage;
        else out.force = true;
      }
      break;
    }
    default:
      break;
  }
  wipe(buffer, sizeof(buffer));
  if (result != Parse::Ok) {
    const Verb recognised = out.verb;
    wipe(&out, sizeof(out));
    out.verb = recognised;
  }
  return result;
}
}
