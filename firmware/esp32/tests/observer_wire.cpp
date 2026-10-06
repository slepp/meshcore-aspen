// SPDX-License-Identifier: Apache-2.0
#include "../ObserverWire.h"
#include <ed_25519.h>
#include <base64.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc, char **argv) {
  assert(argc == 3);
  const auto epoch = uint32_t(strtoul(argv[2], nullptr, 10));
  uint8_t seed[32]{1}, expanded[64], publicKey[32];
  ed25519_create_keypair(publicKey, expanded, seed);
  mesh::LocalIdentity identity;
  identity.readFrom(expanded, sizeof(expanded));
  char key[65];
  for (unsigned i = 0; i < 32; ++i)
    snprintf(key + 2 * i, 3, "%02X", identity.pub_key[i]);
  char token[1024], output[2048];
  using namespace onchip::observerWire;
  assert(!onchip::observerWire::token(identity, "internal-observer-test", 0,
                                     token, sizeof(token)));
  assert(onchip::observerWire::token(identity, "internal-observer-test", epoch,
                                    token, sizeof(token)));
  std::ofstream(std::string(argv[1]) + "/token.json") <<
      "{\"public_key\":\"" << key << "\",\"token\":\"" << token << "\"}\n";
  const uint8_t raw[] = {0x17, 1, 2, 3, 4, 0x82, 0xaa, 0xbb, 0xcc,
                         0x12, 0x34, 0x56, 0x51, 0x52, 0x53};
  mesh::Packet decoded;
  assert(decode(raw, sizeof(raw), decoded));
  assert(decoded.payload_len == 3 && decoded.getPathHashSize() == 3);
  for (size_t length = 0; length < 13; ++length)
    assert(!decode(raw, length, decoded));
  for (const auto &invalid : {std::string("\x15\xc0\x01", 3),
                             std::string("\x15\x96\x01", 3),
                             std::string("\x55\x00\x01", 3)})
    assert(!decode(reinterpret_cast<const uint8_t *>(invalid.data()),
                   invalid.size(), decoded));
  assert(!packet(raw, sizeof(raw), "test", key, 0, -90, 4.5, 0xffff,
                 output, sizeof(output)));
  assert(!packet(raw, sizeof(raw), "test", key, epoch, 127, -32, 0xffff,
                 output, sizeof(output)));
  assert(!packet(raw, sizeof(raw), "test", key, epoch, -90, NAN, 0xffff,
                 output, sizeof(output)));
  assert(!packet(raw, sizeof(raw), "test", key, epoch, -90, 4.5, 0,
                 output, sizeof(output)));
  assert(!packet(raw, sizeof(raw), "test", key, epoch, -90, 4.5, 0xffff,
                 output, 16));
  assert(packet(raw, sizeof(raw), "Observer \"test\"", key, epoch, -90, 4.5,
                0xffff, output, sizeof(output)));
  assert(std::string(output).find("\"route\":\"D\"") != std::string::npos);
  std::ofstream(std::string(argv[1]) + "/packet.json") << output << "\n";
  assert(packet(raw, sizeof(raw), "test", key, epoch, -90, 4.25,
                0xffff, output, sizeof(output), true));
  assert(std::string(output).find("\"route\":\"T\"") != std::string::npos);
  assert(std::string(output).find("\"path\"") == std::string::npos);
  assert(std::string(output).find("\"SNR\":\"4.25\"") != std::string::npos);
  std::ofstream(std::string(argv[1]) + "/capture.json") << output << "\n";
  const uint8_t direct[] = {0x16, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 1};
  assert(packet(direct, sizeof(direct), "test", key, epoch, -90, 4.25,
                0xffff, output, sizeof(output)));
  assert(std::string(output).find("\"path\":[\"aabbcc\",\"123456\"]") != std::string::npos);
  const uint8_t flood[] = {0x15, 0x82, 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56, 1};
  assert(packet(flood, sizeof(flood), "test", key, epoch, -90, 4.25,
                0xffff, output, sizeof(output)));
  assert(std::string(output).find("\"route\":\"F\"") != std::string::npos);
  assert(std::string(output).find("\"path\"") == std::string::npos);
  std::ofstream(std::string(argv[1]) + "/flood.json") << output << "\n";
  assert(packet(direct, sizeof(direct), "test", key, epoch, -90, 4.25,
                0xffff, output, sizeof(output), true));
  assert(std::string(output).find("\"path\":\"aabbcc,123456\"") != std::string::npos);
  std::ofstream(std::string(argv[1]) + "/capture-direct.json") << output << "\n";
  const uint8_t directZero[] = {0x16, 0, 1};
  assert(packet(directZero, sizeof(directZero), "test", key, epoch, -90, 4,
                0xffff, output, sizeof(output), true));
  assert(std::string(output).find("\"path\":\"\"") != std::string::npos);
  assert(std::string(output).find("\"SNR\":\"4.0\"") != std::string::npos);
  std::ofstream(std::string(argv[1]) + "/capture-zero.json") << output << "\n";
  const uint8_t trace[] = {0x25, 0x81, 0xaa, 0xbb, 0xcc, 0x01, 0x02};
  assert(packet(trace, sizeof(trace), "test", key, epoch, -90, 4.5, 0xffff,
                output, sizeof(output)));
  assert(std::string(output).find("\"path\"") == std::string::npos);
  std::ofstream(std::string(argv[1]) + "/trace.json") << output << "\n";
  assert(status("offline", "test", key, "host test", "test", "unknown",
                epoch, output, sizeof(output)));
  std::ofstream(std::string(argv[1]) + "/status.json") << output << "\n";
  puts("PASS observer wire: transport, 3-byte path, filter, UTC, RF reflection, bounds and upstream signer/hash");
}
