// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FirmwareIdentity.h"

#if defined(MESHCORE_ONCHIP) && MESHCORE_ONCHIP && defined(MAX_CONTACTS)
static_assert(MAX_CONTACTS > 0 && MAX_CONTACTS % 2 == 0 && MAX_CONTACTS / 2 <= 255,
              "Companion device-info contact capacity must fit its half-count byte");
#endif

#ifndef ONCHIP_REPEATER_NAME
#define ONCHIP_REPEATER_NAME "MeshCore repeater"
#endif
#ifndef ONCHIP_ROOM_NAME
#define ONCHIP_ROOM_NAME "MeshCore room"
#endif
#ifndef ONCHIP_COMPANION_NAME
#define ONCHIP_COMPANION_NAME "MeshCore companion"
#endif
#ifndef ONCHIP_OBSERVER_NAME
#define ONCHIP_OBSERVER_NAME "MeshCore observer"
#endif
#ifndef ONCHIP_BOT_NAME
#define ONCHIP_BOT_NAME "MeshCore KISS"
#endif
#ifndef ONCHIP_MANAGEMENT_NAME
#define ONCHIP_MANAGEMENT_NAME "MeshCore management"
#endif
#ifndef ONCHIP_COMMAND_BOT_NAME
#define ONCHIP_COMMAND_BOT_NAME "MeshCore command bot"
#endif
#ifndef ONCHIP_OPERATOR_PUBKEY
#define ONCHIP_OPERATOR_PUBKEY ""
#endif
#ifndef ONCHIP_UPDATE_TARGET
#define ONCHIP_UPDATE_TARGET "xiao-esp32s3"
#endif
constexpr bool validUpdateTarget(const char *text) {
  return !*text || (((*text >= 'a' && *text <= 'z') || (*text >= '0' && *text <= '9') ||
                    *text == '-') && validUpdateTarget(text + 1));
}
static_assert(sizeof(ONCHIP_UPDATE_TARGET) > 1 && sizeof(ONCHIP_UPDATE_TARGET) <= 64 &&
              ONCHIP_UPDATE_TARGET[0] != '-' && validUpdateTarget(ONCHIP_UPDATE_TARGET),
              "ONCHIP_UPDATE_TARGET must be 1..63 lowercase letters, digits or hyphens");
#ifndef ONCHIP_TRUSTED_COMPANION_PUBKEY
#define ONCHIP_TRUSTED_COMPANION_PUBKEY ""
#endif
#ifndef ONCHIP_ADMIN_PASSWORD
#define ONCHIP_ADMIN_PASSWORD ""
#endif
#ifndef ONCHIP_MAST_PASSWORD
#define ONCHIP_MAST_PASSWORD ""
#endif
#ifndef ONCHIP_MAST_WEB_HOST
#define ONCHIP_MAST_WEB_HOST ""
#endif
#ifndef ONCHIP_SERVICE_REGION
#define ONCHIP_SERVICE_REGION ""
#endif
static_assert(sizeof(ONCHIP_MAST_PASSWORD) <= 16, "Mast password must be at most 15 bytes");
static_assert(sizeof(ONCHIP_SERVICE_REGION) <= 33, "Service region name must be at most 32 bytes");
#ifndef ONCHIP_ROOM_PASSWORD
#define ONCHIP_ROOM_PASSWORD ""
#endif
#ifndef ONCHIP_COMPANION_PORT
#define ONCHIP_COMPANION_PORT 5000
#endif
#ifndef ONCHIP_MQTT_URI
#define ONCHIP_MQTT_URI ""
#endif
#ifndef ONCHIP_MQTT_TOPIC_PREFIX
#define ONCHIP_MQTT_TOPIC_PREFIX "meshcore"
#endif
// 0: internal events, 1: observer firmware, 2: packet-capture wire dialect.
#ifndef ONCHIP_MQTT_FORMAT
#define ONCHIP_MQTT_FORMAT 0
#endif
#ifndef ONCHIP_MQTT_IATA
#define ONCHIP_MQTT_IATA ""
#endif
#ifndef ONCHIP_MQTT_AUDIENCE
#define ONCHIP_MQTT_AUDIENCE ""
#endif
#ifndef ONCHIP_MQTT_CA_PEM
#define ONCHIP_MQTT_CA_PEM ""
#endif
#ifndef ONCHIP_MQTT_PACKET_FILTER
#define ONCHIP_MQTT_PACKET_FILTER 0xffff
#endif
#ifndef ONCHIP_MQTT_MODEL
#define ONCHIP_MQTT_MODEL "MeshCore shared modem"
#endif
#ifndef ONCHIP_MQTT_FIRMWARE_VERSION
#define ONCHIP_MQTT_FIRMWARE_VERSION ONCHIP_FIRMWARE_VERSION
#endif
static_assert(ONCHIP_MQTT_FORMAT >= 0 && ONCHIP_MQTT_FORMAT <= 2,
              "MQTT format must be 0 (internal), 1 (observer) or 2 (capture)");
static_assert(ONCHIP_MQTT_PACKET_FILTER >= 0 &&
                  ONCHIP_MQTT_PACKET_FILTER <= 0xffff,
              "MQTT packet filter must be a 16-bit payload-type mask");
#ifndef ONCHIP_SNTP_SERVER
#define ONCHIP_SNTP_SERVER "pool.ntp.org"
#endif
#ifndef ONCHIP_SNTP_INTERVAL_SECONDS
#define ONCHIP_SNTP_INTERVAL_SECONDS 3600
#endif
static_assert(ONCHIP_SNTP_INTERVAL_SECONDS >= 60 &&
                  ONCHIP_SNTP_INTERVAL_SECONDS <= 86400,
              "SNTP interval must be 60..86400 seconds");

static_assert(sizeof(ONCHIP_REPEATER_NAME) <= 32 &&
                  sizeof(ONCHIP_ROOM_NAME) <= 32 &&
                  sizeof(ONCHIP_COMPANION_NAME) <= 32 &&
                  sizeof(ONCHIP_OBSERVER_NAME) <= 32 &&
                  sizeof(ONCHIP_BOT_NAME) <= 32 &&
                  sizeof(ONCHIP_COMMAND_BOT_NAME) <= 32 &&
                  sizeof(ONCHIP_MANAGEMENT_NAME) <= 32,
              "Role names are at most 31 bytes");
static_assert(ONCHIP_COMPANION_PORT > 0 && ONCHIP_COMPANION_PORT <= 65535,
              "Companion TCP port must be 1..65535");

#include "Provisioning.h"
namespace onchip {
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
static_assert(sizeof(ONCHIP_ADMIN_PASSWORD) == 1 && sizeof(ONCHIP_ROOM_PASSWORD) == 1 &&
              sizeof(ONCHIP_MAST_PASSWORD) == 1 && sizeof(ONCHIP_OPERATOR_PUBKEY) == 1 &&
              sizeof(ONCHIP_TRUSTED_COMPANION_PUBKEY) == 1 && sizeof(ONCHIP_MQTT_URI) == 1,
              "Public Aspen builds must not embed passwords, MQTT credentials or operator authorities");
#ifdef WIFI_SSID
static_assert(sizeof(WIFI_SSID) == 1, "Public Aspen builds must not embed a WiFi SSID");
#endif
#ifdef WIFI_PWD
static_assert(sizeof(WIFI_PWD) == 1, "Public Aspen builds must not embed a WiFi password");
#endif
inline const char *adminPassword() { return publicProvisioning().adminPassword; }
inline const char *roomPassword() { return publicProvisioning().roomPassword; }
inline const char *mastPassword() { return publicProvisioning().mastPassword; }
inline const char *operatorPublicKey() { return publicProvisioning().operatorPublicKey; }
inline const char *trustedCompanionPublicKey() { return publicProvisioning().trustedCompanionPublicKey; }
#else
inline bool publicProvisioningReady() { return true; }
inline const char *adminPassword() { return ONCHIP_ADMIN_PASSWORD; }
inline const char *roomPassword() { return ONCHIP_ROOM_PASSWORD; }
inline const char *mastPassword() { return ONCHIP_MAST_PASSWORD; }
inline const char *operatorPublicKey() { return ONCHIP_OPERATOR_PUBKEY; }
inline const char *trustedCompanionPublicKey() { return ONCHIP_TRUSTED_COMPANION_PUBKEY; }
#endif
} // namespace onchip
