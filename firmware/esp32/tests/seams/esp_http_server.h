// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifndef MESHCORE_MAST_WEB_TEST
#include_next <esp_http_server.h>
#else
#include <nvs.h>
#include <algorithm>
#include <map>
#include <string>
#include <cstring>
constexpr int HTTP_GET = 0, HTTP_POST = 1;
struct httpd_req_t {
  size_t content_len = 0, offset = 0;
  std::string body, response, status = "200 OK";
  std::map<std::string, std::string> headers, responseHeaders;
  bool failSend = false;
  unsigned readDelay = 0;
};
struct httpd_uri_t {
  const char *uri;
  int method;
  esp_err_t (*handler)(httpd_req_t *);
  void *user_ctx;
};
struct TestHTTPServer { std::map<std::string, httpd_uri_t> routes; };
using httpd_handle_t = TestHTTPServer *;
inline esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route) {
  server->routes[route->uri] = *route; return ESP_OK;
}
inline esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
  r->responseHeaders["Content-Type"] = type; return ESP_OK;
}
inline esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *value) {
  r->responseHeaders[key] = value; return ESP_OK;
}
inline esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
  r->status = status; return ESP_OK;
}
inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *text) {
  r->response = text; return r->failSend ? ESP_FAIL : ESP_OK;
}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *key) {
  const auto found = r->headers.find(key);
  return found == r->headers.end() ? 0 : found->second.size();
}
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *key, char *out, size_t capacity) {
  const auto found = r->headers.find(key);
  if (found == r->headers.end() || found->second.size() >= capacity) return ESP_FAIL;
  strcpy(out, found->second.c_str()); return ESP_OK;
}
inline int httpd_req_recv(httpd_req_t *r, char *out, size_t capacity) {
  if (r->readDelay) delay(r->readDelay);
  const size_t size = std::min(capacity, r->body.size() - r->offset);
  if (!size) return 0;
  memcpy(out, r->body.data() + r->offset, size); r->offset += size;
  return size;
}
#endif
