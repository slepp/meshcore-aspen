// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PinePeerDevice.h"
#include "PinePeerLink.h"
#include "PinePeerEgress.h"
#include "PineRoom.h"
#include "PinePacketPrograms.h"
#include "onchip/NativePacketContracts.h"
#include <InternalFileSystem.h>
#include <helpers/IdentityStore.h>
#include <bluefruit.h>
#include <utility/debug.h>
#include <atomic>
#include <algorithm>
#include <initializer_list>
#include <new>

using namespace Adafruit_LittleFS_Namespace;
namespace nrfmast {
namespace {
struct Configuration {
  uint8_t version = 1, enabled = 0, central = 0, addressType = 0;
  uint8_t address[6]{};
  uint8_t reserved[2]{};
  uint32_t pin = 0;
  uint8_t digest[32]{};
};
Configuration configuration;
uint8_t pendingPrivate[64]{};
bool privatePending;
bool readConfiguration() {
  configuration = {};
  if (!InternalFS.exists("/peer-config")) return true;
  auto file = InternalFS.open("/peer-config");
  if (!file || file.size() != sizeof(configuration) ||
      file.read(reinterpret_cast<uint8_t *>(&configuration), sizeof(configuration)) != sizeof(configuration)) return false;
  uint8_t hash[32];
  mesh::Utils::sha256(hash, 32, reinterpret_cast<uint8_t *>(&configuration), offsetof(Configuration, digest));
  return configuration.version == 1 && configuration.enabled <= 1 && configuration.central <= 1 &&
         configuration.addressType <= 1 && !configuration.reserved[0] && !configuration.reserved[1] &&
         !memcmp(hash, configuration.digest, 32);
}
bool saveConfiguration(Configuration &next) {
  mesh::Utils::sha256(next.digest, 32, reinterpret_cast<uint8_t *>(&next), offsetof(Configuration, digest));
  InternalFS.remove("/peer-config.pending");
  auto file = InternalFS.open("/peer-config.pending", FILE_O_WRITE);
  if (!file || file.write(reinterpret_cast<uint8_t *>(&next), sizeof(next)) != sizeof(next)) return false;
  file.close();
  file = InternalFS.open("/peer-config.pending");
  Configuration check;
  if (!file || file.size() != sizeof(check) ||
      file.read(reinterpret_cast<uint8_t *>(&check), sizeof(check)) != sizeof(check) ||
      memcmp(&check, &next, sizeof(next))) return false;
  file.close();
  if (!InternalFS.rename("/peer-config.pending", "/peer-config")) return false;
  configuration = next; return true;
}
bool hex(const char *text, uint8_t *bytes, unsigned size) {
  if (strlen(text) != size * 2) return false;
  for (unsigned i = 0; i < size; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      char c = text[i * 2 + j];
      unsigned nibble = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
      if (nibble == 16) return false;
      value = value * 16 + nibble;
    }
    bytes[i] = value;
  }
  return true;
}
struct Device final : peer::Host {
  SharedRadio &radio;
  RadioPort port;
  peer::Link link{*this};
  PeerEgress egress{radio, port, link};
  BLEUart peripheral{512};
  BLEClientUart central{512};
  mesh::LocalIdentity identity;
  struct Encoded { uint8_t bytes[peer::EncodedCapacity]; uint16_t length = 0, offset = 0; } output[2];
  uint32_t reconnectAt = 0, connectedAt = 0;
  uint32_t offers = 0, rejected = 0;
  uint32_t remoteSucceeded = 0, remoteUncertain = 0;
  std::atomic<uint16_t> connection{BLE_CONN_HANDLE_INVALID};
  uint16_t observedConnection = BLE_CONN_HANDLE_INVALID;
  std::atomic<bool> discovered{false};
  explicit Device(SharedRadio &r) : radio(r), port(r, 5) { radio.peer = &port; }
  ~Device() { radio.peer = nullptr; }
  bool pinned(uint16_t handle) {
    auto *conn = Bluefruit.Connection(handle);
    if (!conn) return false;
    const auto address = conn->getPeerAddr();
    return address.addr_type == configuration.addressType && !memcmp(address.addr, configuration.address, 6);
  }
  bool authenticated() {
    const uint16_t handle = connection.load();
    ble_gap_conn_sec_t security{};
    return handle != BLE_CONN_HANDLE_INVALID && pinned(handle) &&
           sd_ble_gap_conn_sec_get(handle, &security) == NRF_SUCCESS &&
           security.sec_mode.sm == 1 && security.sec_mode.lv >= 3;
  }
  bool write(const uint8_t *bytes, uint16_t length) override {
    if (!authenticated() || length > peer::EncodedCapacity) return false;
    for (auto &frame : output) if (!frame.length) {
      memcpy(frame.bytes, bytes, length); frame.length = length; frame.offset = 0; return true;
    }
    return false;
  }
  uint8_t transmit(const uint8_t *bytes, uint16_t length, uint8_t origin,
                   uint32_t delay, uint32_t lifetime, uint32_t &job) override {
    return egress.admit(bytes, length, origin, delay, lifetime, millis(), job);
  }
  void closed(const char *) override {
    for (auto &frame : output) frame = {};
    egress.disconnect();
    const auto handle = observedConnection;
    if (handle != BLE_CONN_HANDLE_INVALID) {
      auto *conn = Bluefruit.Connection(handle);
      if (conn) conn->disconnect();
    }
  }
  void loop() {
    const uint16_t handle = connection.load();
    if (handle != observedConnection) {
      if (link.active()) link.disconnect();
      observedConnection = handle; connectedAt = millis();
    }
    if (handle == BLE_CONN_HANDLE_INVALID) {
      if (link.active()) link.disconnect();
      if (configuration.central && int32_t(millis() - reconnectAt) >= 0) {
        reconnectAt = millis() + 5000;
        ble_gap_addr_t address{}; address.addr_type = configuration.addressType;
        memcpy(address.addr, configuration.address, 6);
        Bluefruit.Central.connect(&address);
      }
    } else if (!authenticated()) {
      if (uint32_t(millis() - connectedAt) >= 10000) closed("peer pairing timeout");
    } else {
      if ((configuration.central && !discovered) || (!configuration.central && !peripheral.notifyEnabled(handle))) {
        if (uint32_t(millis() - connectedAt) >= 10000) closed("peer UART discovery timeout");
        return;
      }
      if (!link.active()) {
        uint32_t nonce = 0;
        if (sd_rand_application_vector_get(reinterpret_cast<uint8_t *>(&nonce), sizeof(nonce)) != NRF_SUCCESS || !nonce) return;
        link.begin(nonce, identity.pub_key, millis());
      }
      uint8_t bytes[64];
      const int available = configuration.central ? central.available() : peripheral.available();
      const int count = available > int(sizeof(bytes)) ? sizeof(bytes) : available;
      if (count > 0) {
        const int received = configuration.central ? central.read(bytes, count) : peripheral.read(bytes, count);
        if (received > 0) link.feed(bytes, received, millis());
      }
      link.poll(millis());
      for (auto &frame : output) if (frame.length) {
        const uint16_t count = std::min(uint16_t(20), uint16_t(frame.length - frame.offset));
        const size_t written = configuration.central ? central.write(frame.bytes + frame.offset, count) :
                                                       peripheral.write(frame.bytes + frame.offset, count);
        if (written != count) { link.disconnect(); return; }
        frame.offset += written;
        if (frame.offset == frame.length) { output[0] = output[1]; output[1] = {}; }
        break;
      }
    }
    peer::Result result;
    for (unsigned count = 0; count < peer::Slots * 2 && link.result(result); ++count) {
      if (result.state == queued_tx::SUCCEEDED) ++remoteSucceeded;
      if (result.state == queued_tx::UNKNOWN) ++remoteUncertain;
    }
    egress.poll(millis());
  }
};
Device *device;
void connect(uint16_t handle) {
  if (!device || !device->pinned(handle)) { if (auto *conn = Bluefruit.Connection(handle)) conn->disconnect(); return; }
  device->discovered.store(false);
  device->connection.store(handle);
  Bluefruit.Connection(handle)->requestPairing();
}
void secured(uint16_t handle) {
  if (!device || device->connection.load() != handle || !device->authenticated()) return;
  const bool ready = !configuration.central || (device->central.discover(handle) && device->central.enableTXD());
  if (device->connection.load() == handle) device->discovered.store(ready);
}
void disconnect(uint16_t handle, uint8_t) {
  if (device && device->connection.load() == handle) device->connection.store(BLE_CONN_HANDLE_INVALID);
}
void passkey(uint16_t handle, uint8_t pin[6]) {
  if (!device || !device->pinned(handle)) { memset(pin, 0, 6); return; }
  char text[7]; snprintf(text, sizeof(text), "%06lu", (unsigned long)configuration.pin);
  memcpy(pin, text, 6);
}
}
bool peerDeviceSelected() {
  if (!readConfiguration()) return true;
  return configuration.enabled != 0;
}
bool beginPeerDevice(SharedRadio &radio, mesh::LocalIdentity &repeater) {
  static_assert(sizeof(Device) <= 8192, "Pine peer device exceeds its 8KiB object budget");
  if (!readConfiguration() || !configuration.enabled || !configuration.pin ||
      dbgHeapFree() < int(sizeof(Device) + 8192)) return false;
  IdentityStore store(InternalFS, "");
  mesh::LocalIdentity virtualIdentity;
  if (!store.load("_pinepeer", virtualIdentity)) return false;
  device = new (std::nothrow) Device(radio);
  if (!device) return false;
  device->identity = virtualIdentity;
  Bluefruit.configPrphBandwidth(BANDWIDTH_NORMAL);
  Bluefruit.configCentralBandwidth(BANDWIDTH_NORMAL);
  if (!Bluefruit.begin(configuration.central ? 0 : 1, configuration.central ? 1 : 0)) {
    delete device; device = nullptr; return false;
  }
  Bluefruit.setName("Pine-Peer");
  Bluefruit.Security.setMITM(true);
  Bluefruit.Security.setSecuredCallback(secured);
  if (configuration.central) {
    Bluefruit.Security.setIOCaps(false, false, true);
    Bluefruit.Security.setPairPasskeyRequestCallback(passkey);
    Bluefruit.Central.clearBonds();
    device->central.begin();
    Bluefruit.Central.setConnectCallback(connect);
    Bluefruit.Central.setDisconnectCallback(disconnect);
  } else {
    char pin[7]; snprintf(pin, sizeof(pin), "%06lu", (unsigned long)configuration.pin);
    Bluefruit.Security.setPIN(pin);
    Bluefruit.Periph.clearBonds();
    device->peripheral.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
    device->peripheral.begin();
    Bluefruit.Periph.setConnectCallback(connect);
    Bluefruit.Periph.setDisconnectCallback(disconnect);
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(device->peripheral);
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 160);
    Bluefruit.Advertising.start(0);
  }
  repeater = virtualIdentity;
  return true;
}
void peerDeviceLoop() { if (device) device->loop(); }
void peerDeviceRelay(const mesh::PacketEngineInfo &info, const uint8_t *bytes, uint16_t size) {
  if (!device || !PeerEgress::relayAllowed(info)) return;
  uint32_t job;
  if (device->link.submit(bytes, size, 0, 0, 60000, millis(), job)) ++device->offers;
  else ++device->rejected;
}
bool peerDeviceCommand(const char *input, char *reply, size_t capacity, bool local) {
  if (strncmp(input, "peer ", 5)) return false;
  input += 5;
  if (!strcmp(input, "status")) {
    snprintf(reply, capacity, "Peer saved=%u live=%u central=%u ready=%u offered=%lu rejected=%lu ok=%lu uncertain=%lu",
        peerDeviceSelected(), device != nullptr, configuration.central, device && device->link.ready(),
        device ? (unsigned long)device->offers : 0, device ? (unsigned long)device->rejected : 0,
        device ? (unsigned long)device->egress.succeeded : 0, device ? (unsigned long)device->egress.uncertain : 0);
  } else if (!strcmp(input, "address")) {
    ble_gap_addr_t address{};
    if (sd_ble_gap_addr_get(&address) != NRF_SUCCESS) snprintf(reply, capacity, "Error: BLE address unavailable; start peer or companion BLE");
    else snprintf(reply, capacity, "Peer address=%u %02X%02X%02X%02X%02X%02X", address.addr_type,
        address.addr[5], address.addr[4], address.addr[3], address.addr[2], address.addr[1], address.addr[0]);
  } else if (!local) snprintf(reply, capacity, "Error: peer configuration requires local USB");
  else if (!strcmp(input, "off")) {
    if (!readConfiguration()) configuration = {};
    auto next = configuration; next.enabled = 0;
    snprintf(reply, capacity, "%s", saveConfiguration(next) ? "Peer off saved; reboot to apply" : "Error: peer configuration save failed");
  } else if (device) snprintf(reply, capacity, "Error: peer active; save peer off and restart before changing credentials");
  else if (!strncmp(input, "key private ", 12)) {
    privatePending = hex(input + 12, pendingPrivate, 64) && mesh::LocalIdentity::validatePrivateKey(pendingPrivate);
    if (!privatePending) memset(pendingPrivate, 0, sizeof(pendingPrivate));
    snprintf(reply, capacity, "%s", privatePending ? "Peer private key staged in RAM; send public key" : "Error: peer private key invalid");
  } else if (!strncmp(input, "key public ", 11)) {
    uint8_t bytes[96]{}, signature[64];
    memcpy(bytes, pendingPrivate, 64);
    const bool valid = privatePending && hex(input + 11, bytes + 64, 32);
    memset(pendingPrivate, 0, sizeof(pendingPrivate)); privatePending = false;
    if (!valid) {
      memset(bytes, 0, sizeof(bytes));
      snprintf(reply, capacity, "Error: peer key pair invalid; stage a valid private key first");
      return true;
    }
    mesh::LocalIdentity identity; identity.readFrom(bytes, sizeof(bytes));
    identity.sign(signature, bytes + 64, 32);
    IdentityStore store(InternalFS, "");
    mesh::LocalIdentity existing;
    bool distinct = true;
    for (const char *role : {"_main", "_nrfbot", "_pineroom"})
      if (store.load(role, existing) && existing.matches(identity)) distinct = false;
    if (!valid || !identity.verify(signature, identity.pub_key, 32) || !distinct)
      snprintf(reply, capacity, "Error: peer key pair invalid or duplicates a physical role");
    else if (store.load("_pinepeer", existing) && !existing.matches(identity))
      snprintf(reply, capacity, "Error: peer identity already exists; retain it or restore its protected backup");
    else {
      const bool saved = store.save("_pinepeer", identity);
      snprintf(reply, capacity, "%s", saved && store.load("_pinepeer", existing) && existing.matches(identity) ?
               "Peer virtual identity saved; physical identities unchanged" : "Error: peer identity save failed");
    }
    memset(bytes, 0, sizeof(bytes));
  } else if (!strncmp(input, "configure ", 10)) {
    char role[12], address[13], trailing;
    unsigned type, pin;
    if (sscanf(input + 10, "%11s %u %12s %u %c", role, &type, address, &pin, &trailing) != 4 ||
        (strcmp(role, "central") && strcmp(role, "peripheral")) || type > 1 || pin < 100000 || pin > 999999)
      snprintf(reply, capacity, "Error: peer configure central|peripheral TYPE ADDRESS12 PIN6");
    else {
      Configuration next{};
      uint8_t network[6];
      if (!hex(address, network, 6)) snprintf(reply, capacity, "Error: peer address must be twelve hex digits");
      else {
        for (unsigned i = 0; i < 6; ++i) next.address[i] = network[5-i];
        next.central = !strcmp(role, "central"); next.addressType = type; next.pin = pin;
        snprintf(reply, capacity, "%s", saveConfiguration(next) ? "Peer configuration saved, disabled; peer on then reboot" : "Error: peer configuration save failed");
      }
    }
  } else if (!strcmp(input, "on")) {
    IdentityStore store(InternalFS, ""); mesh::LocalIdentity identity;
    if (roomSelected() || packetProgramsSelected())
      snprintf(reply, capacity, "Error: disable Room/packet role and restart before selecting peer mode");
    else if (!readConfiguration() || !configuration.pin || !store.load("_pinepeer", identity))
      snprintf(reply, capacity, "Error: peer key pair and configuration required");
    else {
      auto next = configuration; next.enabled = 1;
      snprintf(reply, capacity, "%s", saveConfiguration(next) ? "Peer on saved; reboot; Lua and companion pause; PHY unchanged" : "Error: peer configuration save failed");
    }
  } else snprintf(reply, capacity, "Error: peer status|address|configure|key|on|off");
  return true;
}
}
#endif
