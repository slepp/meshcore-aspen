// Exercise the actual ESP-only handlers against a bounded SDK API seam.
// SDK 4.4.7 accepts into a free slot only when LRU purge is disabled. Its
// CLOSE cleanup queues deletion; a failing handler also deletes immediately.
#include "RadioDashboard.cpp"
#include <cassert>
#include <cerrno>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

size_t write_limit = SIZE_MAX;
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *data, size_t length,
                               int flags) {
  return __real_send(fd, data, std::min(length, write_limit), flags);
}

namespace {
int64_t clock_us = 1000000;
esp_err_t receive_result = ESP_OK;
httpd_config_t configured;
std::map<std::string, httpd_uri_t> routes;
struct Session {
  int fd = -1, peer = -1;
  bool websocket = false;
  void *transport = nullptr;
  httpd_free_ctx_fn_t free_transport = nullptr;
  httpd_req_t request{};
  std::map<std::string, std::string> headers;
  std::string status = "200 OK", body;
  unsigned fragments = 0;
  unsigned close_code = 0;
};
Session sessions[3];
Session &session(int fd) {
  for (auto &item : sessions)
    if (item.fd == fd)
      return item;
  assert(false);
  return sessions[0];
}
void destroy(Session &item) {
  if (item.request.sess_ctx)
    item.request.free_ctx(item.request.sess_ctx);
  if (item.transport)
    item.free_transport(item.transport);
  close(item.fd);
  close(item.peer);
  item = {};
}
Session *acceptConnection() {
  for (auto &item : sessions) {
    if (item.fd >= 0)
      continue;
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    item.fd = pair[0];
    item.peer = pair[1];
    item.request.handle = &configured;
    item.request.test_fd = item.fd;
    assert(configured.open_fn(&configured, item.fd) == ESP_OK);
    return &item;
  }
  // With LRU enabled the SDK closes an existing session before parsing the
  // incoming URI. That policy is forbidden for this shared service.
  assert(!configured.lru_purge_enable);
  return nullptr;
}
esp_err_t request(Session &item, const char *path) {
  const auto &route = routes.at(path);
  item.websocket = route.is_websocket;
  item.request.method = HTTP_GET;
  item.request.user_ctx = route.user_ctx;
  return route.handler(&item.request);
}
void alive(Session &item) {
  char byte;
  assert(recv(item.peer, &byte, 1, MSG_DONTWAIT) == -1);
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
}
void retired(Session &item) {
  char byte;
  assert(recv(item.peer, &byte, 1, MSG_DONTWAIT) == 0);
}
} // namespace

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
  configured = *config;
  *handle = &configured;
  return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t) { return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *uri) {
  routes[uri->uri] = *uri;
  return ESP_OK;
}
esp_err_t httpd_queue_work(httpd_handle_t, void (*)(void *), void *) {
  assert(false);
  return ESP_FAIL;
}
int httpd_req_to_sockfd(httpd_req_t *r) { return r->test_fd; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *,
                                      size_t) {
  return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *key,
                             const char *value) {
  session(r->test_fd).headers[key] = value;
  return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
  session(r->test_fd).status = status;
  return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *data, size_t length) {
  session(r->test_fd).body.assign(data, length);
  return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *data) {
  return httpd_resp_send(r, data, strlen(data));
}
esp_err_t httpd_sess_set_send_override(httpd_handle_t, int, httpd_send_func_t) {
  return ESP_OK;
}
esp_err_t httpd_sess_set_recv_override(httpd_handle_t, int, httpd_recv_func_t) {
  return ESP_OK;
}
void httpd_sess_set_transport_ctx(httpd_handle_t, int fd, void *context,
                                  httpd_free_ctx_fn_t free) {
  session(fd).transport = context;
  session(fd).free_transport = free;
}
void *httpd_sess_get_transport_ctx(httpd_handle_t, int fd) {
  return session(fd).transport;
}
void *httpd_sess_get_ctx(httpd_handle_t, int fd) {
  return session(fd).request.sess_ctx;
}
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t, int fd) {
  return session(fd).websocket ? HTTPD_WS_CLIENT_WEBSOCKET
                               : HTTPD_WS_CLIENT_HTTP;
}
esp_err_t httpd_ws_recv_frame(httpd_req_t *, httpd_ws_frame_t *frame, size_t) {
  frame->type = HTTPD_WS_TYPE_CLOSE;
  frame->final = true;
  frame->len = 0;
  return receive_result;
}
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int fd,
                                    httpd_ws_frame_t *frame) {
  auto &item = session(fd);
  if (frame->type == HTTPD_WS_TYPE_CLOSE) {
    item.close_code =
        frame->len == 2 ? (frame->payload[0] << 8) | frame->payload[1] : 1000;
  } else {
    ++item.fragments;
  }
  return ESP_OK;
}
esp_err_t httpd_ws_send_frame(httpd_req_t *r, httpd_ws_frame_t *frame) {
  return httpd_ws_send_frame_async(r->handle, r->test_fd, frame);
}
const char *esp_err_to_name(esp_err_t) { return "test error"; }
int64_t esp_timer_get_time() { return clock_us; }
void vTaskDelay(unsigned) { assert(false); }
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *,
                            unsigned, void *, unsigned) {
  return pdPASS;
}

struct DashboardStreamTest {
  static void transport() {
    RadioDashboard radio;
    assert(radio.beginHTTP());
    auto *peer = acceptConnection();
    assert(request(*peer, "/api/live") == ESP_OK);
    peer->request.method = 0;
    auto &client =
        *static_cast<RadioDashboard::LiveClient *>(peer->request.sess_ctx);
    auto callback = [&] {
      char first_byte;
      assert(RadioDashboard::liveReceive(&configured, peer->fd, &first_byte, 1,
                                         0) == 1);
      assert(static_cast<uint8_t>(first_byte) == client.input[0]);
      return RadioDashboard::liveRequest(&peer->request);
    };
    const uint8_t pong[] = {0x8a, 0x80, 0x01, 0x02, 0x03, 0x04};
    // Every TCP split in a masked empty pong, including a lone first byte.
    for (size_t split = 1; split < sizeof(pong); ++split) {
      client.pong_deadline_ms = clock_us / 1000 + 5000;
      assert(send(peer->peer, pong, split, 0) == static_cast<int>(split));
      assert(callback() == ESP_OK && client.pong_deadline_ms != 0);
      clock_us += 25000;
      assert(send(peer->peer, pong + split, sizeof(pong) - split, 0) ==
             static_cast<int>(sizeof(pong) - split));
      assert(callback() == ESP_OK);
      assert(!client.input_length && !client.pong_deadline_ms);
    }
    // A maximum-size ping arrives one byte at a time. Its masked payload is
    // retained once, then sent as one correct pong despite three-byte writes.
    std::vector<uint8_t> ping = {0x89, 0xfd, 1, 2, 3, 4};
    for (unsigned i = 0; i < 125; ++i)
      ping.push_back(static_cast<uint8_t>(i) ^ ping[2 + i % 4]);
    for (uint8_t byte : ping) {
      assert(send(peer->peer, &byte, 1, 0) == 1);
      assert(callback() == ESP_OK);
      clock_us += 1000;
    }
    assert(client.reply_pending && client.reply_length == 125);
    assert(RadioDashboard::liveSend(&radio, peer->fd, 10, client.reply, 125,
                                    true));
    client.reply_pending = false;
    write_limit = 3;
    while (client.output_length) {
      assert(RadioDashboard::flushOutput(client, clock_us / 1000));
      clock_us += 1000;
    }
    write_limit = SIZE_MAX;
    uint8_t response[127];
    assert(recv(peer->peer, response, sizeof(response), MSG_DONTWAIT) == 127);
    assert(response[0] == 0x8a && response[1] == 125);
    for (unsigned i = 0; i < 125; ++i)
      assert(response[i + 2] == i);

    // Real kernel backpressure preserves the pending frame and byte cursor.
    int small = 1024;
    assert(setsockopt(peer->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) ==
           0);
    char filler[1024]{};
    size_t filled = 0;
    int sent;
    while ((sent = send(peer->fd, filler, sizeof(filler), MSG_DONTWAIT)) > 0)
      filled += sent;
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(RadioDashboard::liveSend(&radio, peer->fd, 1,
                                    reinterpret_cast<const uint8_t *>("hello"),
                                    5, true));
    assert(RadioDashboard::flushOutput(client, clock_us / 1000));
    assert(client.output_offset == 0 && client.output_length == 7);
    while (filled) {
      int count = recv(peer->peer, filler, std::min(filled, sizeof(filler)), 0);
      assert(count > 0);
      filled -= count;
    }
    write_limit = 2;
    while (client.output_length) {
      assert(RadioDashboard::flushOutput(client, clock_us / 1000));
      clock_us += 1000;
    }
    write_limit = SIZE_MAX;
    assert(recv(peer->peer, response, sizeof(response), MSG_DONTWAIT) == 7);
    assert(memcmp(response, "\x81\x05hello", 7) == 0);

    // Cross the former ten-second boundary repeatedly with split pongs.
    for (unsigned heartbeat = 0; heartbeat < 12; ++heartbeat) {
      clock_us += 5000000;
      assert(RadioDashboard::liveSend(&radio, peer->fd, 9, nullptr, 0, true));
      assert(RadioDashboard::flushOutput(client, clock_us / 1000));
      assert(recv(peer->peer, response, sizeof(response), MSG_DONTWAIT) == 2);
      assert(response[0] == 0x89 && response[1] == 0);
      assert(client.pong_deadline_ms ==
             static_cast<uint64_t>(clock_us / 1000 + 5000));
      for (uint8_t byte : pong) {
        assert(send(peer->peer, &byte, 1, 0) == 1);
        assert(callback() == ESP_OK);
        clock_us += 25000;
      }
      assert(!client.pong_deadline_ms && !client.closing);
    }
    assert(client.pings_sent == 12 && client.pongs_received == 17);
    // Coalesced controls remain separate callbacks; no byte of the next
    // frame may be consumed as part of the first.
    assert(send(peer->peer, pong, sizeof(pong), 0) == sizeof(pong));
    assert(send(peer->peer, pong, sizeof(pong), 0) == sizeof(pong));
    assert(callback() == ESP_OK);
    assert(callback() == ESP_OK);
    assert(client.pongs_received == 19);
    const uint8_t unmasked[] = {0x8a, 0};
    assert(send(peer->peer, unmasked, sizeof(unmasked), 0) == sizeof(unmasked));
    assert(callback() == ESP_FAIL);
    assert(strcmp(client.close_reason, "invalid control length or mask") == 0);
    client.input_length = 0;
    assert(send(peer->peer, pong, 1, 0) == 1);
    assert(callback() == ESP_OK);
    assert(RadioDashboard::receiveControl(client, clock_us / 1000 + 1999));
    assert(!RadioDashboard::receiveControl(client, clock_us / 1000 + 2000));
    assert(strcmp(client.close_reason, "control read deadline") == 0);
    client.input_length = 0;
    assert(RadioDashboard::liveSend(&radio, peer->fd, 9, nullptr, 0, true));
    assert(!RadioDashboard::flushOutput(client, clock_us / 1000 + 2000));
    assert(strcmp(client.close_reason, "socket write deadline") == 0);
    destroy(*peer);
    clock_us = 1000000;
    puts("Dashboard real sockets: split controls, short writes, backpressure, "
         "twelve heartbeats and deadlines passed");
  }

  static void run() {
    RadioDashboard radio;
    assert(radio.beginHTTP());
    assert(configured.max_open_sockets == 3 && configured.backlog_conn == 2);
    assert(!configured.lru_purge_enable);
    RadioDashboard::RadioStatus status;
#ifdef MESHCORE_ONCHIP
    strcpy(status.device_name, "Aspen");
#endif
    queued_tx::putFloat(status.profile + 11, 1);
    auto pump = [&] {
      radio.publish(clock_us / 1000, status);
      RadioDashboard::liveWork(&radio);
      RadioDashboard::liveWork(&radio);
      for (auto &item : sessions) {
        if (item.fd < 0 || !item.websocket)
          continue;
        char data[4096];
        int count;
        while ((count = recv(item.peer, data, sizeof(data), MSG_DONTWAIT)) > 0)
          item.fragments += count;
      }
    };
    auto *first = acceptConnection();
    auto *second = acceptConnection();
    assert(request(*first, "/api/live") == ESP_OK);
    assert(request(*second, "/api/live") == ESP_OK);
    pump();
    assert(first->fragments && second->fragments);
    assert(strstr(radio._stream_json,
                  (std::string("\"firmware_version\":\"") + radio_firmware::version + "\"").c_str()));
#ifdef MESHCORE_ONCHIP
    assert(strstr(radio._stream_json, "\"device_name\":\"Aspen\""));
#endif

    // Even a peer that keeps HTTP open cannot retain the diagnostics slot.
    for (const char *path : {"/api/status", "/"}) {
      clock_us += 1000000;
      pump();
      auto *diagnostic = acceptConnection();
      assert(request(*diagnostic, path) == ESP_OK);
      assert(diagnostic->status == "200 OK" && !diagnostic->body.empty());
      if (!strcmp(path, "/api/status"))
        assert(diagnostic->body.find(std::string("\"firmware_version\":\"") +
                                     radio_firmware::version + "\"") != std::string::npos);
#ifdef MESHCORE_ONCHIP
      if (!strcmp(path, "/api/status"))
        assert(diagnostic->body.find("\"device_name\":\"Aspen\"") != std::string::npos);
#endif
      assert(diagnostic->headers.at("Connection") == "close");
      assert(!acceptConnection());
      pump();
      retired(*diagnostic);
      alive(*first);
      alive(*second);
      destroy(*diagnostic);
    }

    // A third upgrade gets an explicit retry close, never an established slot.
    auto *excess = acceptConnection();
    assert(request(*excess, "/api/live") == ESP_OK);
    assert(excess->close_code == 1013);
    retired(*excess);
    alive(*first);
    alive(*second);
    // EOF/CLOSE must not request both immediate and SDK-queued deletion.
    excess->request.method = 0;
    receive_result =
        ESP_FAIL; // SDK classifies EOF as CLOSE before the failed payload read.
    assert(RadioDashboard::liveRequest(&excess->request) == ESP_OK);
    receive_result = ESP_OK;
    destroy(*excess);

    // A connection that sends no request has a bounded admission lease.
    auto *idle = acceptConnection();
    clock_us += 1999000;
    pump();
    alive(*idle);
    clock_us += 1000;
    pump();
    retired(*idle);
    alive(*first);
    alive(*second);
    destroy(*idle);
    const unsigned before = first->fragments;
    clock_us += 500000;
    pump();
    assert(first->fragments > before);
    auto *diagnostic = acceptConnection();
    assert(request(*diagnostic, "/api/status") == ESP_OK);
    assert(diagnostic->status == "200 OK");
    pump();
    retired(*diagnostic);
    destroy(*diagnostic);
    diagnostic = acceptConnection();
    assert(request(*diagnostic, "/api/status") == ESP_OK);
    assert(diagnostic->status == "429 Too Many Requests");
    pump();
    retired(*diagnostic);
    destroy(*diagnostic);
    alive(*first);
    alive(*second);

    first->request.method = 0;
    assert(RadioDashboard::liveRequest(&first->request) == ESP_OK);
    destroy(*first);
    auto *replacement = acceptConnection();
    assert(request(*replacement, "/api/live") == ESP_OK);
    destroy(*replacement);
    destroy(*second);
    puts("Dashboard HTTP admission: diagnostics, idle lease, retry close and "
         "SDK close ownership passed");
  }
};
int main() {
  DashboardStreamTest::transport();
  DashboardStreamTest::run();
}
