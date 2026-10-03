#pragma once

#include <cstdint>

namespace mesh {
class Radio {
public:
  virtual ~Radio() = default;
  virtual void begin() {}
  virtual int recvRaw(uint8_t*, int) = 0;
  virtual uint32_t getEstAirtimeFor(int) = 0;
  virtual float packetScore(float, int) = 0;
  virtual bool startSendRaw(const uint8_t*, int) = 0;
  virtual bool isSendComplete() = 0;
  virtual void onSendFinished() = 0;
  virtual void loop() {}
  virtual int getNoiseFloor() const { return 0; }
  virtual bool isInRecvMode() const = 0;
  virtual bool isReceiving() { return false; }
  virtual float getLastRSSI() const { return 0; }
  virtual float getLastSNR() const { return 0; }
};
}
