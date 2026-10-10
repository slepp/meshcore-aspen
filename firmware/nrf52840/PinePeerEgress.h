// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PinePeerLink.h"
#include "SharedRadio.h"
#include "onchip/NativePacketContracts.h"

namespace nrfmast {
class PeerEgress {
  SharedRadio &radio_;
  RadioPort &port_;
  peer::Link &link_;
  struct Job {
    uint8_t bytes[peer::PacketCapacity]{};
    uint16_t length = 0;
    uint32_t id = 0, due = 0, expiry = 0;
    bool sending = false;
  } jobs_[peer::Slots];
  uint32_t nextJob_ = 0;
  void complete(Job &job, uint8_t state, uint8_t reason, bool known) {
    link_.complete(job.id, {0, known ? radio_.lastTransmitMs() : 0, state, reason, known});
    if (state == queued_tx::SUCCEEDED) ++succeeded;
    if (state == queued_tx::UNKNOWN) ++uncertain;
    job = {};
  }
public:
  static bool relayAllowed(const mesh::PacketEngineInfo &info) {
    return info.stage == mesh::PacketEngineStage::Relay && !info.local &&
           !info.origin.engine && !info.origin.reflection;
  }
  uint32_t succeeded = 0, uncertain = 0;
  PeerEgress(SharedRadio &radio, RadioPort &port, peer::Link &link)
      : radio_(radio), port_(port), link_(link) {}
  uint8_t admit(const uint8_t *bytes, uint16_t length, uint8_t origin,
                uint32_t delay, uint32_t lifetime, uint32_t now, uint32_t &job) {
    if (origin || !onchip::validNativeWire(bytes, length) || !lifetime ||
        lifetime > peer::MaxLifetimeMs || delay >= lifetime) return queued_tx::INVALID;
    for (auto &slot : jobs_) if (!slot.id) {
      memcpy(slot.bytes, bytes, length); slot.length = length;
      do { ++nextJob_; } while (!nextJob_);
      job = slot.id = nextJob_; slot.due = now + delay; slot.expiry = now + lifetime;
      return queued_tx::NONE;
    }
    return queued_tx::FULL;
  }
  void disconnect() {
    for (auto &job : jobs_) if (!job.sending) job = {};
  }
  void poll(uint32_t now) {
    for (auto &job : jobs_) if (job.id) {
      if (job.sending) {
        if (port_.transmissionFailed()) {
          port_.onSendFinished(); complete(job, queued_tx::UNKNOWN, queued_tx::RF_TIMEOUT, false);
        } else if (port_.isSendComplete()) {
          port_.onSendFinished(); complete(job, queued_tx::SUCCEEDED, queued_tx::NONE, true);
        }
      } else if (int32_t(now - job.expiry) >= 0)
        complete(job, queued_tx::FAILED, queued_tx::EXPIRED, false);
      else if (link_.ready() && int32_t(now - job.due) >= 0 && port_.startSendRaw(job.bytes, job.length))
        job.sending = true;
    }
  }
};
static_assert(sizeof(PeerEgress) <= 640, "Pine peer egress exceeds its 640-byte budget");
}
