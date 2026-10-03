// SPDX-License-Identifier: Apache-2.0
#include "BotUtilities.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace onchip;
static std::vector<uint32_t> samples;
static size_t nextSample;
static bool randomSample(uint32_t &value) {
  if (nextSample == samples.size()) return false;
  value = samples[nextSample++]; return true;
}
static void randomSequence(std::initializer_list<uint32_t> values) {
  samples = values; nextSample = 0;
}
static void bounded(const BotUtilityResult &r) {
  assert(strlen(r.text) <= BotReplyLimit);
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(r.text); *p; ++p)
    assert(*p >= 32 && *p <= 126);
}
static void arithmetic() {
  BotUtilityResult r;
  for (const auto &entry : std::vector<std::pair<const char *, const char *>>{
       {"2+3*4", "= 14"}, {"(2+3)*4", "= 20"}, {"10-3-2", "= 5"},
       {"24/3/2", "= 4"}, {"-.5 * (2 + 4)", "= -3"}, {"1e3+2.5E-2", "= 1000.025"},
       {"1/3", "= 0.3333333333"}, {"0-0", "= 0"}, {"-0", "= 0"},
       {"1e18", "= 1e+18"}, {"1e-18", "= 1e-18"}, {"1e18-1e18", "= 0"},
       {"((((((((1))))))))", "= 1"}, {"++++++++1", "= 1"}, {" 1. + .5 ", "= 1.5"}}) {
    assert(botCalculate(entry.first, r) && !strcmp(r.text, entry.second)); bounded(r);
  }
  std::string tokens = "1";
  for (unsigned i = 0; i < 31; ++i) tokens += "+1";
  assert(botCalculate(tokens.c_str(), r) && !strcmp(r.text, "= 32"));
  tokens += "+1";
  assert(!botCalculate(tokens.c_str(), r) && strstr(r.error, "token"));
  std::string operations = "++++++++1";
  for (unsigned i = 0; i < 24; ++i) operations += "+1";
  assert(botCalculate(operations.c_str(), r) && !strcmp(r.text, "= 25"));
  operations += "+1";
  assert(!botCalculate(operations.c_str(), r) && strstr(r.error, "operation"));
  for (const char *bad : {"", " ", "1+", "(1", "1)", "()", "1 2", "1/**/2", "2^3", "5%2",
                         "nan", "inf", "0x10", "1_000", "1e", "1e--2", "1e19", "0e999",
                         "1/0", "1/(2-2)", "1e18*2", "1e18+1e18", "1e-18/2",
                         "1e-18*1e-18", "1234567890123456", "0.0000000000000000001",
                         "(((((((((1)))))))))", "---------1", "sqrt(4)", "os.execute('x')",
                         "1;return 2", "1\n+2"}) {
    assert(!botCalculate(bad, r) && !r.text[0] && r.error[0]);
  }
  assert(!botCalculate(std::string(121, '1').c_str(), r));
  // Every truncated prefix must either produce a bounded result or an explicit error.
  const std::string expression = "-((1e-2+.5)/(4-3))*2";
  for (size_t n = 0; n <= expression.size(); ++n) {
    const bool ok = botCalculate(expression.substr(0, n).c_str(), r);
    if (ok) bounded(r); else assert(r.error[0] && !r.text[0]);
  }
}
static void conversions() {
  BotUtilityResult r;
  struct Example { const char *value, *from, *to, *expected; };
  for (const auto &e : {
       Example{"0", "C", "F", "0 C = 32 F"}, {"32", "F", "C", "32 F = 0 C"},
       {"-40", "C", "F", "-40 C = -40 F"}, {"-273.15", "C", "K", "-273.15 C = 0 K"},
       {"-459.67", "F", "K", "-459.67 F = 0 K"}, {"0", "K", "C", "0 K = -273.15 C"},
       {"1", "mi", "km", "1 mi = 1.609344 km"}, {"12", "in", "ft", "12 in = 1 ft"},
       {"1", "lb", "kg", "1 lb = 0.45359237 kg"}, {"16", "oz", "lb", "16 oz = 1 lb"},
       {"36", "km/h", "m/s", "36 km/h = 10 m/s"}, {"1", "mph", "m/s", "1 mph = 0.44704 m/s"},
       {"1", "d", "h", "1 d = 24 h"}, {"1.5", "h", "min", "1.5 h = 90 min"},
       {"1", "MB", "B", "1 MB = 1000000 B"}, {"1", "MiB", "B", "1 MiB = 1048576 B"},
       {"8", "b", "B", "8 b = 1 B"}, {"8", "Mb", "MB", "8 Mb = 1 MB"},
       {"1", "TiB", "B", "1 TiB = 1.099511628e+12 B"}, {"-1", "m", "cm", "-1 m = -100 cm"}}) {
    assert(botConvert(e.value, e.from, e.to, r));
    if (strcmp(r.text, e.expected)) fprintf(stderr, "%s != %s\n", r.text, e.expected);
    assert(!strcmp(r.text, e.expected)); bounded(r);
  }
  for (const auto &e : {
       Example{"-1", "K", "C", ""}, {"-273.16", "C", "F", ""}, {"1", "m", "kg", ""},
       {"1", "mb", "B", ""}, {"1", "c", "F", ""}, {"1+2", "m", "cm", ""},
       {"nan", "m", "cm", ""}, {"inf", "m", "cm", ""}, {"1e18", "km", "m", ""},
       {"1e-18", "mm", "m", ""}, {"", "s", "h", ""}}) {
    assert(!botConvert(e.value, e.from, e.to, r) && r.error[0] && !r.text[0]);
  }
  for (const char *name : {"C", "F", "K", "mm", "cm", "m", "km", "in", "ft", "yd", "mi", "nmi",
                          "mg", "g", "kg", "oz", "lb", "m/s", "km/h", "mph", "kn", "ft/s",
                          "ms", "s", "min", "h", "d", "wk", "b", "B", "kb", "Mb", "Gb", "Tb",
                          "kB", "MB", "GB", "TB", "KiB", "MiB", "GiB", "TiB"}) {
    assert(botConvert("1", name, name, r));
    const auto expected = "1 " + std::string(name) + " = 1 " + name;
    assert(!strcmp(r.text, expected.c_str()));
  }
}
static void chance() {
  BotUtilityResult r;
  randomSequence({0, 3, 4, 5}); // Six-way rejection discards the first four uint32 values.
  assert(botRoll("2d6+3", r, randomSample) && !strcmp(r.text, "Roll 2d6+3: [5,6] +3 = 14"));
  assert(nextSample == 4);
  randomSequence({6});
  assert(botRoll(nullptr, r, randomSample) && !strcmp(r.text, "Roll 1d6: [1] +0 = 1"));
  randomSequence({7});
  assert(botRoll("d6-10000", r, randomSample) && !strcmp(r.text, "Roll d6-10000: [2] -10000 = -9998"));
  samples.assign(12, UINT32_MAX); nextSample = 0;
  assert(botRoll("12d1000+10000", r, randomSample)); bounded(r);
  std::string widest = "12d1000+10000";
  widest.insert(0, 24 - widest.size(), '0');
  samples.assign(12, 999); nextSample = 0;
  assert(botRoll(widest.c_str(), r, randomSample)); bounded(r);
  assert(strlen(r.text) == 107 && strstr(r.text, "] +10000 = 22000"));
  assert(!botRoll(("0" + widest).c_str(), r, randomSample));
  samples.assign(32, 0); nextSample = 0;
  assert(!botRoll("d6", r, randomSample) && nextSample == 32 && strstr(r.error, "budget") && !r.text[0]);
  randomSequence({});
  assert(!botRoll("d6", r, randomSample) && strstr(r.error, "unavailable"));
  for (const char *bad : {"", "0d6", "13d6", "d1", "d1001", "d", "1D6", "1", "2d6+",
                         "2d6+10001", "d6-10001", "99999999999999999999d6", "d6 x", "d6+1+1"}) {
    randomSequence({42});
    assert(!botRoll(bad, r, randomSample) && !nextSample && !r.text[0] && r.error[0]);
  }
  for (unsigned i = 0; i < 6; ++i) {
    randomSequence({6 + i});
    assert(botRoll("d6", r, randomSample));
    assert(strstr(r.text, ("[" + std::to_string(i + 1) + "]").c_str()));
  }
  randomSequence({0, 4});
  assert(botChoose(" red | green | blue ", r, randomSample) &&
         !strcmp(r.text, "Choice 2/3: green") && nextSample == 2);
  for (unsigned i = 0; i < 8; ++i) {
    randomSequence({i});
    assert(botChoose("a|b|c|d|e|f|g|h", r, randomSample));
    assert(r.text[strlen(r.text) - 1] == char('a' + i));
  }
  randomSequence({0});
  const auto longChoice = std::string(64, 'a') + "|" + std::string(64, 'b');
  assert(botChoose(longChoice.c_str(), r, randomSample)); bounded(r);
  randomSequence({1});
  assert(botChoose((std::string(19, ' ') + longChoice).c_str(), r, randomSample));
  assert(strlen(r.text) == 76 && r.text[75] == 'b');
  for (const auto &bad : {"one", "|two", "one|", "one||two", "one|  |two", "a|b|c|d|e|f|g|h|i", "one\n|two"}) {
    randomSequence({42});
    assert(!botChoose(bad, r, randomSample) && !nextSample && r.error[0] && !r.text[0]);
  }
  assert(!botChoose((std::string(65, 'a') + "|b").c_str(), r));
  assert(!botChoose(std::string(149, 'a').c_str(), r));
  assert(!botChoose("a|b", r, nullptr) && strstr(r.error, "unavailable"));
  uint32_t first, value;
  assert(botUtilityRandom(first));
  bool changed = false;
  for (unsigned i = 0; i < 8; ++i) { assert(botUtilityRandom(value)); changed |= value != first; }
  assert(changed);
}
int main() {
  arithmetic(); conversions(); chance();
  puts("PASS bounded utility core: arithmetic grammar/ranges/precision/resources, six unit dimensions, exact rejection sampling, dice/choice limits and host OS entropy");
}
