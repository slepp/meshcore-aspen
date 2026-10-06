#include "RadioDashboard.h"
#include "RadioFirmwareIdentity.h"
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastWeb.h"
#endif

#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <sys/socket.h>

constexpr size_t RadioDashboard::HISTORY;
constexpr size_t RadioDashboard::TRAFFIC_SECONDS;
constexpr size_t RadioDashboard::PREVIEW_BYTES;
constexpr size_t RadioDashboard::JSON_CAPACITY;
#if defined(MESHCORE_ONCHIP) && defined(ESP32) && !defined(NRF52_PLATFORM)
constexpr size_t RadioDashboard::CONTACT_CAPACITY;
#endif
constexpr size_t RadioDashboard::LIVE_CLIENTS;
constexpr size_t RadioDashboard::LIVE_FRAGMENT;
constexpr size_t RadioDashboard::ROLE_CAPACITY;

void RadioDashboard::advance(uint32_t now) {
  _live.uptime_ms += static_cast<uint32_t>(now - _last_clock);
  _last_clock = now;
}

RadioDashboard::Event &RadioDashboard::event(const uint8_t *packet,
                                             uint16_t length) {
  auto &entry = _live.events[_live.event_next];
  entry = {};
  entry.sequence = ++_live.events_total;
  entry.at_ms = _live.uptime_ms;
  entry.length = length;
  entry.preview_length = std::min<size_t>(length, PREVIEW_BYTES);
  if (entry.preview_length)
    memcpy(entry.preview, packet, entry.preview_length);
  _live.event_next = (_live.event_next + 1) % HISTORY;
  if (_live.event_count < HISTORY)
    ++_live.event_count;
  else
    ++_live.events_overwritten;
  return entry;
}

RadioDashboard::Bucket &RadioDashboard::bucket(uint64_t second) {
  auto &entry = _live.traffic[second % TRAFFIC_SECONDS];
  if (entry.second != second) {
    entry = {};
    entry.second = second;
  }
  return entry;
}

void RadioDashboard::occupancy(uint32_t duration, bool rx) {
  const uint64_t end = _live.uptime_ms;
  const uint64_t second = end / 1000;
  const uint64_t earliest =
      second >= TRAFFIC_SECONDS - 1 ? (second - TRAFFIC_SECONDS + 1) * 1000 : 0;
  uint64_t start = std::max(earliest, end - std::min<uint64_t>(end, duration));
  while (start < end) {
    const uint64_t stop = std::min(end, (start / 1000 + 1) * 1000);
    auto &entry = bucket(start / 1000);
    const uint32_t milliseconds = stop - start;
    if (rx)
      entry.rx_estimated_ms += milliseconds;
    else
      entry.tx_rf_ms += milliseconds;
    start = stop;
  }
}

void RadioDashboard::received(uint32_t now, const uint8_t *packet,
                              uint16_t length, float rssi, float snr,
                              uint32_t estimated_ms) {
  advance(now);
  auto &entry = event(packet, length);
  entry.rx = true;
  entry.has_signal = std::isfinite(rssi) && std::isfinite(snr);
  entry.rssi = rssi;
  entry.snr = snr;
  entry.estimated_ms = estimated_ms;
  ++_live.rx_packets;
  _live.rx_estimated_ms += estimated_ms;
  occupancy(estimated_ms, true);
  ++bucket(_live.uptime_ms / 1000).rx_packets;
}

void RadioDashboard::transmitted(uint32_t now, const uint8_t *packet,
                                 uint16_t length, uint8_t slot,
                                 uint32_t generation, uint32_t job,
                                 uint8_t state, uint8_t reason,
                                 uint32_t queue_ms, uint32_t rf_ms,
                                 uint32_t estimated_ms) {
  advance(now);
  switch (state) {
  case queued_tx::ACCEPTED:
    ++_live.tx_accepted;
    return;
  case queued_tx::REJECTED:
    ++_live.tx_rejected;
    break;
  case queued_tx::SUCCEEDED:
    ++_live.tx_succeeded;
    break;
  case queued_tx::FAILED:
    ++_live.tx_failed;
    break;
  case queued_tx::UNKNOWN:
    ++_live.tx_unknown;
    break;
  }
  auto &entry = event(packet, length);
  entry.source_slot = slot;
  entry.source_generation = generation;
  entry.job_id = job;
  entry.state = state;
  entry.reason = reason;
  entry.queue_ms = queue_ms;
  entry.rf_ms = rf_ms;
  entry.estimated_ms = estimated_ms;
  _live.tx_rf_ms += rf_ms;
  occupancy(rf_ms, false);
  if (rf_ms)
    ++bucket(_live.uptime_ms / 1000).tx_packets;
}

bool RadioDashboard::publish(uint32_t now, const RadioStatus &status) {
  advance(now);
  if (status.wifi_connected && !_live.radio.wifi_connected)
    _wifi_since = _live.uptime_ms;
  _live.wifi_uptime_ms =
      status.wifi_connected ? _live.uptime_ms - _wifi_since : 0;
  _live.radio = status;
  if (_copying.test_and_set(std::memory_order_acquire)) {
    ++_live.publications_skipped;
    return false;
  }
  ++_live.publication;
  _published = _live;
  _copying.clear(std::memory_order_release);
  return true;
}

bool RadioDashboard::snapshot(Snapshot &destination) const {
  if (_copying.test_and_set(std::memory_order_acquire))
    return false;
  destination = _published;
  _copying.clear(std::memory_order_release);
  return destination.publication != 0;
}

bool RadioDashboard::totals(Totals &destination, RadioStatus *radio) const {
  if (_copying.test_and_set(std::memory_order_acquire))
    return false;
  destination = _published;
  if (radio) *radio = _published.radio;
  _copying.clear(std::memory_order_release);
  return destination.publication != 0;
}

namespace {
class JSON {
  char *_output;
  size_t _capacity, _length = 0;
  bool _ok = true;

public:
  JSON(char *output, size_t capacity) : _output(output), _capacity(capacity) {}
  void append(const char *format, ...) {
    if (!_ok)
      return;
    va_list arguments;
    va_start(arguments, format);
    const int n =
        vsnprintf(_output + _length, _capacity - _length, format, arguments);
    va_end(arguments);
    if (n < 0 || static_cast<size_t>(n) >= _capacity - _length)
      _ok = false;
    else
      _length += n;
  }
  void string(const char *value) {
    append("\"");
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(value);
         *p && _ok; ++p) {
      unsigned utf8 = *p >= 0xc2 && *p <= 0xdf   ? 2
                      : *p >= 0xe0 && *p <= 0xef ? 3
                      : *p >= 0xf0 && *p <= 0xf4 ? 4
                                                 : 0;
      for (unsigned i = 1; i < utf8; ++i)
        if ((p[i] & 0xc0) != 0x80) {
          utf8 = 0;
          break;
        }
      if (utf8 &&
          ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] >= 0xa0) ||
           (*p == 0xf0 && p[1] < 0x90) || (*p == 0xf4 && p[1] >= 0x90)))
        utf8 = 0;
      if (utf8) {
        for (unsigned i = 0; i < utf8; ++i)
          append("%c", p[i]);
        p += utf8 - 1;
      } else if (*p == '"' || *p == '\\')
        append("\\%c", *p);
      else if (*p < 32 || *p >= 127)
        append("\\u%04x", *p);
      else
        append("%c", *p);
    }
    append("\"");
  }
  size_t finish() {
    if (!_ok && _capacity)
      _output[0] = '\0';
    return _ok ? _length : 0;
  }
};
void roleJSON(JSON &j, const RadioDashboard::RoleStatus &r) {
  j.append("{\"role\":");
  j.string(r.role);
  j.append(",\"name\":");
  j.string(r.name);
  j.append(",\"public_key\":");
  if (r.has_identity) {
    j.append("\"");
    for (uint8_t byte : r.public_key)
      j.append("%02x", byte);
    j.append("\"");
  } else
    j.append("null");
  j.append(",\"state\":");
  j.string(r.state);
  j.append(",\"ready\":%s,\"fault\":", r.ready ? "true" : "false");
  if (r.fault[0])
    j.string(r.fault);
  else
    j.append("null");
  j.append(",\"source_slot\":");
  if (r.source_slot >= 0)
    j.append("%d", r.source_slot);
  else
    j.append("null");
  j.append(",\"source_generation\":%u,\"profile_generation\":\"%" PRIu64 "\"}",
           r.source_generation, r.profile_generation);
}
} // namespace

size_t RadioDashboard::formatRoleJSON(const RoleStatus &role, char *output,
                                      size_t capacity) {
  JSON j(output, capacity);
  roleJSON(j, role);
  return j.finish();
}

size_t RadioDashboard::formatJSON(const Snapshot &s, const char *name,
                                  char *output, size_t capacity) {
  JSON j(output, capacity);
  const auto &r = s.radio;
  const auto *p = r.profile;
  const float factor = queued_tx::getFloat(p + 11);
  const uint32_t maximum =
      static_cast<uint32_t>(queued_tx::WINDOW_MS * (1.0f / (1.0f + factor)));
  j.append("{\"api_version\":1,\"device_name\":");
#ifdef MESHCORE_ONCHIP
  if (r.device_name[0]) name = r.device_name;
#endif
  j.string(name);
  j.append(",\"firmware_version\":");
  j.string(radio_firmware::version);
  j.append(",\"upstream_tag\":");
  j.string(MESHCORE_UPSTREAM_TAG);
  j.append(",\"upstream_commit\":");
  j.string(MESHCORE_UPSTREAM_COMMIT);
  j.append(",\"publication\":%" PRIu64 ",\"uptime_ms\":%" PRIu64
           ",\"wifi\":{\"connected\":%s,\"rssi_dbm\":",
           s.publication, s.uptime_ms, r.wifi_connected ? "true" : "false");
  if (r.wifi_connected)
    j.append("%d", r.wifi_rssi);
  else
    j.append("null");
  j.append(",\"uptime_ms\":%" PRIu64
           "},\"memory\":{\"free_bytes\":%u,\"minimum_bytes\":%u,"
           "\"dma_free_bytes\":%u,\"dma_largest_bytes\":%u,\"dma_minimum_bytes\":%u}",
           s.wifi_uptime_ms, r.free_heap, r.minimum_heap, r.dma_free_heap,
           r.dma_largest_heap, r.dma_minimum_heap);
  if (r.stream.enabled) {
    j.append(",\"stream\":{\"slot\":%u,\"generation\":%u,\"connected\":%s,"
             "\"negotiated\":%s,\"fault\":%s,\"output_overflows\":%u}",
             r.stream.slot, r.stream.generation,
             r.stream.connected ? "true" : "false",
             r.stream.negotiated ? "true" : "false",
             r.stream.fault ? "true" : "false", r.stream.output_overflows);
  }
#if defined(MESHCORE_ONCHIP) && defined(ESP32) && !defined(NRF52_PLATFORM)
  if (r.contacts_available) {
    const unsigned count = std::min<size_t>(r.contact_count, CONTACT_CAPACITY);
    j.append(",\"contacts\":{\"capacity\":%u,\"total\":%u,\"truncated\":%s,\"items\":[",
             unsigned(CONTACT_CAPACITY), r.contact_total,
             r.contact_total > count ? "true" : "false");
    for (unsigned i = 0; i < count; ++i) {
      const auto &contact = r.contacts[i];
      j.append("%s{\"public_key\":\"", i ? "," : "");
      for (const auto byte : contact.public_key) j.append("%02x", byte);
      j.append("\",\"name\":");
      j.string(contact.name);
      j.append(",\"type\":%u}", contact.type);
    }
    j.append("]}");
  }
#endif
#ifdef MESHCORE_ONCHIP
  if (r.role_count) {
    j.append(",\"roles\":[");
    for (unsigned i = 0; i < r.role_count && i < ROLE_CAPACITY; ++i) {
      if (i)
        j.append(",");
      roleJSON(j, r.roles[i]);
    }
    j.append("]");
  }
#endif
  j.append(",\"profile\":{\"frequency_hz\":%u,\"bandwidth_hz\":%u,\"sf\":%u,"
           "\"cr\":%u,"
           "\"tx_power_dbm\":%u,\"airtime_factor\":%.9g,\"cad\":%s,"
           "\"interference_threshold\":%d,"
           "\"generation\":%u,\"committed\":%s,\"fault\":%s}",
           queued_tx::get32(p), queued_tx::get32(p + 4), p[8], p[9], p[10],
           static_cast<double>(factor), p[15] ? "true" : "false",
           static_cast<int16_t>(queued_tx::get16(p + 16)), r.generation,
           r.committed ? "true" : "false", r.fault ? "true" : "false");
  j.append(",\"scheduler\":{\"queued\":%u,\"capacity\":%u,\"transmitting\":%s,"
           "\"carrier_wait\":%s,"
           "\"credit_ms\":%u,\"maximum_credit_ms\":%u,\"window_ms\":%u,\"owner_"
           "slot\":%d}",
           r.queued, KISS_REQUEST_QUEUE_DEPTH,
           r.transmitting ? "true" : "false", r.carrier_wait ? "true" : "false",
           r.credit_ms, maximum, queued_tx::WINDOW_MS, r.owner_slot);
  j.append(",\"kiss\":{\"connected\":%u,\"capacity\":%u,\"clients\":[",
           r.clients, KISS_MAX_TCP_CLIENTS);
  bool comma = false;
  for (size_t i = 0; i < KISS_MAX_TCP_CLIENTS; ++i) {
    const auto &c = r.client[i];
    if (!c.connected)
      continue;
    j.append("%s{\"slot\":%u,\"generation\":%u,\"negotiated\":%s,\"airtime_"
             "factor\":%.9g,"
             "\"credit_ms\":%u,\"rf_ms\":%u}",
             comma ? "," : "", static_cast<unsigned>(i), c.generation,
             c.negotiated ? "true" : "false", static_cast<double>(c.factor),
             c.credit_ms, c.rf_ms);
    comma = true;
  }
  j.append("]");
  if (r.session.connected) {
    j.append(",\"session\":{\"physical_slot\":%u,\"ports\":[",
             r.session.physical_slot);
    for (size_t i = 0; i < queued_tx::SESSION_PORTS - 1; ++i) {
      const auto &port = r.session.port[i];
      j.append("%s{\"port\":%u,\"connected\":%s,\"generation\":%u,"
               "\"airtime_factor\":%.9g,\"credit_ms\":%u,\"rf_ms\":%u,"
               "\"output_queued_bytes\":%u}",
               i ? "," : "", static_cast<unsigned>(i + 1),
               port.connected ? "true" : "false", port.generation,
               static_cast<double>(port.factor), port.credit_ms, port.rf_ms,
               r.session.output_queued_bytes[i]);
    }
    j.append("]}");
  }
  j.append(
      "},\"totals\":{\"rx_packets\":%" PRIu64 ",\"rx_estimated_ms\":%" PRIu64
      ",\"rx_errors\":%u,\"tx_accepted\":%" PRIu64 ",\"tx_rejected\":%" PRIu64
      ",\"tx_succeeded\":%" PRIu64 ",\"tx_failed\":%" PRIu64
      ",\"tx_unknown\":%" PRIu64 ",\"tx_rf_ms\":%" PRIu64
      "},\"history\":{\"capacity\":%u,\"overwritten\":%" PRIu64 ",\"events\":[",
      s.rx_packets, s.rx_estimated_ms, r.rx_errors, s.tx_accepted,
      s.tx_rejected, s.tx_succeeded, s.tx_failed, s.tx_unknown, s.tx_rf_ms,
      static_cast<unsigned>(HISTORY), s.events_overwritten);
  for (size_t i = 0; i < s.event_count; ++i) {
    const auto &e = s.events[(s.event_next + HISTORY - 1 - i) % HISTORY];
    j.append("%s{\"sequence\":%" PRIu64 ",\"at_ms\":%" PRIu64
             ",\"direction\":\"%s\","
             "\"state\":%u,\"reason\":%u,\"source_slot\":%u,\"source_"
             "generation\":%u,\"job_id\":%u,"
             "\"length\":%u,\"queue_ms\":%u,\"rf_ms\":",
             i ? "," : "", e.sequence, e.at_ms, e.rx ? "rx" : "tx", e.state,
             e.reason, e.source_slot, e.source_generation, e.job_id, e.length,
             e.queue_ms);
    if (e.rx)
      j.append("null");
    else
      j.append("%u", e.rf_ms);
    j.append(",\"estimated_ms\":%u,\"rssi_dbm\":", e.estimated_ms);
    if (e.has_signal)
      j.append("%.2f", static_cast<double>(e.rssi));
    else
      j.append("null");
    j.append(",\"snr_db\":");
    if (e.has_signal)
      j.append("%.2f", static_cast<double>(e.snr));
    else
      j.append("null");
    j.append(",\"preview_hex\":\"");
    for (size_t k = 0; k < e.preview_length; ++k)
      j.append("%02x", e.preview[k]);
    j.append("\",\"preview_truncated\":%s}",
             e.length > e.preview_length ? "true" : "false");
  }
  j.append("]},\"traffic\":[");
  const uint64_t last = s.uptime_ms / 1000;
  const uint64_t first =
      last >= TRAFFIC_SECONDS - 1 ? last - TRAFFIC_SECONDS + 1 : 0;
  for (uint64_t second = first; second <= last; ++second) {
    const auto &b = s.traffic[second % TRAFFIC_SECONDS];
    const bool present = b.second == second;
    j.append("%s{\"second\":%" PRIu64 ",\"rx_packets\":%u,\"tx_packets\":%u,"
             "\"tx_rf_ms\":%u,\"rx_estimated_ms\":%u}",
             second == first ? "" : ",", second, present ? b.rx_packets : 0,
             present ? b.tx_packets : 0, present ? b.tx_rf_ms : 0,
             present ? b.rx_estimated_ms : 0);
  }
  j.append("],\"publications_skipped\":%" PRIu64 "}", s.publications_skipped);
  return j.finish();
}

RadioDashboard::LiveClient *RadioDashboard::subscribe(int fd, uint64_t now) {
  for (auto &client : _clients) {
    if (client.fd >= 0)
      continue;
    client = {};
    client.fd = fd;
    client.started_ms = now;
    client.ping_ms = now;
    return &client;
  }
  return nullptr;
}

void RadioDashboard::pumpLive(uint64_t now, const char *name, LiveSend send,
                              LiveDrop drop, void *context) {
  auto evict = [&](LiveClient &client, const char *reason) {
    client.close_reason = reason;
    client.closing = true;
    client.sending = false;
    drop(context, client.fd);
  };
  bool active = false, transmitting = false;
  for (auto &client : _clients) {
    if (client.fd < 0 || client.closing)
      continue;
    if (client.sending && now - client.started_ms >= 2000) {
      evict(client, "snapshot send deadline");
      continue;
    }
    if (client.pong_deadline_ms && now >= client.pong_deadline_ms) {
      evict(client, "pong deadline");
      continue;
    }
    active = true;
    transmitting |= client.sending;
  }
  if (!active)
    return;
  if (!snapshot(_stream_snapshot)) {
    for (auto &client : _clients)
      if (client.fd >= 0 && !client.closing && !client.delivered &&
          now - client.started_ms >= 3000)
        evict(client, "initial snapshot deadline");
    return;
  }
  if (now > _stream_snapshot.uptime_ms + 3000) {
    for (auto &client : _clients)
      if (client.fd >= 0 && !client.closing)
        evict(client, "stale radio snapshot");
    return;
  }
  // One shared frame stays immutable until current fragments are finished.
  // Intermediate radio publications are coalesced, never queued per client.
  if (!transmitting && _stream_publication != _stream_snapshot.publication) {
    _stream_length =
        formatJSON(_stream_snapshot, name, _stream_json, sizeof(_stream_json));
    if (!_stream_length) {
      for (auto &client : _clients)
        if (client.fd >= 0 && !client.closing)
          evict(client, "snapshot serialization");
      return;
    }
    _stream_publication = _stream_snapshot.publication;
  }
  for (auto &client : _clients) {
    if (client.fd < 0 || client.closing)
      continue;
    if (client.output_length || client.reply_pending)
      continue;
    if (!client.pong_deadline_ms && now - client.ping_ms >= 5000) {
      if (!send(context, client.fd, 9, nullptr, 0, true)) {
        evict(client, "ping send error");
        continue;
      }
      client.ping_ms = now;
      client.pong_deadline_ms = now + 5000;
      if (client.output_length)
        continue;
    }
    if (!client.sending && client.delivered != _stream_publication) {
      client.offset = 0;
      client.started_ms = now;
      client.sending = true;
    }
    if (!client.sending)
      continue;
    const size_t count =
        std::min(LIVE_FRAGMENT, _stream_length - client.offset);
    const bool final = client.offset + count == _stream_length;
    if (!send(context, client.fd, client.offset ? 0 : 1,
              reinterpret_cast<const uint8_t *>(_stream_json + client.offset),
              count, final)) {
      evict(client, "snapshot send error");
      continue;
    }
    client.offset += count;
    if (final) {
      client.sending = false;
      client.delivered = _stream_publication;
    }
  }
}

int RadioDashboard::sendAvailable(int fd, const char *data, size_t length,
                                  int flags) {
  if (!length)
    return 0;
  const int sent = ::send(fd, data, length, flags | MSG_DONTWAIT);
  // IDF 4.4's frame sender checks only negative results, not partial writes.
  // A partial frame cannot be reused; fail the session instead of corrupting
  // it.
  return sent == static_cast<int>(length) ? sent : -1;
}

#ifdef ARDUINO_ARCH_ESP32
#include "RadioDashboardPage.h"
#include "RadioNetwork.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if !CONFIG_HTTPD_WS_SUPPORT
#error "Dashboard push requires ESP-IDF HTTP server WebSocket support"
#endif

#ifndef KISS_DASHBOARD_NAME
#define KISS_DASHBOARD_NAME "MeshCore Radio"
#endif
static_assert(sizeof(KISS_DASHBOARD_NAME) <= 64,
              "Dashboard name must be at most 63 bytes");
#ifdef MESHCORE_ONCHIP
#include "Capacity.h"
#else
static_assert(KISS_MAX_TCP_CLIENTS + 1 + RadioDashboard::HTTP_INTERNAL_SOCKETS +
                 RadioDashboard::HTTP_CLIENTS + 1 <= CONFIG_LWIP_MAX_SOCKETS,
              "Dashboard needs KISS slots/listener, three internal HTTP "
              "sockets, three HTTP clients and one spare socket");
#endif

namespace {
constexpr size_t HTTP_SESSIONS = RadioDashboard::HTTP_CLIENTS;
struct HTTPSession {
  int fd = -1;
  int64_t deadline_us = 0;
  bool retiring = false;
};
// The single dashboard service has the same fixed bound as the SDK session
// table.
HTTPSession http_sessions[HTTP_SESSIONS];

esp_err_t openHTTP(httpd_handle_t server, int fd) {
  for (auto &session : http_sessions) {
    if (session.fd >= 0)
      continue;
    session.fd = fd;
    session.deadline_us = esp_timer_get_time() + 2000000;
    httpd_sess_set_transport_ctx(server, fd, &session, [](void *context) {
      *static_cast<HTTPSession *>(context) = {};
    });
    return ESP_OK;
  }
  Serial.println("Dashboard HTTP session accounting exhausted");
  return ESP_FAIL;
}

void retireHTTP(httpd_handle_t server, int64_t now) {
  for (auto &session : http_sessions) {
    if (session.fd < 0 || session.retiring || now < session.deadline_us ||
        httpd_ws_get_fd_info(server, session.fd) != HTTPD_WS_CLIENT_HTTP)
      continue;
    session.retiring = true;
    shutdown(session.fd, SHUT_RDWR);
  }
}

void responseHeaders(httpd_req_t *request, const char *type) {
  // Connection: close alone does not retire an IDF 4.4 HTTP session.
  // The HTTP task retires it after the response handler has finished.
  auto *session = static_cast<HTTPSession *>(httpd_sess_get_transport_ctx(
      request->handle, httpd_req_to_sockfd(request)));
  if (session)
    session->deadline_us = esp_timer_get_time();
  httpd_resp_set_type(request, type);
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
  httpd_resp_set_hdr(request, "Content-Security-Policy",
                     "default-src 'none'; script-src 'unsafe-inline'; "
                     "style-src 'unsafe-inline'; "
                     "connect-src 'self'; frame-ancestors 'none'; base-uri "
                     "'none'; form-action 'none'");
}
} // namespace

esp_err_t RadioDashboard::pageRequest(httpd_req_t *request) {
  responseHeaders(request, "text/html; charset=utf-8");
  return httpd_resp_send(request, RADIO_DASHBOARD_PAGE,
                         sizeof(RADIO_DASHBOARD_PAGE) - 1);
}

esp_err_t RadioDashboard::statusRequest(httpd_req_t *request) {
  auto &dashboard = *static_cast<RadioDashboard *>(request->user_ctx);
  responseHeaders(request, "application/json");
  const int64_t now = esp_timer_get_time();
  if (dashboard._last_api_us && now - dashboard._last_api_us < 500000) {
    httpd_resp_set_status(request, "429 Too Many Requests");
    httpd_resp_set_hdr(request, "Retry-After", "1");
    return httpd_resp_sendstr(
        request, "{\"error\":\"poll no faster than twice per second\"}");
  }
  dashboard._last_api_us = now;
  if (!dashboard.snapshot(dashboard._http_snapshot)) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_hdr(request, "Retry-After", "1");
    return httpd_resp_sendstr(
        request, "{\"error\":\"snapshot temporarily unavailable\"}");
  }
  if (static_cast<uint64_t>(esp_timer_get_time() / 1000) >
      dashboard._http_snapshot.uptime_ms + 3000) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_hdr(request, "Retry-After", "1");
    return httpd_resp_sendstr(request,
                              "{\"error\":\"radio snapshot is stale\"}");
  }
  const size_t length =
      formatJSON(dashboard._http_snapshot, KISS_DASHBOARD_NAME, dashboard._json,
                 sizeof(dashboard._json));
  if (!length) {
    Serial.println("Dashboard JSON capacity exceeded");
    httpd_resp_set_status(request, "500 Internal Server Error");
    return httpd_resp_sendstr(
        request, "{\"error\":\"snapshot exceeds response capacity\"}");
  }
  return httpd_resp_send(request, dashboard._json, length);
}

void RadioDashboard::releaseLive(void *context) {
  auto &client = *static_cast<LiveClient *>(context);
  Serial.printf("Dashboard stream fd=%d closed: %s; publication=%llu rx=%u "
                "tx=%u/%u ping=%lu pong=%lu\n",
                client.fd, client.close_reason,
                static_cast<unsigned long long>(client.delivered),
                static_cast<unsigned>(client.input_length),
                static_cast<unsigned>(client.output_offset),
                static_cast<unsigned>(client.output_length),
                static_cast<unsigned long>(client.pings_sent),
                static_cast<unsigned long>(client.pongs_received));
  client = {};
}

int RadioDashboard::liveReceive(httpd_handle_t server, int fd, char *data,
                                size_t length, int flags) {
  auto *client = static_cast<LiveClient *>(httpd_sess_get_ctx(server, fd));
  if (!client || length != 1)
    return HTTPD_SOCK_ERR_FAIL;
  // IDF reads one opcode before each callback. Replay that same opcode while
  // our bounded control-frame reader retains a partial mask/payload.
  if (!client->input_length) {
    const int count = ::recv(fd, client->input, 1, flags | MSG_DONTWAIT);
    if (count != 1)
      return HTTPD_SOCK_ERR_FAIL;
    client->input_length = 1;
    client->input_since_ms = esp_timer_get_time() / 1000;
  }
  data[0] = client->input[0];
  return 1;
}

bool RadioDashboard::receiveControl(LiveClient &client, uint64_t now) {
  if (!client.input_length)
    return true;
  if (now - client.input_since_ms >= 2000) {
    client.close_reason = "control read deadline";
    return false;
  }
  const uint8_t opcode = client.input[0] & 15;
  if ((client.input[0] & 0xf0) != 0x80 || (opcode != 9 && opcode != 10)) {
    client.close_reason = "invalid control opcode";
    return false;
  }
  // At most two nonblocking reads: length, then mask and payload. Never read
  // ahead into the next frame; the SDK must see its first byte independently.
  for (unsigned step = 0; step < 2; ++step) {
    size_t required = 2;
    if (client.input_length >= 2) {
      if (!(client.input[1] & 0x80) || (client.input[1] & 127) > 125) {
        client.close_reason = "invalid control length or mask";
        return false;
      }
      required = 6 + (client.input[1] & 127);
    }
    if (client.input_length < required) {
      const int count = ::recv(client.fd, client.input + client.input_length,
                               required - client.input_length, MSG_DONTWAIT);
      if (count < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return true;
      if (count <= 0) {
        client.close_reason = "control socket EOF/error";
        return false;
      }
      client.input_length += count;
      if (client.input_length < required)
        return true;
    }
  }
  const size_t length = client.input[1] & 127;
  if (client.input_length < 6 + length)
    return true;
  if (opcode == 10) {
    if (length) {
      client.close_reason = "unexpected pong payload";
      return false;
    }
    client.pong_deadline_ms = 0;
    ++client.pongs_received;
  } else {
    if (client.reply_pending) {
      client.close_reason = "control reply capacity";
      return false;
    }
    for (size_t i = 0; i < length; ++i)
      client.reply[i] = client.input[6 + i] ^ client.input[2 + i % 4];
    client.reply_length = length;
    client.reply_pending = true;
  }
  client.input_length = 0;
  return true;
}

bool RadioDashboard::flushOutput(LiveClient &client, uint64_t now) {
  if (!client.output_length)
    return true;
  if (now - client.output_since_ms >= 2000) {
    client.close_reason = "socket write deadline";
    return false;
  }
  const int count =
      ::send(client.fd, client.output + client.output_offset,
             client.output_length - client.output_offset, MSG_DONTWAIT);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return true;
  if (count <= 0) {
    client.close_reason = "socket write error";
    return false;
  }
  client.output_offset += count;
  if (client.output_offset == client.output_length) {
    if (client.output_opcode == 9) {
      ++client.pings_sent;
      client.pong_deadline_ms = now + 5000;
    }
    client.output_length = client.output_offset = 0;
  }
  return true;
}

esp_err_t RadioDashboard::liveRequest(httpd_req_t *request) {
  auto &dashboard = *static_cast<RadioDashboard *>(request->user_ctx);
  if (request->method == HTTP_GET) {
    char origin[128], host[96];
    const esp_err_t origin_result =
        httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin));
    if (origin_result != ESP_ERR_NOT_FOUND &&
        (origin_result != ESP_OK ||
         httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) !=
             ESP_OK ||
         strncmp(origin, "http://", 7) != 0 || strcmp(origin + 7, host) != 0))
      return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(request);
    auto send = [](httpd_handle_t, int socket, const char *data, size_t length,
                   int flags) {
      const int result = sendAvailable(socket, data, length, flags);
      return result < 0 ? HTTPD_SOCK_ERR_FAIL : result;
    };
    if (httpd_sess_set_send_override(request->handle, fd, send) != ESP_OK)
      return ESP_FAIL;
    auto *client = dashboard.subscribe(fd, esp_timer_get_time() / 1000);
    if (!client) {
      // IDF sends the upgrade before invoking this handler. Refuse only this
      // subscriber with WebSocket "Try Again Later", not an abrupt reset.
      uint8_t retry_code[] = {0x03, 0xf5}; // 1013
      httpd_ws_frame_t close_frame{};
      close_frame.type = HTTPD_WS_TYPE_CLOSE;
      close_frame.payload = retry_code;
      close_frame.len = sizeof(retry_code);
      const esp_err_t result = httpd_ws_send_frame(request, &close_frame);
      shutdown(fd, SHUT_RDWR);
      return result;
    }
    request->sess_ctx = client;
    request->free_ctx = releaseLive;
    if (httpd_sess_set_recv_override(request->handle, fd, liveReceive) !=
        ESP_OK)
      return ESP_FAIL;
    return ESP_OK;
  }
  auto *client = static_cast<LiveClient *>(request->sess_ctx);
  // No saved byte means EOF; CLOSE is also already owned by SDK cleanup.
  // Do not return failure and trigger a second deletion of the same slot.
  if (!client || !client->input_length ||
      (client->input[0] & 15) == HTTPD_WS_TYPE_CLOSE)
    return ESP_OK;
  return !client->closing &&
                 receiveControl(*client, esp_timer_get_time() / 1000)
             ? ESP_OK
             : ESP_FAIL;
}

bool RadioDashboard::liveSend(void *context, int fd, uint8_t opcode,
                              const uint8_t *data, size_t length, bool final) {
  auto &dashboard = *static_cast<RadioDashboard *>(context);
  if (httpd_ws_get_fd_info(dashboard._server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
    return false;
  for (auto &client : dashboard._clients) {
    if (client.fd != fd)
      continue;
    if (client.output_length || length > LIVE_FRAGMENT)
      return false;
    client.output[0] = (final ? 0x80 : 0) | opcode;
    size_t header = 2;
    if (length < 126) {
      client.output[1] = length;
    } else {
      client.output[1] = 126;
      client.output[2] = length >> 8;
      client.output[3] = length;
      header = 4;
    }
    if (length)
      memcpy(client.output + header, data, length);
    client.output_length = header + length;
    client.output_offset = 0;
    client.output_opcode = opcode;
    client.output_since_ms = esp_timer_get_time() / 1000;
    return true;
  }
  return false;
}

void RadioDashboard::liveDrop(void *context, int fd) {
  // Shutdown wakes the SDK's select loop; it owns close/context cleanup.
  // Do not queue a delayed close against a potentially reused descriptor.
  (void)context;
  shutdown(fd, SHUT_RDWR);
}

void RadioDashboard::liveWork(void *context) {
  auto &dashboard = *static_cast<RadioDashboard *>(context);
  retireHTTP(dashboard._server, esp_timer_get_time());
  const uint64_t now = esp_timer_get_time() / 1000;
  for (auto &client : dashboard._clients) {
    if (client.fd < 0 || client.closing)
      continue;
    if (client.input_length && now - client.input_since_ms >= 2000) {
      client.close_reason = "control read deadline";
      client.closing = true;
      liveDrop(context, client.fd);
      continue;
    }
    if (!flushOutput(client, now)) {
      client.closing = true;
      liveDrop(context, client.fd);
      continue;
    }
    if (!client.output_length && client.reply_pending) {
      if (!liveSend(context, client.fd, 10, client.reply, client.reply_length,
                    true)) {
        client.close_reason = "pong reply send error";
        client.closing = true;
        liveDrop(context, client.fd);
      }
      client.reply_pending = false;
    }
  }
  dashboard.pumpLive(esp_timer_get_time() / 1000, KISS_DASHBOARD_NAME, liveSend,
                     liveDrop, context);
  dashboard._live_work_pending.store(false, std::memory_order_release);
}

void RadioDashboard::liveTask(void *context) {
  auto &dashboard = *static_cast<RadioDashboard *>(context);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(25));
    if (dashboard._live_work_pending.exchange(true, std::memory_order_acq_rel))
      continue;
    if (httpd_queue_work(dashboard._server, liveWork, context) != ESP_OK) {
      dashboard._live_work_pending.store(false, std::memory_order_release);
      Serial.println("Dashboard push scheduling failed");
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

bool RadioDashboard::beginHTTP() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = KISS_HTTP_PORT;
  config.task_priority = 1;
  config.core_id = 0;
  config.stack_size = 8192;
  config.max_open_sockets = HTTP_SESSIONS;
  config.backlog_conn = 2;
  config.max_uri_handlers = 3;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  config.max_uri_handlers += 7;
#endif
  // LRU purge runs before accept/route admission and may evict a healthy
  // stream. Full capacity must wait in the bounded listen backlog instead.
  config.lru_purge_enable = false;
  config.open_fn = openHTTP;
  config.recv_wait_timeout = 2;
  config.send_wait_timeout = 2;
  esp_err_t error = httpd_start(&_server, &config);
  if (error == ESP_OK) {
    httpd_uri_t page{};
    page.uri = "/";
    page.method = HTTP_GET;
    page.handler = pageRequest;
    page.user_ctx = this;
    error = httpd_register_uri_handler(_server, &page);
    if (error == ESP_OK) {
      httpd_uri_t status{};
      status.uri = "/api/status";
      status.method = HTTP_GET;
      status.handler = statusRequest;
      status.user_ctx = this;
      error = httpd_register_uri_handler(_server, &status);
    }
    if (error == ESP_OK) {
      httpd_uri_t live{};
      live.uri = "/api/live";
      live.method = HTTP_GET;
      live.handler = liveRequest;
      live.user_ctx = this;
      live.is_websocket = true;
      live.handle_ws_control_frames = true;
      error = httpd_register_uri_handler(_server, &live);
    }
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    if (error == ESP_OK) error = onchip::registerMastWeb(_server);
#endif
    if (error == ESP_OK &&
        xTaskCreatePinnedToCore(liveTask, "radio-push", 2048, this, 1, nullptr,
                                0) != pdPASS)
      error = ESP_ERR_NO_MEM;
  }
  if (error != ESP_OK) {
    Serial.printf("Dashboard HTTP unavailable: %s\n", esp_err_to_name(error));
    if (_server) {
      httpd_stop(_server);
      _server = nullptr;
    }
    return false;
  }
  Serial.printf("Read-only radio dashboard listening on port %u\n",
                KISS_HTTP_PORT);
  return true;
}
#endif
