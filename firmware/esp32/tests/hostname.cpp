// Network name formation: KISS_HOSTNAME plus the factory MAC suffix, or the
// fixed prefix when KISS_HOSTNAME_FIXED=1.
#include "RadioNetwork.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

int main() {
  using radio_network::formatHostname;
  const uint8_t mac[6] = {0x20, 0x6e, 0xf1, 0x88, 0x07, 0xb0};
  char out[radio_network::HostnameCapacity];

  assert(formatHostname("aspen", mac, false, out, sizeof(out)) && !strcmp(out, "aspen-8807b0"));
  assert(formatHostname("aspen", mac, true, out, sizeof(out)) && !strcmp(out, "aspen"));
  const uint8_t edge[6] = {0, 0, 0, 0x0a, 0xff, 0x00};
  assert(formatHostname("Field-Node", edge, false, out, sizeof(out)) &&
         !strcmp(out, "Field-Node-0aff00"));

  // 24 + 7 = 31 bytes is the longest name the WiFi driver keeps.
  const std::string longest(24, 'a'), tooLong(25, 'a');
  assert(formatHostname(longest.c_str(), mac, false, out, sizeof(out)) && strlen(out) == 31 &&
         radio_network::validHostname(out));
  assert(!formatHostname(tooLong.c_str(), mac, false, out, sizeof(out)) && !*out);
  assert(formatHostname(std::string(31, 'b').c_str(), mac, true, out, sizeof(out)));
  assert(!formatHostname(std::string(32, 'b').c_str(), mac, true, out, sizeof(out)) && !*out);
  char small[8];
  assert(!formatHostname("aspen", mac, false, small, sizeof(small)) && !*small);

  const char *name = radio_network::hostname();
  assert(name == radio_network::hostname()); // Computed once.
  assert(!strcmp(name, EXPECTED_HOSTNAME) && radio_network::validHostname(name));
  std::printf("Hostname %s formed from prefix %s\n", name, radio_network::hostnamePrefix);
}
