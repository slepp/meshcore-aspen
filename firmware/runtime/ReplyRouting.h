// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Config.h"
#include <Mesh.h>
#include <helpers/RoutingPolicy.h>
#include <helpers/TransportKeyStore.h>
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include "Scopes.h"
#endif

namespace onchip {
struct PeerRoute {
  uint8_t bytes[MAX_PATH_SIZE]{}, length = 0xff;
  uint8_t scoped = 0;
  void learn(const uint8_t *path, uint8_t size) {
    length = mesh::Packet::copyPath(bytes, path, size);
  }
};

class ReplyRouting {
  TransportKey key_{};
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  TransportKey home_{};
#endif
public:
  ReplyRouting() {
    if (ONCHIP_SERVICE_REGION[0]) {
      TransportKeyStore keys;
      keys.getAutoKeyFor(1, ONCHIP_SERVICE_REGION, key_);
    }
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    if (nativeHostScopes.configured) {
      key_ = nativeHostScopes.fallback;
      home_ = nativeHostScopes.home;
    }
#endif
  }
  uint8_t defaultScope() const { return key_.isNull() ? 0 : 1; }
  uint8_t scope(const mesh::Packet *request) const {
    uint8_t matched = 0;
    if (request->hasTransportCodes() && !key_.isNull()) {
      const auto code = key_.calcTransportCode(request);
      if (request->transport_codes[0] == code || request->transport_codes[1] == code) matched = 1;
    }
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    if (!matched && request->hasTransportCodes() && !home_.isNull()) {
      const auto code = home_.calcTransportCode(request);
      if (request->transport_codes[0] == code || request->transport_codes[1] == code) matched = 2;
    }
#endif
    const auto choice = mesh::chooseReplyScope(matched != 0, request->getRouteType() == ROUTE_TYPE_FLOOD,
                                               !key_.isNull());
    if (choice == mesh::REPLY_SCOPE_REQUEST) return matched;
    return choice == mesh::REPLY_SCOPE_DEFAULT ? 1 : 0;
  }
  void flood(mesh::Mesh &mesh, mesh::Packet *packet, uint8_t scoped,
             uint8_t width, uint32_t delay = 0) const {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    const TransportKey &key = scoped == 2 ? home_ : key_;
#else
    const TransportKey &key = key_;
#endif
    if (scoped && !key.isNull()) {
      uint16_t codes[2];
      codes[0] = codes[1] = key.calcTransportCode(packet);
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
