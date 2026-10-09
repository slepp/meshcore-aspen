#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate isolated native applications in a disposable pinned build tree."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

COMPANION_DASHBOARD_BRIDGE = """
#include "DashboardContacts.h"
namespace onchip {
bool companionContactAdvert(const uint8_t* hash, unsigned& cursor,
                            uint8_t* key, uint8_t* advert, uint8_t& size) {
  size = 0;
  if (!meshInstance || lifecycleBusy(Role::Companion)) return false;
  const int total = meshInstance->getNumContacts();
  if (total < 0 || total > MAX_CONTACTS) {
    Serial.println("Bot contact recovery unavailable: companion contact count invalid");
    return false;
  }
  while (cursor < unsigned(total)) {
    ContactInfo contact{};
    if (!meshInstance->getContactByIdx(MAX_ANON_CONTACTS + cursor++, contact)) {
      Serial.println("Bot contact recovery unavailable: companion contact lookup failed");
      return false;
    }
    if (!contact.id.isHashMatch(hash)) continue;
    memcpy(key, contact.id.pub_key, 32);
    size = meshInstance->exportContact(contact, advert);
    return true;
  }
  return false;
}
void companionDashboardContacts(RadioDashboard::RadioStatus& status) {
#if defined(MESHCORE_ONCHIP) && defined(ESP32) && !defined(NRF52_PLATFORM)
  status.contacts_available = false;
  status.contact_count = status.contact_total = 0;
  if (!meshInstance || lifecycleBusy(Role::Companion)) return;
  copyDashboardContacts<ContactInfo>(*meshInstance, status);
#else
  (void)status;
#endif
}
}
"""


def check_target(upstream, target):
    root = Path(__file__).resolve().parents[2]
    resolved = target.resolve()
    if (resolved in (upstream.resolve(), (root / ".tmp/onchip-upstream").resolve()) or
            resolved.parent != (root / ".tmp").resolve() or
            not resolved.name.startswith(("onchip-", "public-aspen-setup-"))):
        raise ValueError("Build target must be a dedicated .tmp/onchip-* or .tmp/public-aspen-setup-* directory")


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Native source anchor changed: {old[:90]!r}")
    return text.replace(old, new, 1)

def preserve_shared_gps(text):
    return replace_once(text, "\n  applyGpsPrefs();",
                        "\n  // GPS power belongs to the shared modem, including after a role restart.")


def body(text, signature, replacement, closing_indent=""):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    # Native functions selected here contain balanced braces in comments/strings.
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[:opening] + "{\n" + replacement + "\n" + closing_indent + "}" + text[end:]


def bound_owner_info_replies(text):
    anchor = "      if (reply_len == 0) return; // invalid command"
    signature = "      if (data[4] == REQ_TYPE_GET_OWNER_INFO)"
    bounds = """        const size_t owner_info_capacity = packet->isRouteFlood() ?
            MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE - 5 - packet->getPathByteLen() :
            MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1);
        if (size_t(reply_len) > owner_info_capacity) {
          size_t end = owner_info_capacity;
          while (end > 4 && (reply_data[end] & 0xc0) == 0x80) --end;
          reply_data[end] = 0;
          reply_len = int(end);
          Serial.println("Owner information shortened for native reply; use get owner.info for full text");
        }
"""
    bounds = bounds.rstrip("\n")
    if signature in text:
        return body(text, signature, bounds, closing_indent="      ")
    return replace_once(text, anchor, anchor + "\n" + signature + " {\n" + bounds + "\n      }\n")

def stage_packet_pool(upstream, dest):
    support = dest / "support"
    support.mkdir(exist_ok=True)
    pool_h = (upstream / "src/helpers/StaticPoolPacketManager.h").read_text()
    pool_h = replace_once(pool_h, "mesh::Packet** _table;",
                          "mesh::Packet* _table[ONCHIP_PACKET_CAPACITY];")
    pool_h = replace_once(pool_h, "uint8_t* _pri_table;",
                          "uint8_t _pri_table[ONCHIP_PACKET_CAPACITY];")
    pool_h = replace_once(pool_h, "uint32_t* _schedule_table;",
                          "uint32_t _schedule_table[ONCHIP_PACKET_CAPACITY];")
    pool_h = replace_once(pool_h, "PacketQueue(int max_entries);",
                          "PacketQueue(int max_entries);\n  void clear() { _num = 0; }")
    pool_h = replace_once(pool_h, "PacketQueue unused, send_queue, rx_queue;",
                          "PacketQueue unused, send_queue, rx_queue;\n  mesh::Packet packets[ONCHIP_PACKET_CAPACITY];")
    pool_h = replace_once(pool_h, "StaticPoolPacketManager(int pool_size);",
                          "StaticPoolPacketManager(int pool_size);\n  void reset();")
    pool_cpp = (upstream / "src/helpers/StaticPoolPacketManager.cpp").read_text()
    pool_cpp = pool_cpp.replace('#include "StaticPoolPacketManager.h"', '#include "PacketPool.h"')
    pool_cpp = body(pool_cpp, "PacketQueue::PacketQueue(",
                    "  assert(max_entries == ONCHIP_PACKET_CAPACITY);\n  _size = max_entries;\n  _num = 0;")
    pool_cpp = body(pool_cpp, "StaticPoolPacketManager::StaticPoolPacketManager(", "  reset();")
    pool_cpp += """
void StaticPoolPacketManager::reset() {
  unused.clear();
  send_queue.clear();
  rx_queue.clear();
  for (auto& packet : packets) {
    packet = {};
    unused.add(&packet, 0, 0);
  }
}
"""
    (support / "PacketPool.h").write_text(pool_h)
    (support / "PacketPool.inc").write_text(pool_cpp)


def stage_field_network(target):
    source = Path(__file__).resolve().parent
    sys.path.insert(0, str(source.parent / "shared"))
    from gps_time import stage_gps_time
    stage_gps_time(target)
    shutil.copy2(source / "wifi_kiss_main.cpp",
                 target / "examples/kiss_modem/main.cpp")
    shutil.copy2(source.parent / "shared/RadioFirmwareIdentity.h",
                 target / "examples/kiss_modem/RadioFirmwareIdentity.h")
    for name in ("SntpConfig.h", "EspSntpClock.h", "RadioTimeProtocol.h"):
        shutil.copy2(source.parent / "shared" / name,
                     target / "examples/kiss_modem" / name)
        role_headers = target / "examples/kiss_modem/onchip"
        if role_headers.is_dir():
            shutil.copy2(source.parent / "shared" / name, role_headers / name)
    shutil.copy2(source / "FirmwareIdentity.h",
                 target / "examples/kiss_modem/FirmwareIdentity.h")
    # Keep the clock resource on the existing HTTP server.
    dashboard = (source.parent / "shared/RadioDashboard.cpp").read_text()
    dashboard = '#include "onchip/Clock.h"\n' + dashboard
    dashboard = replace_once(dashboard, "config.max_uri_handlers = 3;",
                             "config.max_uri_handlers = 4;")
    dashboard = replace_once(dashboard, "    if (error == ESP_OK &&\n",
                             "    if (error == ESP_OK) error = onchip::registerClockHTTP(_server);\n"
                             "    if (error == ESP_OK &&\n")
    (target / "examples/kiss_modem/RadioDashboard.cpp").write_text(dashboard)


def generate(upstream, target):
    check_target(upstream, target)
    source = Path(__file__).resolve().parent
    dest = target / "examples/kiss_modem/onchip"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("SntpConfig.h", "EspSntpClock.h", "RadioTimeProtocol.h"):
        shutil.copy2(source.parent / "shared" / name, dest / name)
    data = target / "onchip-data"
    data.mkdir(exist_ok=True)
    shutil.copy2(source / "data/onchip-layout", data / "onchip-layout")
    for obsolete in ("CompanionServer.cpp", "CompanionServer.h",
                     "OnchipStatsFormatHelper.h"):
        (dest / obsolete).unlink(missing_ok=True)
    for directory in (source, source.parent / "runtime"):
        for path in directory.iterdir():
            if path.suffix in (".h", ".cpp") and path.name != "wifi_kiss_main.cpp":
                shutil.copy2(path, dest / path.name)
    shutil.copytree(source.parent / "runtime/wasm/sdk", dest / "wasm/sdk", dirs_exist_ok=True)
    shutil.copytree(source.parent / "shared/cloudroom", dest / "cloudroom", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("tests", "Makefile"))
    mesh_path = target / "src/Mesh.cpp"
    mesh = mesh_path.read_text()
    guard = "                if (2u + hash_size * hash_count > unsigned(len)) break;"
    if guard not in mesh:
        mesh = replace_once(mesh, "                uint8_t hash_count = path_len & 63;",
                            "                uint8_t hash_count = path_len & 63;\n" + guard)
        mesh_path.write_text(mesh)
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", datetime.now(timezone.utc).timestamp()))
    baseline = epoch - epoch % 86400
    if not 1715770351 <= baseline <= 4102444800:
        raise ValueError("Build clock baseline must be between May 2024 and January 2100")
    (dest / "BuildClock.h").write_text(
        "// Build-day clock baseline for offline startup.\n"
        "#pragma once\n#ifndef ONCHIP_CLOCK_BUILD_EPOCH\n"
        f"#define ONCHIP_CLOCK_BUILD_EPOCH {baseline}u\n#endif\n"
        '#define ONCHIP_NATIVE_REVISION "' +
        subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"],
                                text=True).strip()[:12] + '"\n')
    stage_field_network(target)
    stage_packet_pool(target, dest)
    for role, example in (("repeater", "simple_repeater"),
                          ("room", "simple_room_server"),
                          ("companion", "companion_radio")):
        native = dest / role
        native.mkdir(exist_ok=True)
        for path in (target / "examples" / example).iterdir():
            if path.name in ("main.cpp", "UITask.cpp", "UITask.h") or not path.is_file():
                continue
            text = path.read_text()
            if role != "companion" and path.name == "MyMesh.cpp":
                text = preserve_shared_gps(text)
            text = re.sub(r"\bMyMesh\b", f"Onchip{role.title()}", text)
            if role == "companion":
                text = re.sub(r"\bNodePrefs\b", "OnchipCompanionPrefs", text)
            if role != "companion" and path.name == "MyMesh.h":
                anchor = "  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);"
                text = replace_once(text, anchor, anchor + "\n  bool onchipFlush();\n  bool onchipSavePrefs();\n  bool onchipAdvertise(bool zeroHop);")
            if role == "companion" and path.name == "MyMesh.h":
                text = replace_once(text, "  void begin(bool has_display);",
                                    """  void begin(bool has_display);
  bool onchipFlush();
  bool onchipSavePrefs();
  bool onchipAdvertise(bool zeroHop);
  int searchChannelsByHash(const uint8_t*, mesh::GroupChannel[], int) override;
  void onchipRequestFailed();""")
                text = replace_once(text, "void saveChannels() { _store->saveChannels(this); }",
                                    "bool saveChannels() { return _store->saveChannels(this); }")
            if path.name == "MyMesh.cpp":
                cls = f"Onchip{role.title()}"
                voltage = "telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);"
                if voltage not in text:
                    raise ValueError(f"Pinned {role} battery telemetry hook changed")
                text = text.replace(voltage, "{ const auto battery = board.getBattMilliVolts(); "
                                    "if (battery) telemetry.addVoltage(TELEM_CHANNEL_SELF, battery / 1000.0f); }")
                text = '#include "../FirmwareIdentity.h"\n' + text
                text = text.replace("FIRMWARE_VERSION", "ONCHIP_FIRMWARE_VERSION")
                if role == "repeater":
                    text = bound_owner_info_replies(text)
                pool_size = 16 if role == "companion" else 32
                text = replace_once(text, f"*new StaticPoolPacketManager({pool_size})",
                                    f"onchip::{role}Packets()")
                if role == "companion":
                    text = replace_once(text, "*new ArduinoMillis()", "onchip::companionMillis()")
                text = text.replace("_mgr->getOutboundTotal() > 0",
                                    f"onchip::{role}Radio().hasPendingWork()")
                text = text.replace("_mgr->getOutboundTotal()",
                                    f"onchip::{role}Radio().queuedCount()")
                text = text.replace(
                    "formatCoreStats(reply, board, *_ms, _err_flags, _mgr)",
                    f"formatCoreStats(reply, board, *_ms, _err_flags, onchip::{role}Radio().queuedCount())")
                # Native begin() applies persisted device settings. The shared
                # arbiter alone owns these; role preferences cannot reconfigure it.
                for function in ("setParams", "setTxPower", "setRxBoostedGainMode"):
                    text = re.sub(r"^ +radio_driver\." + function + r"\([^\n]*\);\n",
                                  "", text, flags=re.M)
                text = re.sub(r"^  board\.(?:setLoRaFemLnaEnabled|setLoRaFemPaGainEnabled|setAdcMultiplier)\([^\n]*\);\n",
                              "", text, flags=re.M)
                if role == "companion":
                    text = replace_once(text,
                        '  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",\n'
                        '                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");',
                        '  // Physical RX gain is owned and reported by the shared modem.')
                if role != "companion":
                    for timer in ("next_flood_advert", "next_local_advert"):
                        text = replace_once(
                            text, f"if ({timer} && millisHasNowPassed({timer}))",
                            f"if (onchip::automaticAdvertsEnabled() && {timer} && millisHasNowPassed({timer}))")
                    text = body(text, f"bool {cls}::formatFileSystem()",
                                '  Serial.println("Use the local deferred role erase command");\n  return false;')
                    text = body(text, f"void {cls}::setTxPower(", '  Serial.println("Role TX power changes are unsupported");')
                    text = body(text, f"bool {cls}::setRxBoostedGain(", "  return false;")
                    text = body(text, f"void {cls}::applyTempRadioParams(",
                                '  Serial.println("Role radio changes are unsupported");')
                    text = body(text, f"void {cls}::saveIdentity(",
                                f"  onchip::stageIdentity(onchip::Role::{role.title()}, new_id);")
                    # These branches are unreachable after rejecting tempradio.
                    text = re.sub(r"^    radio_driver\.setParams\([^\n]*\);\n",
                                  "", text, flags=re.M)
                    text = text.replace("radio_driver.resetStats();", "")
                    anchor = "  // handle ACL related commands"
                    text = replace_once(text, anchor, f"""
  if (onchip::networkClockCommand(command, reply, 160, false)) return;
  if (!strcmp(command, "ver")) {{
    snprintf(reply, 160, "%s (Build: %s)", ONCHIP_FIRMWARE_VERSION, __DATE__);
    return;
  }}
  if (onchip::lifecycleCommand(onchip::Role::{role.title()}, sender_timestamp, command, reply)) return;
""" + f"""
#if !defined(NRF52_PLATFORM)
  if (onchip::sharedRadioReadCommand(onchip::{role}Radio(), command, reply, 160)) return;
#endif
""" + """
  if (onchip::clearsAdminPassword(command)) {
    strcpy(reply, "Error: administrator password must not be empty");
    return;
  }
  if (onchip::unsafeCLI(command)) {
    strcpy(reply, "Error: unsupported on shared device");
    return;
  }
""" + anchor)
                    if role == "repeater":
                        text = '#include "Clock.h"\n#include "RadioTimeProtocol.h"\n' + text
                        gate = "type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()"
                        guest_gate = """type == PAYLOAD_TYPE_TXT_MSG && len > 5 &&
      (client->isAdmin() || onchip::publicTimeCommand(data, len))"""
                        text = replace_once(text, gate, guest_gate)
                        hook = """  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) {"""
                        public_time = """  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ &&
      packet->isRouteDirect() && packet->getPathHashCount() == 0 &&
      onchip::networkTimeReply(data, len, reply_data)) {
    static uint32_t timeReplyAt = 0;
    static bool timeReplySent = false;
    const uint32_t now = millis();
    if (timeReplySent && uint32_t(now - timeReplyAt) < 10000u) return;
    if (_mgr->getOutboundTotal() || _radio->getEstAirtimeFor(80) > radio_time::MaxAirtimeMs) return;
    timeReplySent = true; timeReplyAt = now;
    auto *reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret,
                                 reply_data, radio_time::ResponseSize);
    if (reply) sendZeroHop(reply, uint32_t(300));
    return;
  }
"""
                        text = replace_once(text, hook, public_time + hook)
                    text += f"""
bool {cls}::onchipFlush() {{
  if (!_cli.savePrefs(_fs)) return false;
  acl.save(_fs{", saveFilter" if role == "room" else ""});
  return true;
}}
bool {cls}::onchipSavePrefs() {{ return _cli.savePrefs(_fs); }}
bool {cls}::onchipAdvertise(bool zeroHop) {{
  auto* packet = createSelfAdvert();
  if (!packet) return false;
  if (zeroHop) sendZeroHop(packet);
  else sendFloodScoped(default_scope, packet, 0, _prefs.path_hash_mode + 1);
  return true;
}}
"""
                else:
                    text = replace_once(text, "  bootstrapRTCfromContacts();",
                                        "  // Use the role clock; contact timestamps may be stale.")
                    text = re.sub(r"^ +sensors\.node_(lat|lon) = _prefs\.node_\1;\n", "", text, flags=re.M)
                    text = text.replace("sensors.node_lat", "_prefs.node_lat")
                    text = text.replace("sensors.node_lon", "_prefs.node_lon")
                    text = re.sub(r"^ +_prefs\.node_(lat|lon) = _prefs\.node_\1;\n", "", text, flags=re.M)
                    text = body(text, f"void {cls}::checkCLIRescueCmd()",
                                '  Serial.println("On-chip serial rescue is disabled");\n  _cli_rescue = false;')
                    # Runtime loads a checked, durable role identity before begin.
                    first = text.index("  if (!_store->loadMainIdentity(self_id))")
                    last = text.index("// if name is provided", first)
                    text = text[:first] + text[last:]
                    anchor = f"void {cls}::handleCmdFrame(size_t len) {{"
                    text = replace_once(text, anchor, anchor + """
  const auto lifecycle = onchip::companionLifecycleCommand(cmd_frame, len);
  if (lifecycle != onchip::CompanionLifecycle::Unhandled) {
    if (lifecycle == onchip::CompanionLifecycle::BadArgument) writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    if (lifecycle == onchip::CompanionLifecycle::Busy) writeErrFrame(ERR_CODE_BAD_STATE);
    return;
  }
  if (onchip::unsafeCompanion(cmd_frame[0])) {
    writeDisabledFrame();
    return;
  }""")
                    text = text.replace("board.reboot();",
                                        "onchip::requestLifecycle(onchip::Role::Companion, onchip::LifecycleAction::Reboot);")
                    text = replace_once(text, "if (success && sendGroupMessage(",
                                        "if (success && channel.name[0] && sendGroupMessage(")
                    text = replace_once(text, "if (!getChannel(channel_idx, channel)) {",
                                        "if (!getChannel(channel_idx, channel) || !channel.name[0]) {")
                    text = replace_once(text, "      saveChannels();\n      writeOKFrame();",
                                        "      if (saveChannels()) writeOKFrame();\n      else writeErrFrame(ERR_CODE_FILE_IO_ERROR);")
                    text += """
bool OnchipCompanion::onchipFlush() {
  if (!_store->savePrefs(_prefs)) return false;
  if (!_store->saveContacts(this, save_filter)) return false;
  if (!_store->saveChannels(this)) return false;
  dirty_contacts_expiry = 0;
  return true;
}
void OnchipCompanion::onchipRequestFailed() {
  writeErrFrame(ERR_CODE_FILE_IO_ERROR);
}
bool OnchipCompanion::onchipSavePrefs() { return _store->savePrefs(_prefs); }
bool OnchipCompanion::onchipAdvertise(bool zeroHop) {
  auto* packet = _prefs.advert_loc_policy == ADVERT_LOC_NONE ?
      createSelfAdvert(_prefs.node_name) :
      createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon);
  if (!packet) return false;
  if (zeroHop) sendZeroHop(packet);
  else {
    TransportKey default_scope;
    memcpy(default_scope.key, _prefs.default_scope_key, sizeof(default_scope.key));
    sendFloodScoped(default_scope, packet, 0);
  }
  return true;
}
int OnchipCompanion::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int maximum) {
  int count = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS && count < maximum; ++i) {
    ChannelDetails channel;
    if (getChannel(i, channel) && channel.name[0] && channel.channel.hash[0] == hash[0])
      dest[count++] = channel.channel;
  }
  return count;
}
"""
                text = text.replace("radio_driver.getLast", "radio_driver.physical().getLast")
                text = text.replace("StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver,",
                                    "StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver.physical(),")
                text = text.replace("_radio->getLastSNR()", "onchip::nativeSNR(*_radio)")
                text = text.replace("_radio->getLastRSSI()", "onchip::nativeRSSI(*_radio)")
                text = text.replace("radio_driver", f"onchip::{role}Radio()")
                text = text.replace("rtc_clock", f"onchip::{role}Clock()")
            if role == "companion" and path.name == "DataStore.cpp":
                text = body(text, "bool DataStore::formatFileSystem()", "  return false;")
                start = text.index("void DataStore::saveContacts(")
                end = text.index("\nvoid DataStore::loadChannels(", start)
                save = text[start:end].replace("void DataStore::saveContacts(", "bool DataStore::saveContacts(")
                save = replace_once(save, "      if (!success) break; // write failed",
                                    "      if (!success) { file.close(); return false; }")
                save = replace_once(save, "    file.close();\n  }\n}",
                                    "    file.flush();\n    file.close();\n    return true;\n  }\n  return false;\n}")
                text = text[:start] + save + text[end:]
                start = text.index("void DataStore::saveChannels(")
                end = text.index("\n#if defined(NRF52_PLATFORM)", start)
                save = text[start:end].replace("void DataStore::saveChannels(", "bool DataStore::saveChannels(")
                save = replace_once(save, "      if (!success) break; // write failed",
                                    "      if (!success) { file.close(); return false; }")
                save = replace_once(save, "    file.close();\n  }\n}",
                                    "    file.flush();\n    file.close();\n    return true;\n  }\n  return false;\n}")
                text = text[:start] + save + text[end:]
            if role == "companion" and path.name == "DataStore.h":
                text = replace_once(text, "void saveContacts(", "bool saveContacts(")
                text = replace_once(text, "void saveChannels(", "bool saveChannels(")
            # Each native application is included by exactly one wrapper TU.
            name = path.name.replace("MyMesh", f"Onchip{role.title()}")
            if role == "companion":
                name = name.replace("NodePrefs", "OnchipCompanionPrefs")
            (native / name).write_text(text)
        wrapper = f"""// Generated from pinned native application; original notices remain in {role}/.
#include "Runtime.h"
#include "RoleProfile.h"
#include "ScopedFS.h"
#include "CommandPolicy.h"
#include "CompanionSessions.h"
#include "RoleBoard.h"
#include "RoleStorage.h"
#include "Config.h"
#include <SPIFFS.h>
#include <target.h>
#include "PhyConfig.h"
#include <cassert>
#include <new>
#undef ADVERT_NAME
#define ADVERT_NAME ONCHIP_{role.upper()}_NAME
#undef ADMIN_PASSWORD
#define ADMIN_PASSWORD onchip::adminPassword()
namespace onchip {{
LocalRadio& {role}Radio() {{ static LocalRadio radio; return radio; }}
namespace {role}_pool {{
#define ONCHIP_PACKET_CAPACITY {16 if role == "companion" else 32}
#include "support/PacketPool.h"
#include "support/PacketPool.inc"
#undef ONCHIP_PACKET_CAPACITY
}}
static ArduinoMillis milliseconds;
{"mesh::MillisecondClock& companionMillis() { return milliseconds; }" if role == "companion" else ""}
}}
#include "{role}/Onchip{role.title()}.cpp"
"""
        if role == "companion":
            wrapper += '#include "companion/DataStore.cpp"\n'
        wrapper += f"""
namespace onchip {{
static ScopedFS roleFS(SPIFFS, "/{role}");
static HardwareRNG rng;
static ScopedErase eraser(SPIFFS, "/{role}");
static RoleBoard roleBoard(board, Role::{role.title()});
struct {role.title()}Storage {{
  SimpleMeshTables tables;
  {role}_pool::StaticPoolPacketManager packets{{{16 if role == "companion" else 32}}};
  alignas(Onchip{role.title()}) uint8_t mesh[sizeof(Onchip{role.title()})];
}};
static {role.title()}Storage* storage;
static Onchip{role.title()}* meshInstance;
mesh::PacketManager& {role}Packets() {{ assert(storage); return storage->packets; }}
static void report() {{
  if (!meshInstance) return;
  mesh::QueuedRadioStats stats;
  const bool known = {role}Radio().getQueuedRadioStats(stats);
  reportRoleIdentity(Role::{role.title()}, meshInstance->getNodePrefs()->node_name,
                     meshInstance->self_id.pub_key, {role}Radio().sourceSlot(),
                     known ? stats.generation : 0, {role}Radio().queuedReady());
}}
"""
        if role == "companion":
            wrapper += f"""static DataStore store(roleFS, {role}Clock());
static void construct() {{
  meshInstance = new (storage->mesh) OnchipCompanion({role}Radio(), rng, {role}Clock(), storage->tables, store);
}}
"""
        else:
            wrapper += f"""static void construct() {{
  meshInstance = new (storage->mesh) Onchip{role.title()}(roleBoard, {role}Radio(), milliseconds, rng, {role}Clock(), storage->tables);
}}
"""
        wrapper += f"""bool {role}Begin(WifiKissMultiplexer& mux) {{
  if (!publicProvisioningReady()) {{
    Serial.println("Public setup unavailable; native role startup refused");
    return false;
  }}
  if (storage) return false;
  storage = allocateRoleStorage<{role.title()}Storage>("{role}");
  if (!storage) return false;
  if (!{role}Radio().attach(mux)) {{
    releaseRoleStorage(storage);
    return false;
  }}
  construct();
  auto& mesh = *meshInstance;
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  mesh.getNodePrefs()->path_hash_mode = publicProvisioning().pathWidth - 1;
#endif
  if (!loadIdentity("{role}", mesh.self_id)) {{ {role}Stop(); return false; }}
"""
        if role == "companion":
            wrapper += """  store.begin();
  mesh.startInterface(companionSessions());
  mesh.begin(false);
"""
        else:
            if role == "room":
                wrapper += '  StrHelper::strncpy(mesh.getNodePrefs()->guest_password, onchip::roomPassword(), 16);\n'
            wrapper += """  mesh.begin(&roleFS);
  if (!mesh.getNodePrefs()->password[0]) {
    Serial.println("On-chip role has an empty persisted administrator password; startup refused");
    return false;
  }
"""
        wrapper += f"  reflectSharedConfiguration(*mesh.getNodePrefs(), {role}Radio());\n"
        wrapper += "  report();\n"
        if role != "companion":
            wrapper += "  if (automaticAdvertsEnabled()) mesh.sendSelfAdvertisement(16000, false);\n"
        wrapper += f"""  return true;
}}
void {role}Loop() {{
  if (!meshInstance) return;
  reflectSharedConfiguration(*meshInstance->getNodePrefs(), {role}Radio());
  {role}Clock().tick();
  meshInstance->loop();
  report();
}}
bool {role}Flush() {{
  roleFS.clearErrors();
  return meshInstance && meshInstance->onchipFlush() && !roleFS.failed();
}}
uint8_t {role}PathWidth() {{
  return meshInstance ? meshInstance->getNodePrefs()->path_hash_mode + 1 : 0;
}}
bool {role}SetPathWidth(uint8_t width) {{
  if (!meshInstance || width < 1 || width > 3) return false;
  meshInstance->getNodePrefs()->path_hash_mode = width - 1;
  return {role}Flush();
}}
bool {role}Name(char name[32]) {{
  if (!meshInstance) return false;
  snprintf(name, 32, "%s", meshInstance->getNodePrefs()->node_name);
  return true;
}}
bool {role}Advertise(bool zeroHop) {{
  return meshInstance && {role}Radio().queuedReady() &&
      !{role}Radio().hasPendingWork() && !storage->packets.getOutboundTotal() &&
      meshInstance->onchipAdvertise(zeroHop);
}}
bool {role}SetName(const char* name) {{
  if (!meshInstance || !name || !name[0] || strlen(name) > 31) return false;
  char before[32];
  auto& prefs = *meshInstance->getNodePrefs();
  memcpy(before, prefs.node_name, sizeof(before));
  strcpy(prefs.node_name, name);
  roleFS.clearErrors();
  if (!meshInstance->onchipSavePrefs() || roleFS.failed()) {{
    memcpy(prefs.node_name, before, sizeof(before));
    Serial.println("Role name persistence failed; saved state may be uncertain");
    return false;
  }}
  report();
  return true;
}}
void {role}Stop() {{
  {role}Radio().detach();
  retireRoleSource(Role::{role.title()});
  {"companionSessions().resetNativeSession();" if role == "companion" else ""}
  if (meshInstance) {{
    meshInstance->~Onchip{role.title()}();
    meshInstance = nullptr;
  }}
  releaseRoleStorage(storage);
}}
bool {role}EraseStep(bool& done) {{ return eraser.step(done); }}
{"void companionRequestFailed() { if (meshInstance) meshInstance->onchipRequestFailed(); }" if role == "companion" else ""}
{"int roomGuestAccess() { return !meshInstance ? -1 : meshInstance->getNodePrefs()->guest_password[0] ? 0 : 1; }" if role == "room" else ""}
}}
"""
        if role in ("repeater", "room"):
            wrapper += f"""
namespace onchip {{
RolePasswordUpdate {role}SetPassword(const char* password) {{
  if (!meshInstance) return RolePasswordUpdate::Unavailable;
  auto& prefs = *meshInstance->getNodePrefs();
  char before[sizeof(prefs.password)];
  memcpy(before, prefs.password, sizeof(before));
  memset(prefs.password, 0, sizeof(prefs.password));
  strcpy(prefs.password, password);
  roleFS.clearErrors();
  const bool written = meshInstance->onchipSavePrefs() && !roleFS.failed();
  NodePrefs readback{{}};
  auto file = roleFS.open("/prefs.json", "r");
  const bool verified = file && readback.loadSerial(file) &&
      !strcmp(readback.password, password) && !roleFS.failed();
  file.close();
  const auto clear = [](char* value, size_t size) {{
    auto* secret = reinterpret_cast<volatile char*>(value);
    while (size--) *secret++ = 0;
  }};
  clear(readback.password, sizeof(readback.password));
  clear(readback.guest_password, sizeof(readback.guest_password));
  clear(readback.bridge_secret, sizeof(readback.bridge_secret));
  if (!written || !verified) memcpy(prefs.password, before, sizeof(before));
  clear(before, sizeof(before));
  return written && verified ? RolePasswordUpdate::SavedApplied : RolePasswordUpdate::Uncertain;
}}
}}
"""
        if role == "companion":
            wrapper += COMPANION_DASHBOARD_BRIDGE + """
namespace onchip {
unsigned companionChannelCount() { return MAX_GROUP_CHANNELS; }
bool companionChannelInfo(unsigned index, char name[32], uint8_t fingerprint[8]) {
  if (!meshInstance || lifecycleBusy(Role::Companion) || index >= MAX_GROUP_CHANNELS) return false;
  ChannelDetails channel;
  if (!meshInstance->getChannel(index, channel)) return false;
  snprintf(name, 32, "%s", channel.name);
  memset(fingerprint, 0, 8);
  bool wide = false;
  for (unsigned i = 16; i < sizeof(channel.channel.secret); ++i) wide = wide || channel.channel.secret[i];
  if (name[0]) mesh::Utils::sha256(fingerprint, 8, channel.channel.secret, wide ? 32 : 16);
  return true;
}
bool companionSetChannel(unsigned index, const char* name, const uint8_t key[16]) {
  if (!meshInstance || lifecycleBusy(Role::Companion) || index >= MAX_GROUP_CHANNELS ||
      !name || strlen(name) > 31) return false;
  ChannelDetails before, channel{};
  if (!meshInstance->getChannel(index, before)) return false;
  strcpy(channel.name, name); memcpy(channel.channel.secret, key, 16);
  roleFS.clearErrors();
  if (!meshInstance->setChannel(index, channel)) return false;
  if (!store.saveChannels(meshInstance) || roleFS.failed()) {
    meshInstance->setChannel(index, before);
    Serial.println("Companion channel persistence failed; saved state may be uncertain");
    return false;
  }
  return true;
}
}
"""
        (dest / f"{role.title()}.cpp").write_text(wrapper)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--check-target", action="store_true")
    parser.add_argument("--field-network", action="store_true",
                        help="refresh dispatch and HTTP sources without regenerating retained role wrappers or clock")
    args = parser.parse_args()
    try:
        check_target(args.upstream, args.target)
    except ValueError as error:
        parser.error(str(error))
    if args.field_network:
        stage_field_network(args.target)
    elif not args.check_target:
        generate(args.upstream, args.target)
