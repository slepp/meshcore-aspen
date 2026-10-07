// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CloudRoomSocket.h"
#include "cloudroom/RadioBridge.h"

class WifiKissMultiplexer;
namespace onchip {
// Chosen codec/trust implementation supplies this interface. All callbacks run
// on the network task. Config/public keys remain immutable for its lifetime.
// No local history, membership replica or credentials are supplied by default.
class CloudRoomDriver {
public:
  virtual ~CloudRoomDriver() = default;
  virtual unsigned aliases() const = 0;
  virtual const CloudRoomPeer &peer(unsigned alias) const = 0;
  virtual const uint8_t *publicKey(unsigned alias) const = 0;
  virtual void opened(unsigned alias, uint32_t generation) = 0;
  virtual void disconnected(unsigned alias) = 0;
  virtual void received(const cloudroom::Reception &) = 0;
  // Reject malformed/version-mismatched API frames by returning false.
  virtual bool frame(unsigned alias, const char *, size_t) = 0;
  virtual void receipt(const cloudroom::Receipt &) = 0;
  // Return a single next operation; socket loss must not replay it automatically.
  virtual size_t operation(unsigned alias, char *, size_t capacity) = 0;
};
// Optional private implementation. The weak default returns nullptr: even the
// opt-in compile profile cannot connect, advertise or import keys on its own.
CloudRoomDriver *createCloudRoomDriver(cloudroom::RadioBridge &);
bool beginCloudRoom(WifiKissMultiplexer &);
void loopCloudRoom(); // Dispatch task only, like LocalRadio.
unsigned cloudRoomAliases();
const uint8_t *cloudRoomPublicKey(unsigned alias);
} // namespace onchip
