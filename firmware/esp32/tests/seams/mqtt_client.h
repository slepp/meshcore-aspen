#pragma once
#include "nvs.h"
#include <string>
#include <vector>
using esp_event_base_t = const char *;
constexpr int MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED = 1;
constexpr int MQTT_EVENT_DISCONNECTED = 2, MQTT_EVENT_ERROR = 3;
struct esp_mqtt_client_config_t {
  const char *uri = nullptr, *client_id = nullptr, *lwt_topic = nullptr,
             *lwt_msg = nullptr;
  const char *username = nullptr, *password = nullptr, *cert_pem = nullptr;
  int lwt_qos = 0, lwt_retain = 0, buffer_size = 0;
  int network_timeout_ms = 0, reconnect_timeout_ms = 0;
};
struct TestMQTT {
  std::string identity, willTopic, will;
  bool retain;
  void (*event)(void *, esp_event_base_t, int32_t, void *) = nullptr;
  void *context = nullptr;
  std::string username, password, certificate;
  int willQos = 0;
};
using esp_mqtt_client_handle_t = TestMQTT *;
namespace mqtt_test {
struct Message {
  std::string topic, data;
  bool retained;
  int qos = 0;
};
inline std::vector<Message> messages;
inline bool failPublish = false;
inline bool reconnectDuringPublish = false;
} // namespace mqtt_test
inline TestMQTT *esp_mqtt_client_init(const esp_mqtt_client_config_t *c) {
  auto result = new TestMQTT{c->client_id, c->lwt_topic, c->lwt_msg,
                            bool(c->lwt_retain)};
  result->username = c->username ? c->username : "";
  result->password = c->password ? c->password : "";
  result->certificate = c->cert_pem ? c->cert_pem : "";
  result->willQos = c->lwt_qos;
  return result;
}
inline int esp_mqtt_client_register_event(
    TestMQTT *client, int,
    void (*event)(void *, esp_event_base_t, int32_t, void *), void *arg) {
  client->event = event;
  client->context = arg;
  return ESP_OK;
}
inline int esp_mqtt_client_start(TestMQTT *) { return ESP_OK; }
inline int esp_mqtt_client_stop(TestMQTT *) { return ESP_OK; }
inline void esp_mqtt_client_destroy(TestMQTT *client) { delete client; }
inline int esp_mqtt_client_publish(TestMQTT *client, const char *topic,
                                   const char *data, int size, int qos,
                                   int retain) {
  if (mqtt_test::failPublish)
    return -1;
  if (qos < 0 || qos > 1)
    std::abort();
  mqtt_test::messages.push_back({topic, std::string(data, size), bool(retain), qos});
  if (mqtt_test::reconnectDuringPublish) {
    mqtt_test::reconnectDuringPublish = false;
    client->event(client->context, nullptr, MQTT_EVENT_DISCONNECTED, nullptr);
    client->event(client->context, nullptr, MQTT_EVENT_CONNECTED, nullptr);
  }
  return mqtt_test::messages.size();
}
