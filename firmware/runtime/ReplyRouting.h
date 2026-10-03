// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Config.h"
#include <Mesh.h>
#include <helpers/RoutingPolicy.h>
#include <helpers/TransportKeyStore.h>

namespace onchip {
struct PeerRoute {
  uint8_t bytes[MAX_PATH_SIZE]{}, length = 0xff;
  bool scoped = false;
  void learn(const uint8_t *path, uint8_t size) {
    length = mesh::Packet::copyPath(bytes, path, size);
  }
};

class ReplyRouting {
  TransportKey key_{};
public:
  ReplyRouting() {
    if (ONCHIP_SERVICE_REGION[0]) {
      TransportKeyStore keys;
      keys.getAutoKeyFor(1, ONCHIP_SERVICE_REGION, key_);
    }
  }
  bool scope(const mesh::Packet *request) const {
    bool matched = false;
    if (request->hasTransportCodes() && !key_.isNull()) {
      const auto code = key_.calcTransportCode(request);
      matched = request->transport_codes[0] == code || request->transport_codes[1] == code;
    }
    return mesh::chooseReplyScope(matched, request->getRouteType() == ROUTE_TYPE_FLOOD,
                                  !key_.isNull()) != mesh::REPLY_SCOPE_NONE;
  }
  void flood(mesh::Mesh &mesh, mesh::Packet *packet, bool scoped,
             uint8_t width, uint32_t delay = 0) const {
    if (scoped && !key_.isNull()) {
      uint16_t codes[2];
      codes[0] = codes[1] = key_.calcTransportCode(packet);
      mesh.sendFlood(packet, codes, delay, width);
    } else mesh.sendFlood(packet, delay, width);
  }
  void send(mesh::Mesh &mesh, mesh::Packet *packet, const PeerRoute &route,
            uint8_t width, uint32_t delay = 0) const {
    if (mesh::chooseReplyRoute(false, false, route.length != 0xff) == mesh::REPLY_ROUTE_DIRECT_OUT_PATH)
      mesh.sendDirect(packet, route.bytes, route.length, delay);
    else flood(mesh, packet, route.scoped, width, delay);
  }
};
} // namespace onchip
