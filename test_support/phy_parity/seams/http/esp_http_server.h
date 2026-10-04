#pragma once
#include <cstddef>
#include <cstdint>

#define CONFIG_HTTPD_WS_SUPPORT 1
#define CONFIG_LWIP_MAX_SOCKETS 16
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = -2,
              ESP_ERR_NO_MEM = -3, HTTPD_SOCK_ERR_FAIL = -4;
using httpd_handle_t = void *;
using httpd_free_ctx_fn_t = void (*)(void *);
using httpd_send_func_t = int (*)(httpd_handle_t, int, const char *, size_t,
                                  int);
using httpd_recv_func_t = int (*)(httpd_handle_t, int, char *, size_t, int);
constexpr int HTTP_GET = 1;
struct httpd_req_t {
  httpd_handle_t handle{};
  int method{};
  void *user_ctx{};
  void *sess_ctx{};
  httpd_free_ctx_fn_t free_ctx{};
  int test_fd{};
};
struct httpd_config_t {
  int server_port{}, task_priority{}, core_id{}, stack_size{},
      max_open_sockets{}, backlog_conn{}, max_uri_handlers{},
      recv_wait_timeout{}, send_wait_timeout{};
  bool lru_purge_enable{};
  esp_err_t (*open_fn)(httpd_handle_t, int){};
};
#define HTTPD_DEFAULT_CONFIG()                                                 \
  httpd_config_t {}
struct httpd_uri_t {
  const char *uri{};
  int method{};
  esp_err_t (*handler)(httpd_req_t *){};
  void *user_ctx{};
  bool is_websocket{}, handle_ws_control_frames{};
};
enum httpd_ws_type_t {
  HTTPD_WS_TYPE_CONTINUE = 0,
  HTTPD_WS_TYPE_TEXT = 1,
  HTTPD_WS_TYPE_CLOSE = 8,
  HTTPD_WS_TYPE_PING = 9,
  HTTPD_WS_TYPE_PONG = 10
};
struct httpd_ws_frame_t {
  bool final{}, fragmented{};
  httpd_ws_type_t type{};
  uint8_t *payload{};
  size_t len{};
};
enum httpd_ws_client_info_t {
  HTTPD_WS_CLIENT_INVALID,
  HTTPD_WS_CLIENT_HTTP,
  HTTPD_WS_CLIENT_WEBSOCKET
};
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_queue_work(httpd_handle_t, void (*)(void *), void *);
int httpd_req_to_sockfd(httpd_req_t *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *,
                                      size_t);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, size_t);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
esp_err_t httpd_sess_set_send_override(httpd_handle_t, int, httpd_send_func_t);
esp_err_t httpd_sess_set_recv_override(httpd_handle_t, int, httpd_recv_func_t);
void httpd_sess_set_transport_ctx(httpd_handle_t, int, void *,
                                  httpd_free_ctx_fn_t);
void *httpd_sess_get_transport_ctx(httpd_handle_t, int);
void *httpd_sess_get_ctx(httpd_handle_t, int);
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t, int);
esp_err_t httpd_ws_recv_frame(httpd_req_t *, httpd_ws_frame_t *, size_t);
esp_err_t httpd_ws_send_frame(httpd_req_t *, httpd_ws_frame_t *);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int, httpd_ws_frame_t *);
const char *esp_err_to_name(esp_err_t);
