#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

#include "RemoteKissRadio.h"
#ifdef PHY_NATIVE_SCORE_VECTORS
#include "native_scores.inc"
#endif

static unsigned long now_ms;
unsigned long millis() { return now_ms; }

class FakeStream : public Stream {
public:
  std::deque<uint8_t> input;
  std::vector<uint8_t> output;

  int available() override { return static_cast<int>(input.size()); }
  int read() override {
    if (input.empty()) return -1;
    const uint8_t byte = input.front();
    input.pop_front();
    return byte;
  }
  std::size_t write(uint8_t byte) override {
    output.push_back(byte);
    return 1;
  }
  std::size_t write(const uint8_t* data, std::size_t length) override {
    output.insert(output.end(), data, data + length);
    return length;
  }
  void receive(std::initializer_list<uint8_t> bytes) {
    input.insert(input.end(), bytes.begin(), bytes.end());
  }
};

int main() {
  FakeStream link;
  RemoteKissRadio radio(link, RemoteKissRadio::Mode::Legacy);
  radio.setParams(910.525f, 62.5f, 7, 5);
  radio.begin();
  assert(radio.getEstAirtimeFor(149) == 542);
  assert(radio.packetScore(-8, 32) == 0);
  assert(std::fabs(radio.packetScore(-2.5, 128) - 0.25) < 0.0001);

  link.receive({0xC0, 0x00, 'a', 'b', 'c', 0xC0,
                0xC0, 0x06, 0xF9, 0x32, 0xEC, 0xC0});
  uint8_t packet[255];
  assert(radio.recvRaw(packet, sizeof(packet)) == 3);
  assert(packet[0] == 'a' && packet[2] == 'c');
  assert(std::fabs(radio.getLastSNR() - 12.5f) < 0.01f);
  assert(radio.getLastRSSI() == -20.0f);

  link.output.clear();
  const uint8_t tx[] = {0x01, 0xC0, 0xDB};
  assert(radio.startSendRaw(tx, sizeof(tx)));
  assert(!radio.isSendComplete());
  const std::vector<uint8_t> expected = {
      0xC0, 0x00, 0x01, 0xDB, 0xDC, 0xDB, 0xDD, 0xC0};
  assert(link.output == expected);

  link.receive({0xC0, 0x06, 0xF8, 0x01, 0xC0});
  assert(radio.isSendComplete());
  radio.onSendFinished();
  assert(!radio.isSendComplete());
  assert(radio.startSendRaw(tx, sizeof(tx)));
  link.receive({0xC0, 0x06, 0xF8, 0x00, 0xC0});
  assert(!radio.isSendComplete());
  radio.onSendFinished();
  assert(radio.startSendRaw(tx, sizeof(tx)));
  radio.onSendFinished();
  link.receive({0xC0, 0x06, 0xF8, 0x01, 0xC0});
  assert(!radio.isSendComplete());
  assert(!radio.startSendRaw(tx, sizeof(tx)));
  radio.onLinkConnected();
  assert(radio.startSendRaw(tx, sizeof(tx)));
#ifdef PHY_NATIVE_SCORE_VECTORS
  FakeStream score_link;
  RemoteKissRadio score_radio(score_link, RemoteKissRadio::Mode::Legacy);
  for (const auto& vector : native_scores) {
    score_radio.setParams(910.525f, 62.5f, vector.sf, 5);
    assert(std::fabs(score_radio.packetScore(vector.snr, vector.length) -
                     vector.score) < 0.000001f);
  }
  printf("RemoteKissRadio matches %zu native extracted-kernel score vectors\n",
         sizeof(native_scores) / sizeof(native_scores[0]));
#endif
  return 0;
}
