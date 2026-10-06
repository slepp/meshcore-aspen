// SPDX-License-Identifier: Apache-2.0
#include "ObserverConfig.h"
#include <SPIFFS.h>
#include <nvs.h>
#include <cassert>
#include <cstring>
#include <string>

using namespace onchip;
unsigned long millis() { return 0; }
void delay(unsigned long) {}
static const identity_test::Key Key{"mc-onchip", "observer-config"};
static std::string command(const std::string &text, bool success = true) {
  char reply[163]{};
  observerConfigCommand(text.c_str(), reply, sizeof(reply));
  if ((strncmp(reply, "Error:", 6) != 0) != success)
    fprintf(stderr, "Unexpected observer configuration reply: %s\n", reply);
  assert((strncmp(reply, "Error:", 6) != 0) == success);
  assert(strlen(reply) <= 162);
  return reply;
}
static void reboot() {
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  filesystem_test::failOpen = false;
  filesystem_test::writeLimit = filesystem_test::readLimit = SIZE_MAX;
  identity_test::handles.clear();
  resetObserverConfigForTest();
}
static void reset() {
  reboot();
  identity_test::durable.clear();
  filesystem_test::files.clear();
}
static void append(const char *field, const std::string &text) {
  for (size_t offset = 0; offset < text.size(); offset += 64) {
    std::string hex;
    for (size_t i = offset; i < std::min(text.size(), offset + 64); ++i) {
      char byte[3];
      snprintf(byte, sizeof(byte), "%02x", unsigned(uint8_t(text[i])));
      hex += byte;
    }
    command(std::string(field) + " " + hex);
  }
}
static void expect(const ObserverConfig &expected) {
  ObserverConfig actual;
  assert(loadObserverConfig(actual));
  assert(!memcmp(&actual, &expected, sizeof(actual)));
  assert(identity_test::handles.empty());
}
int main() {
  reset();
  ObserverConfig initial;
  assert(loadObserverConfig(initial));
  assert(!strcmp(initial.uri, "mqtt://test-broker:11883"));
  assert(!strcmp(initial.username, "user"));
  assert(!strcmp(initial.password, "secret/@:pass"));
  assert(identity_test::durable.empty() && filesystem_test::files.empty());
  command("commit");
  assert(identity_test::durable.at(Key).size() == 40);
  const auto before = identity_test::durable;
  reboot();
  expect(initial);
  assert(identity_test::durable == before);
  assert(command("uri") == initial.uri);
  assert(command("status").find(initial.password) == std::string::npos);
  command("uri mqtt://replacement:11883");
  command("format 1");
  command("iata YEG");
  command("name Saved Aspen observer");
  command("password clear");
  append("password", std::string(256, 'p'));
  command("password 70", false);
  command("ca clear");
  append("ca", std::string(4096, 'C'));
  command("ca 43", false);
  command("audience public.example");
  command("commit", false);
  command("audience -");
  command("filter 65535");
  command("commit");
  ObserverConfig saved;
  assert(loadObserverConfig(saved));
  assert(strlen(saved.ca) == 4096 && strlen(saved.password) == 256 && saved.format == 1);
  assert(!strcmp(saved.name, "Saved Aspen observer"));
  assert(filesystem_test::files.size() == 2);
  reboot();
  expect(saved);
  command("uri mqtt://discarded");
  command("discard");
  command("commit");
  expect(saved);
  command("password clear");
  command("username clear");
  command("audience public.example");
  command("uri wss://public.example:443");
  command("commit");
  assert(loadObserverConfig(saved) && !strcmp(saved.audience, "public.example"));
  reboot();
  expect(saved);
  for (const char *uri : {"mqtt://", "mqtt://user:secret@broker", "https://broker", "mqtt:///path", "mqtt://broker#part"}) {
    command(std::string("uri ") + uri);
    command("commit", false);
    command("discard");
  }
  for (const char *text : {"format 3", "filter 65536", "filter 999999999999999999", "format -1", "password 0011", "ca 0a00"}) {
    command(text, false);
    expect(saved);
  }
  command("uri mqtt://uncommitted");
  filesystem_test::writeLimit = sizeof(ObserverConfig) - 1;
  command("commit", false);
  assert(command("status").find("storage=error") != std::string::npos);
  reboot();
  expect(saved);
  command("uri mqtt://uncertain");
  identity_test::failCommit = true;
  command("commit", false);
  reboot();
  expect(saved);
  identity_test::durable.at(Key)[0] ^= 1;
  reboot();
  ObserverConfig broken;
  assert(!loadObserverConfig(broken));
  assert(command("status").find("storage=error") != std::string::npos);
  command("commit", false);
  puts("PASS saved observer defaults migration, credential redaction, maximum CA/password, restart, bounds and failed commits");
}
