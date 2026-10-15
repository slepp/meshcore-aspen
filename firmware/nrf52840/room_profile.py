"""Stage a bounded upstream Room without sharing the repeater's storage paths."""

import pathlib
import re

NAMES = {
    "MyMesh": "PineRoomMesh",
    "CommonCLI": "PineRoomCLI",
    "CommonCLICallbacks": "PineRoomCLICallbacks",
    "NodePrefs": "PineRoomPrefs",
    "ClientACL": "PineRoomACL",
    "ClientInfo": "PineRoomClientInfo",
    "RegionMap": "PineRoomRegionMap",
    "RegionEntry": "PineRoomRegionEntry",
}


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Pinned Pine Room hook changed: {old}")
    return text.replace(old, new)


def body(text, signature, replacement):
    start = text.index("{", text.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[:start + 1] + "\n" + replacement + "\n" + text[end - 1:]


def stage_room(output):
    output = pathlib.Path(output)
    dest = output / "examples/nrfmast/room"
    dest.mkdir(parents=True, exist_ok=True)
    inputs = [(output / "examples/simple_room_server" / ("MyMesh" + ext))
              for ext in (".h", ".cpp")]
    inputs += [(output / "src/helpers" / (name + ext))
               for name in ("CommonCLI", "ClientACL", "RegionMap") for ext in (".h", ".cpp")]
    for path in inputs:
        text = path.read_text()
        for name, replacement in NAMES.items():
            text = re.sub(r"\b" + name + r"\b", replacement, text)
        text = re.sub(r"<helpers/(PineRoom\w+\.h)>", r'"\1"', text)
        text = text.replace('#include "TransportKeyStore.h"', '#include <helpers/TransportKeyStore.h>')
        text = re.sub(r"\bMAX_CLIENTS\b", "PINE_ROOM_CLIENTS", text)
        text = re.sub(r"\bMAX_REGION_ENTRIES\b", "PINE_ROOM_REGIONS", text)
        text = re.sub(r"\bMAX_UNSYNCED_POSTS\b", "PINE_ROOM_POSTS", text)
        for filename in ("prefs.json", "com_prefs", "s_contacts", "regions2", "packet_log"):
            text = text.replace(f'"/{filename}"', f'"/room-{filename}"')
        if path.name == "MyMesh.h":
            text = '#include "../FirmwareIdentity.h"\n' + text
            text = text.replace("return FIRMWARE_VERSION;", "return MESHCORE_SLP_PINE_VERSION;")
            text = text.replace("return FIRMWARE_BUILD_DATE;", "return __DATE__;")
            text = replace_once(text, "mesh::RTCClock& rtc, mesh::MeshTables& tables);",
                                "mesh::RTCClock& rtc, mesh::MeshTables& tables, mesh::PacketManager& packets);")
            text = text.replace("#define FIRMWARE_ROLE", "#undef FIRMWARE_ROLE\n#define FIRMWARE_ROLE")
            text = replace_once(text, "  void savePrefs() override {\n    _cli.savePrefs(_fs);\n  }",
                                "  bool saveChecked() { return _cli.savePrefs(_fs); }\n"
                                "  bool saveFailed = false;\n"
                                "  void savePrefs() override { saveFailed = !saveChecked(); }")
            text = text.replace("  void loop();", "  void loop();\n"
                                "  unsigned queuedCount() const { return _mgr->getOutboundTotal(); }\n")
        elif path.name == "MyMesh.cpp":
            text = re.sub(r"^  board\.attachDynamicPrefs\([^\n]*\);\n", "", text, flags=re.M)
            text = '#include "../FirmwareIdentity.h"\n#include "../PineRoom.h"\n' + text
            text = text.replace("FIRMWARE_VERSION", "MESHCORE_SLP_PINE_VERSION")
            text = replace_once(text, "mesh::RTCClock &rtc, mesh::MeshTables &tables)",
                                "mesh::RTCClock &rtc, mesh::MeshTables &tables, mesh::PacketManager &packets)")
            text = replace_once(text, "*new StaticPoolPacketManager(32)", "packets")
            text = text.replace("_prefs.advert_interval = 1;", "_prefs.advert_interval = 0;")
            text = text.replace("_prefs.flood_advert_interval = 47;", "_prefs.flood_advert_interval = 0;")
            text = text.replace("ADVERT_NAME, sizeof(_prefs.node_name)", '"Pine Room", sizeof(_prefs.node_name)')
            # Room settings cannot change the physical radio, board or shared sensors.
            text = re.sub(r"^ +(?:radio_driver\.(?:setParams|setTxPower|setRxBoostedGainMode|resetStats)|"
                          r"board\.(?:setLoRaFemLnaEnabled|setLoRaFemPaGainEnabled|setAdcMultiplier))"
                          r"\([^\n]*\);\n", "", text, flags=re.M)
            text = text.replace("  applyGpsPrefs();", "")
            text = body(text, "void PineRoomMesh::applyTempRadioParams", "  return;")
            text = body(text, "bool PineRoomMesh::formatFileSystem", "  return false;")
            text = body(text, "void PineRoomMesh::setTxPower", "  return;")
            text = body(text, "bool PineRoomMesh::setRxBoostedGain", "  return false;")
            text = body(text, "void PineRoomMesh::saveIdentity", "  return;")
            text = replace_once(text, "  // handle ACL related commands",
                                "  if (nrfmast::roomUnsafeCommand(command)) {\n"
                                '    strcpy(reply, "Error: Room command not allowed; device settings use repeater; passwords require 1..15 bytes without edge spaces");\n'
                                "    return;\n  }\n\n  // handle ACL related commands")
            text = replace_once(text, "      client = acl.putClient(sender, 0);  // add to known clients (if not already known)",
                                "      client = acl.putClient(sender, 0);\n"
                                "      if (!client) return;")
            text = replace_once(text, "    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands",
                                "    saveFailed = false;\n"
                                "    _cli.handleCommand(sender_timestamp, command, reply);\n"
                                "    if (saveFailed) {\n"
                                "      _cli.loadPrefs(_fs);\n"
                                '      strcpy(reply, "Error: Room preferences save failed; saved settings reloaded");\n'
                                "    }")
            # A telemetry request has four reserved bytes after the request type.
            text = text.replace("payload[0] == REQ_TYPE_GET_TELEMETRY_DATA)",
                                "payload_len >= 5 && payload[0] == REQ_TYPE_GET_TELEMETRY_DATA)")
            text = text.replace("payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()",
                                "payload_len >= 3 && payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()")
            text = replace_once(text, "    acl.save(_fs, PineRoomMesh::saveFilter);\n    dirty_contacts_expiry = 0;",
                                "    dirty_contacts_expiry = acl.save(_fs, PineRoomMesh::saveFilter) ? 0 : futureMillis(5000);")
        elif path.name == "CommonCLI.cpp":
            text = text.replace('#include "TxtDataHelpers.h"', '#include <helpers/TxtDataHelpers.h>')
            text = text.replace('#include "AdvertDataHelpers.h"', '#include <helpers/AdvertDataHelpers.h>')
            text = text.replace("OK - Advert sent", "OK - advert queued")
            text = text.replace("OK - zerohop advert sent", "OK - zerohop advert queued")
            text = replace_once(text, '      sprintf(reply, "password now: ");\n'
                                '      StrHelper::strncpy(&reply[14], _prefs->password, 160-15);   // echo back just to let admin know for sure!!',
                                '      strcpy(reply, "OK: Room administrator password saved");')
            text = body(text, "bool PineRoomCLI::savePrefs", """  const char* pending = "/room-prefs.pending";
  fs->remove(pending);
  File file = fs->open(pending, FILE_O_WRITE);
  if (!file) return false;
  const bool written = _prefs->saveSerial(file);
  file.close();
  if (!written) { fs->remove(pending); return false; }
  PineRoomPrefs check;
  file = fs->open(pending);
  const bool valid = file && check.loadSerial(file);
  file.close();
  if (!valid) { fs->remove(pending); return false; }
  return fs->rename(pending, "/room-prefs.json");""")
        elif path.name == "ClientACL.h":
            text = replace_once(text, "  void save(FILESYSTEM* _fs, bool (*filter)(PineRoomClientInfo*)=NULL);",
                                "  bool save(FILESYSTEM* _fs, bool (*filter)(PineRoomClientInfo*)=NULL);")
        elif path.name == "ClientACL.cpp":
            text = '#include <SHA256.h>\n' + text
            text = text.replace("void PineRoomACL::save(", "bool PineRoomACL::save(")
            text = body(text, "bool PineRoomACL::save", """  _fs = fs;
  const char* pending = "/room-contacts.pending";
  File file = openWrite(fs, pending);
  if (!file) return false;
  SHA256 written;
  written.reset();
  uint32_t length = 0;
  auto append = [&](const uint8_t* bytes, size_t size) {
    if (file.write(bytes, size) != size) return false;
    written.update(bytes, size); length += size; return true;
  };
  const uint8_t unused[2]{};
  bool success = true;
  for (int i = 0; success && i < num_clients; ++i) {
    const auto* c = &clients[i];
    if (!c->permissions || (filter && !filter(&clients[i]))) continue;
    success = append(c->id.pub_key, 32) &&
        append(&c->permissions, 1) &&
        append(reinterpret_cast<const uint8_t*>(&c->extra.room.sync_since), 4) &&
        append(unused, 2) && append(&c->out_path_len, 1) &&
        append(c->out_path, 64) && append(c->shared_secret, PUB_KEY_SIZE);
  }
  file.close();
  uint8_t expected[32], actual[32], buffer[64];
  written.finalize(expected, sizeof(expected));
  SHA256 readback;
  readback.reset();
  file = fs->open(pending);
  success = success && file && file.size() == length;
  while (success && length) {
    const size_t count = length < sizeof(buffer) ? length : sizeof(buffer);
    success = file.read(buffer, count) == count;
    if (success) { readback.update(buffer, count); length -= count; }
  }
  file.close();
  readback.finalize(actual, sizeof(actual));
  success = success && !memcmp(expected, actual, sizeof(expected));
  if (!success) { fs->remove(pending); return false; }
  return fs->rename(pending, "/room-s_contacts");""")
            text = replace_once(text, "  PineRoomClientInfo* oldest = &clients[PINE_ROOM_CLIENTS - 1];",
                                "  PineRoomClientInfo* oldest = nullptr;")
            text = replace_once(text, "    c = oldest;  // evict least active contact",
                                "    if (!oldest) return nullptr;\n    c = oldest;")
            text = replace_once(text, "bool PineRoomACL::applyPermissions(const mesh::LocalIdentity& self_id, const uint8_t* pubkey, int key_len, uint8_t perms) {",
                                "bool PineRoomACL::applyPermissions(const mesh::LocalIdentity& self_id, const uint8_t* pubkey, int key_len, uint8_t perms) {\n"
                                "  if (key_len < 1 || key_len > PUB_KEY_SIZE) return false;")
            text = replace_once(text, "    c = putClient(id, 0);",
                                "    c = putClient(id, 0);\n    if (!c) return false;")
        elif path.name == "RegionMap.cpp":
            start = text.index("bool PineRoomRegionMap::save(")
            end = text.index("\nPineRoomRegionEntry* PineRoomRegionMap::putRegion", start)
            section = text[start:end]
            section = replace_once(section, '  File file = openWrite(_fs, path ? path : "/room-regions2");',
                                   '  const char* pending = "/room-regions.pending";\n'
                                   "  File file = openWrite(_fs, pending);")
            section = replace_once(section, "    return success;", """    PineRoomRegionMap check(*_store);
    success = success && check.load(_fs, pending) &&
        check.num_regions == num_regions && check.default_id == default_id &&
        check.home_id == home_id && check.wildcard.flags == wildcard.flags &&
        check.next_id == next_id;
    for (unsigned i = 0; success && i < num_regions; ++i) {
      const auto& a = regions[i];
      const auto& b = check.regions[i];
      success = a.id == b.id && a.parent == b.parent && a.flags == b.flags &&
          !memcmp(a.name, b.name, sizeof(a.name));
    }
    if (!success) { _fs->remove(pending); return false; }
    return _fs->rename(pending, path ? path : "/room-regions2");""")
            text = text[:start] + section + text[end:]
        (dest / (NAMES.get(path.stem, path.stem) + path.suffix)).write_text(
            "#if NRFMAST_PRODUCTION_LUA\n" + text + "\n#endif\n")
