// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomGrammar.h"
#include <stdio.h>
#include <string.h>
#include <string>

using namespace cloudroom;
static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static Parse run(const char *line, Command &c) { return parse(line, c); }

static void legacy() {
  for (const char *line : {"", "status", "error", "advertise TestA", "config begin 00", "config chunk aa 0 ff",
                           "enable on", "enable off", "bogus", "joined", "listing", "config status"}) {
    Command c;
    CHECK(run(line, c) == Parse::Ok);
    CHECK(c.verb == Verb::NotNew);
  }
  Command c;
  CHECK(parse(nullptr, c) == Parse::NotCloudroom);
  // A legacy line is never rejected for length: config chunks are long.
  std::string chunk = "config chunk 0123456789abcdef 3 " + std::string(96, 'a');
  CHECK(run(chunk.c_str(), c) == Parse::Ok && c.verb == Verb::NotNew);
}

static void codes() {
  struct Case { const char *token; bool ok; const char *canonical; } cases[] = {
    {"ABCDEFGHJKMNPQRS", true, "ABCDEFGHJKMNPQRS"},
    {"ABCD-EFGH-JKMN-PQRS", true, "ABCDEFGHJKMNPQRS"},
    {"abcd-efgh-jkmn-pqrs", true, "ABCDEFGHJKMNPQRS"},
    {"ABCD-EFGH-JKMN-PQRS-", false, ""},
    {"OOOO-IIII-LLLL-0123", true, "0000111111110123"},
    {"oiloiloiloiloil0", true, "0110110110110110"},
    {"ABCDEFGHJKMNPQRU", false, ""},        // U is not in the alphabet
    {"ABCDEFGHJKMNPQR", false, ""},         // 15
    {"ABCDEFGHJKMNPQRST", false, ""},       // 17 symbols
    {"ABCDEFGHJKMNPQR!", false, ""},
    {"ABCD_EFGH_JKMN_PQRS", false, ""},
    {"-ABCDEFGHJKMNPQRS", true, "ABCDEFGHJKMNPQRS"},
    {"A-B-C-D-EFGHJKMNPQRS", false, ""},   // 20 raw bytes
  };
  for (const auto &t : cases) {
    Command c;
    const std::string line = std::string("join ") + t.token;
    const Parse r = run(line.c_str(), c);
    if (t.ok) { CHECK(r == Parse::Ok); CHECK(!strcmp(c.code, t.canonical)); CHECK(!c.host[0]); }
    else { CHECK(r == Parse::BadCode); CHECK(!c.code[0]); }
    CHECK(c.verb == Verb::Join);
  }
  char out[17];
  CHECK(canonicalCode("ABCD-EFGH-JKMN-PQRS", 19, out) && !strcmp(out, "ABCDEFGHJKMNPQRS"));
}

static void hosts() {
  Command c;
  CHECK(run("join ABCD-EFGH-JKMN-PQRS@Rooms.Example.COM", c) == Parse::Ok && !strcmp(c.host, "rooms.example.com"));
  CHECK(run("join ABCDEFGHJKMNPQRS@a", c) == Parse::Ok && !strcmp(c.host, "a"));
  // 36 bytes is accepted in full, 37 is not.
  std::string h36 = std::string(30, 'a') + ".b.com";
  CHECK(h36.size() == 36);
  CHECK(run(("join ABCDEFGHJKMNPQRS@" + h36).c_str(), c) == Parse::Ok && h36 == c.host);
  CHECK(run(("join ABCDEFGHJKMNPQRS@" + h36 + "x").c_str(), c) == Parse::BadHost);
  for (const char *bad : {"", "https://x.com", "x.com:443", "x.com/path", "u@x.com", "-x.com", "x-.com", "x..com",
                          ".x.com", "x.com.", "x_y.com", "192.168.1.1", "1.2.3.4", "[::1]", "x.com?a",
                          "sp ace.com"}) {
    const std::string line = std::string("join ABCDEFGHJKMNPQRS@") + bad;
    const Parse r = run(line.c_str(), c);
    CHECK(r == Parse::BadHost || r == Parse::Usage);
    CHECK(r != Parse::Ok);
  }
  CHECK(run("join ABCDEFGHJKMNPQRS@x.123", c) == Parse::BadHost);
  CHECK(run("join ABCDEFGHJKMNPQRS@1x.com", c) == Parse::Ok);
  std::string label64 = std::string(64, 'a') + ".com";
  CHECK(run(("probe " + label64).c_str(), c) == Parse::BadHost);
  // A 63-byte label is fine when the host stays within 36 bytes? It cannot; 36 caps it.
  CHECK(run("probe", c) == Parse::Ok && c.verb == Verb::Probe && !c.host[0]);
  CHECK(run("probe rooms.example.com", c) == Parse::Ok && !strcmp(c.host, "rooms.example.com"));
  CHECK(run("probe a b", c) == Parse::Usage);
}

static void join() {
  Command c;
  CHECK(run("join", c) == Parse::Usage && c.verb == Verb::Join);
  CHECK(run("join ABCDEFGHJKMNPQRS extra", c) == Parse::Usage);
  CHECK(run("join   ABCDEFGHJKMNPQRS   ", c) == Parse::Ok && !strcmp(c.code, "ABCDEFGHJKMNPQRS"));
  CHECK(run("cloudroom join ABCDEFGHJKMNPQRS", c) == Parse::Ok && c.verb == Verb::Join);
  CHECK(run("join open", c) == Parse::BadCode);
}

static void create() {
  Command c;
  CHECK(run("create ABCD-EFGH-JKMN-PQRS Back Yard Room", c) == Parse::Ok);
  CHECK(c.verb == Verb::Create && !strcmp(c.name, "Back Yard Room") && !c.open);
  CHECK(run("create ABCDEFGHJKMNPQRS@x.example.com   Fleet  ", c) == Parse::Ok && !strcmp(c.name, "Fleet") &&
        !strcmp(c.host, "x.example.com"));
  CHECK(run("create OPEN Fleet", c) == Parse::Ok && c.open && !strcmp(c.code, "OPEN"));
  CHECK(run("create open@x.example.com Fleet", c) == Parse::Ok && c.open && !strcmp(c.host, "x.example.com"));
  CHECK(run("create OpEn Fleet", c) == Parse::Ok && c.open);
  CHECK(run("create ABCDEFGHJKMNPQRS", c) == Parse::Usage);
  CHECK(run("create ABCDEFGHJKMNPQRS ", c) == Parse::Usage);
  CHECK(run("create", c) == Parse::Usage);
  const std::string n31(31, 'n'), n32(32, 'n');
  CHECK(run(("create ABCDEFGHJKMNPQRS " + n31).c_str(), c) == Parse::Ok && n31 == c.name);
  CHECK(run(("create ABCDEFGHJKMNPQRS " + n32).c_str(), c) == Parse::BadName);
  for (const char *bad : {"a:b", "a[b", "a]b", "a\\b", "a,b", "a?b", "a*b"}) {
    const std::string line = std::string("create ABCDEFGHJKMNPQRS ") + bad;
    CHECK(run(line.c_str(), c) == Parse::BadName);
  }
  CHECK(run("create ABCDEFGHJKMNPQRS caf\xC3\xA9", c) == Parse::BadName);
  CHECK(run("create ABCDEFGHJKMNPQRS A-b_c'd\"e!f", c) == Parse::Ok);
  CHECK(run("create ABCDEFGHJKMNPQRU Fleet", c) == Parse::BadCode);
}

static void convert() {
  Command c;
  CHECK(run("convert ABCD-EFGH-JKMN-PQRS", c) == Parse::Ok && c.verb == Verb::Convert && !c.yes);
  CHECK(run("convert ABCDEFGHJKMNPQRS yes", c) == Parse::Ok && c.yes && !c.yesHost[0]);
  CHECK(run("convert ABCDEFGHJKMNPQRS@x.example.com YES x.example.com", c) == Parse::Ok && c.yes &&
        !strcmp(c.yesHost, "x.example.com") && !strcmp(c.host, "x.example.com"));
  CHECK(run("convert ABCDEFGHJKMNPQRS@x.example.com yes X.Example.com", c) == Parse::Ok &&
        !strcmp(c.yesHost, "x.example.com"));
  // A named host needs its exact retype; a default-host convert takes a bare yes only.
  CHECK(run("convert ABCDEFGHJKMNPQRS@x.example.com yes", c) == Parse::Ok && !c.yes && !c.yesHost[0]);
  CHECK(run("convert ABCDEFGHJKMNPQRS@x.example.com yes y.example.com", c) == Parse::Ok && !c.yes && !c.yesHost[0]);
  CHECK(run("convert ABCDEFGHJKMNPQRS yes x.example.com", c) == Parse::Ok && !c.yes && !c.yesHost[0]);
  CHECK(run("convert ABCDEFGHJKMNPQRS yes", c) == Parse::Ok && c.yes);
  CHECK(run("convert ABCDEFGHJKMNPQRS no", c) == Parse::Usage);
  CHECK(run("convert ABCDEFGHJKMNPQRS yes a b", c) == Parse::Usage);
  CHECK(run("convert ABCDEFGHJKMNPQRS yes bad_host", c) == Parse::BadHost);
  CHECK(run("convert OPEN", c) == Parse::BadCode);
  CHECK(run("convert", c) == Parse::Usage);
  std::string h36 = std::string(30, 'a') + ".b.com";
  CHECK(run(("convert ABCDEFGHJKMNPQRS@" + h36 + " yes " + h36).c_str(), c) == Parse::Ok && h36 == c.yesHost);
}

static void smallVerbs() {
  Command c;
  CHECK(run("invite", c) == Parse::Ok && c.verb == Verb::Invite && !c.owner);
  CHECK(run("invite owner", c) == Parse::Ok && c.owner);
  CHECK(run("invite OWNER", c) == Parse::Ok && c.owner);
  CHECK(run("invite admin", c) == Parse::Usage);
  CHECK(run("invite owner now", c) == Parse::Usage);
  CHECK(run("list", c) == Parse::Ok && c.verb == Verb::List && !c.arg[0]);
  CHECK(run("list 2", c) == Parse::Ok && !strcmp(c.arg, "2"));
  CHECK(run("list 0", c) == Parse::Usage);
  CHECK(run("list x", c) == Parse::Usage);
  CHECK(run("list 1 2", c) == Parse::Usage);
  CHECK(run("nodes", c) == Parse::Ok && c.verb == Verb::Nodes);
  CHECK(run("nodes 12", c) == Parse::Ok && !strcmp(c.arg, "12"));
  CHECK(run("nodes 1000", c) == Parse::Usage);
  CHECK(run("result", c) == Parse::Ok && c.verb == Verb::Result);
  CHECK(run("result now", c) == Parse::Usage);
  CHECK(run("apply", c) == Parse::Ok && c.verb == Verb::Apply);
  CHECK(run("apply force", c) == Parse::Usage);
  CHECK(run("revoke AB12cd34", c) == Parse::Ok && !strcmp(c.arg, "ab12cd34"));
  CHECK(run("revoke 0123456789abcdef", c) == Parse::Ok && !strcmp(c.arg, "0123456789abcdef"));
  CHECK(run("revoke 0123456789abcdef0", c) == Parse::Usage);
  CHECK(run("revoke 1234567", c) == Parse::Usage);
  CHECK(run("revoke zz123456", c) == Parse::Usage);
  CHECK(run("revoke", c) == Parse::Usage);
  CHECK(run("revoke ab12cd34 ef", c) == Parse::Usage);
}

static void password() {
  Command c;
  CHECK(run("password", c) == Parse::Ok && c.verb == Verb::Password && !c.arg[0] && !c.open && !c.generate);
  CHECK(run("password open", c) == Parse::Ok && c.open && !c.arg[0]);
  CHECK(run("password NEW", c) == Parse::Ok && c.generate && !c.arg[0]);
  CHECK(run("password 7QX2M9KDAB", c) == Parse::Ok && !strcmp(c.arg, "7QX2M9KDAB") && !c.open);
  CHECK(run("password two words", c) == Parse::Ok && !strcmp(c.arg, "two words"));
  CHECK(run("password 123456789012345", c) == Parse::Ok);
  CHECK(run("password 1234567890123456", c) == Parse::Usage);
}

static void unlink() {
  Command c;
  CHECK(run("unlink", c) == Parse::Ok && c.verb == Verb::Unlink && !c.alias[0] && !c.remove && !c.local && !c.force);
  CHECK(run("unlink 2", c) == Parse::Ok && !strcmp(c.alias, "2"));
  CHECK(run("unlink r2q7x3bndkamw4qzfxe7abcdmn2xyz delete", c) == Parse::Ok && c.remove &&
        !strcmp(c.alias, "r2q7x3bndkamw4qzfxe7abcdmn2xyz"));
  CHECK(run("unlink delete", c) == Parse::Ok && c.remove && !c.alias[0]);
  CHECK(run("unlink TestA local force", c) == Parse::Ok && c.local && c.force && !strcmp(c.alias, "TestA"));
  CHECK(run("unlink force local 1", c) == Parse::Ok && c.local && c.force && !strcmp(c.alias, "1"));
  CHECK(run("unlink 1 delete local", c) == Parse::Usage);
  CHECK(run("unlink 1 2", c) == Parse::Usage);
  CHECK(run(("unlink " + std::string(64, 'a')).c_str(), c) == Parse::Ok);
  CHECK(run(("unlink " + std::string(65, 'a')).c_str(), c) == Parse::Usage);
}

static void share() {
  Command c;
  CHECK(run("share on", c) == Parse::Ok && c.verb == Verb::Share && c.on && !c.force);
  CHECK(run("share off force", c) == Parse::Ok && !c.on && c.force);
  CHECK(run("share OFF", c) == Parse::Ok && !c.on);
  CHECK(run("share", c) == Parse::Usage);
  CHECK(run("share maybe", c) == Parse::Usage);
  CHECK(run("share on now", c) == Parse::Usage);
}

static void limitsAndWipe() {
  Command c;
  // Every verb at its widest input stays within 145 bytes including the prefix.
  const std::string h36 = std::string(30, 'a') + ".b.com", n31(31, 'n');
  const std::string widest[] = {
    "cloudroom join ABCD-EFGH-JKMN-PQRS@" + h36,
    "cloudroom create ABCD-EFGH-JKMN-PQRS@" + h36 + " " + n31,
    "cloudroom convert ABCD-EFGH-JKMN-PQRS@" + h36 + " yes " + h36,
    "cloudroom unlink " + std::string(64, 'a') + " delete force",
  };
  for (const auto &line : widest) {
    CHECK(line.size() <= RequestLimit);
    CHECK(run(line.c_str() + 10, c) == Parse::Ok);
  }
  CHECK(run(("unlink " + std::string(64, 'a') + " delete force" + std::string(60, ' ') + "x").c_str(), c) == Parse::TooLong);
  CHECK(c.verb == Verb::Unlink);
  // Over-long text is rejected even if the verb is the only valid part.
  CHECK(run(("create ABCDEFGHJKMNPQRS " + std::string(130, 'x')).c_str(), c) == Parse::TooLong);

  // A failed parse leaves no code, name or password behind.
  CHECK(run("create ABCDEFGHJKMNPQRS bad:name", c) == Parse::BadName);
  CHECK(!c.code[0] && !c.name[0] && !c.host[0] && c.verb == Verb::Create);
  CHECK(run("password 1234567890123456", c) == Parse::Usage);
  char zero[sizeof(c.arg)] = {};
  CHECK(!memcmp(c.arg, zero, sizeof(zero)));

  // Messages are fixed text within one reply.
  for (Verb v : {Verb::NotNew, Verb::List, Verb::Probe, Verb::Join, Verb::Create, Verb::Convert, Verb::Invite,
                 Verb::Nodes, Verb::Revoke, Verb::Password, Verb::Unlink, Verb::Share, Verb::Result, Verb::Apply})
    for (Parse p : {Parse::Usage, Parse::BadCode, Parse::BadHost, Parse::BadName, Parse::TooLong}) {
      const char *m = parseMessage(p, v);
      CHECK(m && *m && strlen(m) <= RequestLimit);
    }
  CHECK(!strcmp(usage(Verb::Join), "Usage: cloudroom join CODE[@HOST]"));
  CHECK(!strcmp(parseMessage(Parse::Usage, Verb::Create), usage(Verb::Create)));
}

static void gating() {
  CHECK(verbOf("join ABCD") == Verb::Join);
  CHECK(verbOf("cloudroom unlink") == Verb::Unlink);
  CHECK(verbOf("status") == Verb::NotNew && verbOf("config chunk x") == Verb::NotNew && verbOf(nullptr) == Verb::NotNew);
  CHECK(verbOf("joining") == Verb::NotNew && verbOf("") == Verb::NotNew);
  for (Verb v : {Verb::Probe, Verb::Join, Verb::Create, Verb::Convert, Verb::Invite, Verb::Nodes, Verb::Revoke,
                 Verb::Password, Verb::Unlink, Verb::Result, Verb::Apply})
    CHECK(needsEncryptedRf(v));
  for (Verb v : {Verb::NotNew, Verb::List, Verb::Share}) CHECK(!needsEncryptedRf(v));
  CHECK(strlen(encryptedRfMessage()) <= RequestLimit);
}

int main() {
  legacy(); codes(); hosts(); join(); create(); convert(); smallVerbs(); password(); unlink(); share(); limitsAndWipe(); gating();
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  puts("cloudroom grammar ok");
  return 0;
}
