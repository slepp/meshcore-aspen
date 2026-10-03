#pragma once

#include <Dispatcher.h>

namespace nrfmast {

class SharedRadio;

class RadioPort : public mesh::Radio {
  friend class SharedRadio;
  struct Frame {
    uint8_t bytes[MAX_TRANS_UNIT];
    uint16_t length = 0;
    float rssi = 0, snr = 0;
  };
  SharedRadio& shared;
  const uint8_t index;
  Frame frames[2];
  uint8_t head = 0, count = 0;
  float lastRssi = 0, lastSnr = 0;
  bool failedSend = false;
  void finishSend();

public:
  uint32_t rxDrops = 0;
  uint32_t transmissionsStarted = 0;
  RadioPort(SharedRadio& owner, uint8_t id) : shared(owner), index(id) {}
  void begin() override;
  int recvRaw(uint8_t* bytes, int capacity) override;
  uint32_t getEstAirtimeFor(int bytes) override;
  float packetScore(float snr, int bytes) override;
  bool startSendRaw(const uint8_t* bytes, int length) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isReceiving() override;
  float getLastRSSI() const override { return lastRssi; }
  float getLastSNR() const override { return lastSnr; }
  int getNoiseFloor() const override;
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enabled) override;
  void resetAGC() override;
  void abortSend();
  bool transmissionFailed() const { return failedSend; }
  uint32_t receivedAirtimeMs() const;
#if NRFMAST_PRODUCTION_LUA
  void aggregateStats(mesh::QueuedRadioStats& value) const;
#endif
};

// Only this object touches the physical driver. MeshCore retains packet queues,
// routing, CAD, encryption and retransmission; each port has two RX snapshots.
class SharedRadio {
  friend class RadioPort;
  mesh::Radio& physical;
  mesh::MillisecondClock& clock;
  const bool transmitEnabled;
  bool begun = false;
  int owner = -1;
  uint32_t started = 0, quietUntil = 0, timeoutAt = 0, receivedMs = 0;
  float airtimeFactor = 1;
  uint32_t confirmedRfMs = 0, lastTransmitAirtime = 0;
  bool lastTransmitKnown = false, completionLatched = false;
  const mesh::PacketManager* repeaterQueue = nullptr;
  const mesh::PacketManager* botQueue = nullptr;

public:
  RadioPort repeater, bot;
#if NRFMAST_PRODUCTION_LUA
  RadioPort lua;
#endif
  uint32_t txStarted = 0, txRejected = 0;
  uint32_t rfMs = 0, txSucceeded = 0, txFailed = 0;
  uint16_t otherQueued = 0;
  SharedRadio(mesh::Radio& radio, mesh::MillisecondClock& ms, bool enabled)
      : physical(radio), clock(ms), transmitEnabled(enabled),
        repeater(*this, 0), bot(*this, 1)
#if NRFMAST_PRODUCTION_LUA
        , lua(*this, 2)
#endif
        {}
  void begin();
  void poll();
  bool idle() const { return owner < 0; }
  bool repeaterCanLoop() const { return owner < 0 || owner == 0 || repeater.transmissionFailed(); }
  bool botCanLoop() const { return owner < 0 || owner == 1 || bot.transmissionFailed(); }
  bool setAirtimeFactor(float factor);
  float getAirtimeFactor() const { return airtimeFactor; }
  uint32_t receivedAirtimeMs() const { return receivedMs; }
  uint32_t aggregateRfMs() const { return confirmedRfMs; }
  uint32_t aggregateActiveMs() const { return rfMs; }
  uint32_t lastTransmitMs() const { return lastTransmitAirtime; }
  bool lastTransmitWasConfirmed() const { return lastTransmitKnown; }
  void observeQueues(const mesh::PacketManager& repeaterPackets, const mesh::PacketManager& botPackets) {
    repeaterQueue = &repeaterPackets; botQueue = &botPackets;
  }
  uint8_t queuedPackets() const {
    const unsigned count = repeaterQueue || botQueue ?
        (repeaterQueue ? repeaterQueue->getOutboundTotal() : 0) +
        (botQueue ? botQueue->getOutboundTotal() : 0) : otherQueued;
    return count > 255 ? 255 : count;
  }
};

}  // namespace nrfmast
