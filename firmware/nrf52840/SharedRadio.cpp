#include "SharedRadio.h"
#include <cmath>

namespace nrfmast {

void SharedRadio::begin() {
  if (!begun) {
    physical.begin();
    quietUntil = clock.getMillis();
    begun = true;
  }
}

bool SharedRadio::setAirtimeFactor(float factor) {
  if (!std::isfinite(factor) || factor < 0) return false;
  airtimeFactor = factor;
  return true;
}

void SharedRadio::poll() {
  physical.loop();
#if NRFMAST_PRODUCTION_LUA
  if (owner >= 0 && int32_t(clock.getMillis() - timeoutAt) >= 0) {
    RadioPort* ports[] = {&repeater, &bot, &lua};
    auto* port = ports[owner];
    if (!port->isSendComplete()) port->abortSend();
  }
#endif
  // recvRaw() would abort an in-flight SX1262 transmission, even on the other port.
  if (owner >= 0) return;
  RadioPort::Frame frame;
  int length = physical.recvRaw(frame.bytes, sizeof(frame.bytes));
  if (length <= 0) return;
  receivedMs += physical.getEstAirtimeFor(length);
  RadioPort* ports[] = {&repeater, &bot
#if NRFMAST_PRODUCTION_LUA
      , &lua
#endif
  };
  if (length < 2 || length > int(sizeof(frame.bytes))) {
    for (auto* port : ports) ++port->rxDrops;
    return;
  }
  const uint8_t route = frame.bytes[0] & PH_ROUTE_MASK;
  if ((route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT) && length < 6) {
    for (auto* port : ports) ++port->rxDrops;
    return;
  }
  frame.length = length;
  frame.rssi = physical.getLastRSSI();
  frame.snr = physical.getLastSNR();
  for (auto* port : ports) {
    if (port->count == 2) {
      ++port->rxDrops;
    } else {
      port->frames[(port->head + port->count) % 2] = frame;
      ++port->count;
    }
  }
}

void RadioPort::begin() { shared.begin(); }

uint32_t RadioPort::receivedAirtimeMs() const { return shared.receivedAirtimeMs(); }

int RadioPort::recvRaw(uint8_t* bytes, int capacity) {
  if (!count) return 0;
  auto& frame = frames[head];
  int length = frame.length;
  if (capacity < length) {
    ++rxDrops;
    length = 0;
  } else {
    memcpy(bytes, frame.bytes, length);
    lastRssi = frame.rssi;
    lastSnr = frame.snr;
  }
  head = (head + 1) % 2;
  --count;
  return length;
}

uint32_t RadioPort::getEstAirtimeFor(int bytes) {
  return shared.physical.getEstAirtimeFor(bytes);
}
float RadioPort::packetScore(float snr, int bytes) {
  return shared.physical.packetScore(snr, bytes);
}
bool RadioPort::startSendRaw(const uint8_t* bytes, int length) {
  if ((index < 2 && failedSend) || !shared.transmitEnabled || shared.owner >= 0 ||
      int32_t(uint32_t(shared.clock.getMillis()) - shared.quietUntil) < 0 ||
      length <= 0 || length > MAX_TRANS_UNIT) {
    ++shared.txRejected;
    return false;
  }
  if (!shared.physical.startSendRaw(bytes, length)) {
    ++shared.txRejected;
    return false;
  }
  shared.owner = index;
  shared.completionLatched = false;
  shared.started = shared.clock.getMillis();
  shared.timeoutAt = shared.started + shared.physical.getEstAirtimeFor(length) * 2 + 2000;
  failedSend = false;
  ++shared.txStarted;
  ++transmissionsStarted;
  return true;
}
bool RadioPort::isSendComplete() {
  // Native Dispatcher interprets true as success; only Lua checks the failure flag.
  if (failedSend) {
#if NRFMAST_PRODUCTION_LUA
    return index == 2;
#else
    return false;
#endif
  }
  if (shared.owner != index) return false;
  if (!shared.completionLatched)
    shared.completionLatched = shared.physical.isSendComplete();
  return shared.completionLatched;
}
void RadioPort::onSendFinished() {
  if (failedSend) {
    failedSend = false;
    return;
  }
  finishSend();
}
void RadioPort::finishSend() {
  if (shared.owner != index) return;
  const bool complete = shared.completionLatched;
  shared.lastTransmitKnown = complete && !failedSend;
  shared.physical.onSendFinished();
  uint32_t now = shared.clock.getMillis();
  shared.lastTransmitAirtime = uint32_t(now - shared.started);
  shared.rfMs += shared.lastTransmitAirtime;
  if (shared.lastTransmitKnown) shared.confirmedRfMs += shared.lastTransmitAirtime;
  if (complete && !failedSend) ++shared.txSucceeded;
  else ++shared.txFailed;
  double quiet = double(uint32_t(now - shared.started)) * shared.airtimeFactor;
  if (quiet > 0x7FFFFFFF) quiet = 0x7FFFFFFF;
  shared.quietUntil = now + uint32_t(quiet);
  shared.owner = -1;
  shared.completionLatched = false;
}
void RadioPort::abortSend() {
  if (shared.owner != index) return;
  failedSend = true;
  finishSend();
}
#if NRFMAST_PRODUCTION_LUA
void RadioPort::aggregateStats(mesh::QueuedRadioStats& value) const {
  value.aggregate_rf_ms = shared.aggregateRfMs();
  value.aggregate_successes = shared.txSucceeded;
  value.aggregate_failures = shared.txFailed;
  value.aggregate_queued = shared.queuedPackets();
  value.aggregate_transmitting = shared.owner >= 0;
}
#endif
bool RadioPort::isReceiving() {
  return shared.owner >= 0 ||
         int32_t(uint32_t(shared.clock.getMillis()) - shared.quietUntil) < 0 ||
         shared.physical.isReceiving();
}
bool RadioPort::isInRecvMode() const { return shared.physical.isInRecvMode(); }
int RadioPort::getNoiseFloor() const { return shared.physical.getNoiseFloor(); }
void RadioPort::triggerNoiseFloorCalibrate(int threshold) {
  if (index == 0 && shared.idle()) shared.physical.triggerNoiseFloorCalibrate(threshold);
}
void RadioPort::setCADEnabled(bool enabled) {
  if (index == 0 && shared.idle()) shared.physical.setCADEnabled(enabled);
}
void RadioPort::resetAGC() {
  if (index == 0 && shared.idle()) shared.physical.resetAGC();
}

}  // namespace nrfmast
