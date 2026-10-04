// SPDX-License-Identifier: Apache-2.0
#include "TelemetryEndpoint.h"
#include <SPIFFS.h>
#include <nvs.h>
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
using namespace onchip;
unsigned long millis() { return 0; }
void delay(unsigned long) {}
struct Transport : BotHttpsTransport {
  bool trusted = true, opens = true, stall = false, shortWrite = false;
  unsigned closes = 0, connections = 0;
  uint32_t clock = 0;
  std::string sent, response = "HTTP/1.1 204 No Content\r\nX-Test: value\r\n\r\n";
  size_t offset = 0;
  bool validate(char *, size_t) override { return trusted; }
  bool open(const BotHttpsConfig &config, char *, size_t) override {
    assert(config.validPeer()); ++connections; return opens;
  }
  int write(const uint8_t *data, size_t size) override {
    if (shortWrite) size = std::min<size_t>(3, size);
    sent.append(reinterpret_cast<const char *>(data), size); return int(size);
  }
  int read(uint8_t *data, size_t size) override {
    if (stall) return 0;
    if (offset == response.size()) return -1;
    size = std::min(size, response.size() - offset);
    memcpy(data, response.data() + offset, size); offset += size; return int(size);
  }
  void close() override { ++closes; }
  uint32_t now() const override { return clock; }
  void idle() override { clock += 100; }
};
static void command(const char *text, bool good = true) {
  char reply[163];
  telemetryEndpointCommand(text, reply, sizeof(reply));
  assert((strncmp(reply, "Error:", 6) != 0) == good);
}
static void config() {
  assert(!telemetryEndpointConfigured());
  command("commit", false);
  command("address 192.0.2.1");
  command("host vm.example");
  command("path /write?precision=ns");
  command("port 443");
  command("port 99999999999999", false);
  command("ca 2d2d2d2d2d424547494e2043455254494649434154452d2d2d2d2d0a");
  command("ca 666978747572650a2d2d2d2d2d454e442043455254494649434154452d2d2d2d2d");
  command("ca 0d00", false);
  command("commit");
  TelemetryEndpoint endpoint;
  assert(telemetryEndpoint(endpoint) && endpoint.valid() && !endpoint.token[0]);
  assert(!endpoint.https().valid() && endpoint.https().validPeer()); // RPC still requires its grant/token.
  command("token 7365637265742d746f6b656e");
  command("commit");
  char reply[163];
  telemetryEndpointCommand("status", reply, sizeof(reply));
  assert(strstr(reply, "auth=1") && !strstr(reply, "secret-token"));
  assert(telemetryEndpoint(endpoint) && !strcmp(endpoint.token, "secret-token"));
  command("host bad:443");
  command("commit", false);
  assert(telemetryEndpoint(endpoint) && !strcmp(endpoint.host, "vm.example"));
  command("discard");
  command("path //other.example");
  command("commit", false);
  command("discard");
  identity_test::failCommit = true;
  command("commit", false);
  assert(!telemetryEndpointConfigured());
  identity_test::failCommit = false;
  command("commit");
  identity_test::failRead = true;
  command("commit", false);
  assert(!telemetryEndpointConfigured());
  identity_test::failRead = false;
  command("commit");
  assert(identity_test::handles.empty());
  const auto &blob = identity_test::durable.at({"mc-onchip", "telemetry-peer"});
  assert(blob.size() == 40);
  const auto &file = filesystem_test::files.at(blob[4] ? "/telemetry-b.bin" : "/telemetry-a.bin");
  assert(file.size() == sizeof(endpoint));
  memcpy(&endpoint, file.data(), sizeof(endpoint));
  assert(endpoint.valid() && !strcmp(endpoint.path, "/write?precision=ns"));
  endpoint.path[0] = '\n'; assert(!endpoint.valid());
  puts("PASS native endpoint/CA/optional credential staging, persistence, redaction, readback fencing");
}
static void transport() {
  TelemetryEndpoint endpoint;
  assert(telemetryEndpoint(endpoint));
  std::atomic<bool> cancelled{false}, stopped{false};
  const char *body = "meshcore_device,device=host-test heap_free_bytes=123i\n";
  const auto run = [&](Transport &t) {
    TelemetryCompletion result;
    performTelemetryPost(t, endpoint, body, strlen(body), 15000, cancelled, stopped, result);
    assert(t.closes == 1);
    return result;
  };
  Transport ok;
  ok.shortWrite = true;
  auto result = run(ok);
  assert(result.ok && result.httpStatus == 204);
  assert(ok.sent.find("POST /write?precision=ns HTTP/1.1\r\nHost: vm.example:443\r\n") == 0);
  assert(ok.sent.find("Authorization: Bearer secret-token\r\n") != std::string::npos);
  assert(ok.sent.find("Content-Type: text/plain\r\n") != std::string::npos);
  assert(ok.sent.substr(ok.sent.size() - strlen(body)) == body);
  for (unsigned status : {200, 202, 204, 301, 400, 401, 403, 429, 500, 503}) {
    Transport t;
    t.response = "HTTP/1.1 " + std::to_string(status) + " Result\r\nContent-Length: 0\r\n\r\n";
    result = run(t);
    assert(result.httpStatus == status && result.ok == (status < 300));
    assert(t.connections == 1); // Never follow a redirect.
  }
  for (const char *response : {"HTTP/1.1 204 X\r\n", "HTTP/1.1 204 X\n\n",
       "HTTP/1.1 204 X\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 204 X\r\nBad Header: x\r\n\r\n",
       "HTTP/1.1 204 X\r\nContent-Length: -1\r\n\r\n", "HTTP/1.1 100 Continue\r\n\r\n"}) {
    Transport t; t.response = response; assert(!run(t).ok);
  }
  Transport huge; huge.response = "HTTP/1.1 204 X\r\nX: " + std::string(4096, 'x');
  assert(!run(huge).ok && huge.offset <= 2048);
  Transport tls; tls.trusted = false;
  assert(!run(tls).ok && !tls.connections && tls.sent.empty());
  Transport open; open.opens = false;
  assert(!run(open).ok && open.sent.empty());
  Transport timeout; timeout.stall = true;
  result = run(timeout);
  assert(!result.ok && result.error == TelemetryError::Timeout && timeout.clock == 15000);
  Transport cancel; cancelled = true;
  result = run(cancel);
  assert(!result.ok && result.error == TelemetryError::Cancelled && !cancel.connections);
  cancelled = false; stopped = true;
  Transport stop; assert(!run(stop).ok && !stop.connections);
  puts("PASS bounded native POST, partial writes, TLS rejection, HTTP failures, header budgets, timeout, cancel, cleanup");
}
int main() { config(); transport(); }
