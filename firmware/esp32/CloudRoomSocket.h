// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace onchip {
struct CloudRoomPeer {
  const char *host = nullptr, *address = nullptr, *ca = nullptr;
  const char *token = nullptr, *alias = nullptr;
};
// Network task only. Owns one WSS connection; no application pings, automatic
// operation replay, periodic requests or radio access. Caller owns frame buffers.
class CloudRoomSocket {
public:
  virtual ~CloudRoomSocket() = default;
  virtual bool open(const CloudRoomPeer &,char *error,size_t capacity) = 0;
  // The IDF masking implementation temporarily writes into this buffer.
  virtual bool send(char *json,size_t size) = 0;
  // >0 complete JSON frame bytes, 0 no data/control handled, -1 disconnected.
  virtual int receive(char *frame,size_t capacity) = 0;
  virtual void close() = 0;
};
CloudRoomSocket *createCloudRoomSocket();
} // namespace onchip
