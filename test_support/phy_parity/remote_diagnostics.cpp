#include <RemoteKissRadio.h>
#include <cassert>
#include <cstdio>

unsigned long millis() { return 1234; }
int main() {
  uint8_t bytes[107];
  memset(bytes, 0xa5, sizeof(bytes));
  assert(!RemoteKissDiagnostics::format(bytes + 1, 104));
  for (uint8_t byte : bytes) assert(byte == 0xa5);
  RemoteKissDiagnostics::record(RemoteKissDiagnostics::Offline, "Transport write fault",
                                UINT32_MAX, 42, 244);
  const auto size = RemoteKissDiagnostics::format(bytes + 1, 105);
  assert(size == 55 && bytes[0] == 0xa5 && bytes[106] == 0xa5);
  assert(queued_tx::get32(bytes + 22) == UINT32_MAX);
#ifdef ENABLE_USB_INTERFACE
  assert(Serial.output.empty());
#else
  std::string text(Serial.output.begin(), Serial.output.end());
  assert(text.find("Transport write fault [code=2 a=4294967295 b=42 c=244]") == 0);
#endif
  puts("Bounded diagnostic formatting and USB/console routing passed");
}
