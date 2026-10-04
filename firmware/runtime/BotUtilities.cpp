// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotUtilities.h"
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_system.h>
#elif defined(NRF52_PLATFORM)
#include "PineRuntimePlatform.h"
#else
#include <sys/random.h>
#endif

namespace onchip {
namespace {
bool fail(BotUtilityResult &result, const char *message) {
  result.text[0] = 0;
  snprintf(result.error, sizeof(result.error), "%s", message);
  return false;
}
bool input(const char *text, size_t limit, BotUtilityResult &result) {
  if (!text || !*text || strnlen(text, limit + 1) > limit)
    return fail(result, "Utility input empty or too long");
  for (const auto *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
    if (*p < 32 || *p > 126) return fail(result, "Utility input requires printable ASCII");
  return true;
}
bool range(double value, BotUtilityResult &result, bool lost = false) {
  if (!std::isfinite(value) || std::abs(value) > 1e18)
    return fail(result, "Numeric overflow (maximum magnitude 1e18)");
  if (lost || (value && std::abs(value) < 1e-18))
    return fail(result, "Numeric underflow (minimum nonzero magnitude 1e-18)");
  return true;
}
bool digit(char c) { return c >= '0' && c <= '9'; }
struct Arithmetic {
  const char *p;
  BotUtilityResult &result;
  unsigned tokens = 0, operations = 0;
  void space() { while (*p == ' ') ++p; }
  bool token() {
    if (++tokens > 64) return fail(result, "Arithmetic token limit (64)");
    return true;
  }
  bool operation() {
    if (++operations > 32) return fail(result, "Arithmetic operation limit (32)");
    return true;
  }
  bool number(double &value) {
    space();
    const char *start = p;
    unsigned digits = 0, significant = 0;
    bool nonzero = false;
    const auto mantissa = [&] {
      while (digit(*p)) {
        nonzero = nonzero || *p != '0';
        significant += nonzero;
        ++digits; ++p;
      }
    };
    mantissa();
    if (*p == '.') { ++p; mantissa(); }
    if (!digits) return fail(result, "Expected decimal number");
    if (significant > 15) return fail(result, "Literal precision exceeds 15 significant digits");
    if (*p == 'e' || *p == 'E') {
      ++p;
      if (*p == '+' || *p == '-') ++p;
      unsigned exponent = 0, count = 0;
      while (digit(*p)) {
        if (++count > 2) return fail(result, "Decimal exponent outside -18..18");
        exponent = exponent * 10 + unsigned(*p++ - '0');
      }
      if (!count || exponent > 18) return fail(result, "Decimal exponent outside -18..18");
    }
    const auto size = size_t(p - start);
    if (size > 32) return fail(result, "Numeric literal exceeds 32 bytes");
    char literal[33]{};
    memcpy(literal, start, size);
    errno = 0;
    char *end;
    value = strtod(literal, &end);
    if (errno == ERANGE || *end) return fail(result, "Numeric literal out of range");
    return token() && range(value, result, nonzero && value == 0);
  }
  bool factor(double &value, unsigned depth) {
    if (depth > 8) return fail(result, "Arithmetic nesting limit (8)");
    space();
    if (*p == '+' || *p == '-') {
      const char op = *p++;
      if (!token() || !operation() || !factor(value, depth + 1)) return false;
      if (op == '-') value = -value;
      return true;
    }
    if (*p == '(') {
      ++p;
      if (!token() || !expression(value, depth + 1)) return false;
      space();
      if (*p++ != ')') return fail(result, "Missing closing parenthesis");
      return token();
    }
    return number(value);
  }
  bool term(double &value, unsigned depth) {
    if (!factor(value, depth)) return false;
    space();
    while (*p == '*' || *p == '/') {
      const char op = *p++;
      double rhs;
      if (!token() || !operation() || !factor(rhs, depth)) return false;
      if (op == '/' && rhs == 0) return fail(result, "Division by zero");
      const bool nonzero = value != 0 && rhs != 0;
      value = op == '*' ? value * rhs : value / rhs;
      if (!range(value, result, nonzero && value == 0)) return false;
      space();
    }
    return true;
  }
  bool expression(double &value, unsigned depth = 0) {
    if (!term(value, depth)) return false;
    space();
    while (*p == '+' || *p == '-') {
      const char op = *p++;
      double rhs;
      if (!token() || !operation() || !term(rhs, depth)) return false;
      value = op == '+' ? value + rhs : value - rhs;
      if (!range(value, result)) return false;
      space();
    }
    return true;
  }
};
enum Dimension { Temperature, Distance, Mass, Speed, Duration, Data };
struct Unit { const char *name; Dimension dimension; double scale, zero = 0, origin = 0; };
const Unit units[] = {
  {"C", Temperature, 1, -273.15}, {"F", Temperature, 5.0 / 9, -459.67, 32},
  {"K", Temperature, 1, 0, 273.15},
  {"mm", Distance, .001}, {"cm", Distance, .01}, {"m", Distance, 1}, {"km", Distance, 1000},
  {"in", Distance, .0254}, {"ft", Distance, .3048}, {"yd", Distance, .9144},
  {"mi", Distance, 1609.344}, {"nmi", Distance, 1852},
  {"mg", Mass, .000001}, {"g", Mass, .001}, {"kg", Mass, 1},
  {"oz", Mass, .028349523125}, {"lb", Mass, .45359237},
  {"m/s", Speed, 1}, {"km/h", Speed, 1.0 / 3.6}, {"mph", Speed, .44704},
  {"kn", Speed, 1852.0 / 3600}, {"ft/s", Speed, .3048},
  {"ms", Duration, .001}, {"s", Duration, 1}, {"min", Duration, 60},
  {"h", Duration, 3600}, {"d", Duration, 86400}, {"wk", Duration, 604800},
  {"b", Data, .125}, {"B", Data, 1},
  {"kb", Data, 125}, {"Mb", Data, 125000}, {"Gb", Data, 125000000}, {"Tb", Data, 125000000000.0},
  {"kB", Data, 1000}, {"MB", Data, 1000000}, {"GB", Data, 1000000000}, {"TB", Data, 1e12},
  {"KiB", Data, 1024}, {"MiB", Data, 1048576}, {"GiB", Data, 1073741824}, {"TiB", Data, 1099511627776.0}
};
const Unit *unit(const char *name) {
  for (const auto &entry : units) if (!strcmp(entry.name, name)) return &entry;
  return nullptr;
}
struct Random {
  BotUtilityRandom source;
  BotUtilityResult &result;
  unsigned draws = 0;
  bool index(uint32_t bound, uint32_t &value) {
    const uint32_t threshold = (uint32_t(0) - bound) % bound;
    do {
      if (++draws > 32) return fail(result, "Random rejection budget exhausted (32 draws)");
      if (!source || !source(value)) return fail(result, "Native randomness unavailable");
    } while (value < threshold);
    value %= bound;
    return true;
  }
};
bool natural(const char *&p, unsigned &value, unsigned maximum, BotUtilityResult &result) {
  if (!digit(*p)) return fail(result, "Expected unsigned dice number");
  value = 0;
  while (digit(*p)) {
    value = value * 10 + unsigned(*p++ - '0');
    if (value > maximum) return fail(result, "Dice bound exceeded (12 dice, 2..1000 sides, modifier +/-10000)");
  }
  return true;
}
} // namespace

bool botUtilityRandom(uint32_t &value) {
#ifdef ARDUINO_ARCH_ESP32
  esp_fill_random(&value, sizeof(value));
  return true;
#elif defined(NRF52_PLATFORM)
  return nrfmast::secureRandom(reinterpret_cast<uint8_t *>(&value), sizeof(value));
#else
  return getrandom(&value, sizeof(value), GRND_NONBLOCK) == sizeof(value);
#endif
}
bool botCalculate(const char *expression, BotUtilityResult &result) {
  result = {};
  if (!input(expression, 120, result)) return false;
  Arithmetic parser{expression, result};
  double value;
  if (!parser.expression(value)) return false;
  if (*parser.p) return fail(result, "Unexpected arithmetic token; use + - * / and parentheses");
  snprintf(result.text, sizeof(result.text), "= %.10g", value == 0 ? 0 : value);
  return true;
}
bool botConvert(const char *text, const char *from, const char *to, BotUtilityResult &result) {
  result = {};
  if (!input(text, 32, result) || !input(from, 4, result) || !input(to, 4, result)) return false;
  Arithmetic parser{text, result};
  parser.space();
  bool negative = false;
  if (*parser.p == '-' || *parser.p == '+') negative = *parser.p++ == '-';
  double value;
  if (!parser.number(value)) return false;
  parser.space();
  if (*parser.p) return fail(result, "Conversion requires a scalar decimal, not an expression");
  if (negative) value = -value;
  const Unit *a = unit(from), *b = unit(to);
  if (!a || !b) return fail(result, "Unknown unit; units are case-sensitive");
  if (a->dimension != b->dimension) return fail(result, "Incompatible unit dimensions");
  if (a->dimension == Temperature && value < a->zero)
    return fail(result, "Temperature below absolute zero");
  // Use freezing-point origins to preserve exact 0 C / 32 F; absolute zero is exact too.
  const double converted = a == b ? value :
      a->dimension == Temperature && value == a->zero ? b->zero :
      (value - a->origin) * (a->scale / b->scale) + b->origin;
  if (!range(converted, result, a->dimension != Temperature && value != 0 && converted == 0)) return false;
  snprintf(result.text, sizeof(result.text), "%.10g %s = %.10g %s",
           value == 0 ? 0 : value, from, converted == 0 ? 0 : converted, to);
  return true;
}
bool botRoll(const char *dice, BotUtilityResult &result, BotUtilityRandom random) {
  result = {};
  if (!dice) dice = "1d6";
  if (!input(dice, 24, result)) return false;
  const char *p = dice;
  unsigned count = 1, sides = 0, adjustment = 0;
  if (*p != 'd' && !natural(p, count, 12, result)) return false;
  if (*p++ != 'd') return fail(result, "Use [N]dS[+/-K]; default 1d6");
  if (!natural(p, sides, 1000, result)) return false;
  int sign = 1;
  if (*p == '+' || *p == '-') {
    if (*p++ == '-') sign = -1;
    if (!natural(p, adjustment, 10000, result)) return false;
  }
  if (*p || !count || sides < 2) return fail(result, "Use 1..12 dice with 2..1000 sides");
  Random rng{random, result};
  size_t used = snprintf(result.text, sizeof(result.text), "Roll %s: [", dice);
  int total = sign * int(adjustment);
  for (unsigned i = 0; i < count; ++i) {
    uint32_t index;
    if (!rng.index(sides, index)) return false;
    total += int(index + 1);
    used += snprintf(result.text + used, sizeof(result.text) - used, "%s%u", i ? "," : "", unsigned(index + 1));
  }
  snprintf(result.text + used, sizeof(result.text) - used, "] %c%u = %d", sign < 0 ? '-' : '+', adjustment, total);
  return true;
}
bool botChoose(const char *choices, BotUtilityResult &result, BotUtilityRandom random) {
  result = {};
  if (!input(choices, 148, result)) return false;
  struct Choice { const char *start; unsigned size; } entries[8]{};
  unsigned count = 0;
  const char *p = choices;
  for (;;) {
    if (count == 8) return fail(result, "At most 8 alternatives");
    const char *start = p;
    while (*p && *p != '|') ++p;
    const char *end = p;
    while (start < end && *start == ' ') ++start;
    while (end > start && end[-1] == ' ') --end;
    if (start == end || end - start > 64) return fail(result, "Each alternative requires 1..64 printable bytes");
    entries[count++] = {start, unsigned(end - start)};
    if (!*p) break;
    ++p;
  }
  if (count < 2) return fail(result, "Choose requires 2..8 alternatives separated by |");
  Random rng{random, result};
  uint32_t index;
  if (!rng.index(count, index)) return false;
  const auto &selected = entries[index];
  snprintf(result.text, sizeof(result.text), "Choice %u/%u: %.*s", unsigned(index + 1), count,
           int(selected.size), selected.start);
  return true;
}
const char BotUtilitySource[] = R"lua(
function calc(expression) return utility.calc(expression) end
function convert(value,from,to) return utility.convert(value,from,to) end
function roll(dice) return utility.roll(dice) end
function choose(options) return utility.choose(options) end
command("calc","expression:text:120","Decimal + - * / (); depth <=8","calc","public","!calc (2+3)*4")
command("convert","value:string:32,from:string:4,to:string:4","Case-sensitive units; scalar value; 10 significant digits","convert","public","!convert 32 F C")
command("roll","dice?:string:24","[N]dS[+/-K]; default 1d6; <=12d1000 +/-10000","roll","public","!roll 2d6+1")
command("choose","options:text:148","2..8 nonempty | choices; each <=64 bytes","choose","public","!choose tea|coffee")
)lua";
static_assert(sizeof(BotUtilitySource) <= 1024, "Keep compiled utilities independently bounded");
}
#endif
