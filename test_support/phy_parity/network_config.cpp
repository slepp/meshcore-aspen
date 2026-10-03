#include "RadioNetwork.h"

#ifndef EXPECTED_HOSTNAME
#define EXPECTED_HOSTNAME "meshcore-radio"
#endif

constexpr bool equal(const char* a, const char* b) {
  return *a == *b && (!*a || equal(a + 1, b + 1));
}

static_assert(equal(radio_network::hostname, EXPECTED_HOSTNAME), "Hostname override was lost");
static_assert(radio_network::validHostname("meshcore-radio"), "Default hostname invalid");
static_assert(radio_network::validHostname("abcdefghijklmnopqrstuvwxyz12345"), "31-byte label rejected");
static_assert(!radio_network::validHostname("abcdefghijklmnopqrstuvwxyz123456"), "SDK truncation permitted");
static_assert(!radio_network::validHostname(""), "Empty hostname accepted");
static_assert(!radio_network::validHostname("-radio"), "Leading hyphen accepted");
static_assert(!radio_network::validHostname("radio-"), "Trailing hyphen accepted");
static_assert(!radio_network::validHostname("radio.local"), "FQDN accepted as bare label");
static_assert(!radio_network::validHostname("radio name"), "Space accepted");
static_assert(!radio_network::validHostname("radio_name"), "Underscore accepted");
static_assert(!radio_network::validHostname("radio/host"), "URI accepted as hostname");
