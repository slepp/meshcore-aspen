// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CloudRoomSocket.h"
#include "NativeServices.h"
#include "cloudroom/RadioBridge.h"
#include "cloudroom/OpaqueFrontend.h"

class WifiKissMultiplexer;
namespace onchip {
// Opaque transport implementation supplies this interface. All callbacks run
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
  virtual bool advertise(unsigned alias) { (void)alias; return false; }
  // Return a single next operation; socket loss must not replay it automatically.
  virtual size_t operation(unsigned alias, char *, size_t capacity) = 0;
};
// The built-in opaque driver uses saved settings or a custom configuration provider.
CloudRoomDriver *createCloudRoomDriver(cloudroom::RadioBridge &);
struct CloudRoomConfiguration {
  unsigned count = 0;
  CloudRoomPeer peers[cloudroom::AliasLimit];
  cloudroom::OpaqueAlias aliases[cloudroom::AliasLimit];
};
// The generic image loads a private saved profile at boot. Custom applications
// may supply their own immutable provider. nullptr disables the service.
const CloudRoomConfiguration *cloudRoomConfiguration();
bool beginCloudRoom(WifiKissMultiplexer &,NativeNetworkHost &);
void loopCloudRoom(); // Dispatch task only, like LocalRadio.
unsigned cloudRoomAliases();
const uint8_t *cloudRoomPublicKey(unsigned alias);
bool requestCloudRoomAdvertisement(unsigned alias); // Explicit operator action.
void cloudRoomCommand(const char *command, char *reply, size_t capacity);
} // namespace onchip
