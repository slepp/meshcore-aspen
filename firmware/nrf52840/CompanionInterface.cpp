#include "CompanionInterface.h"
#include "CompanionProtocol.h"
#include "CompanionStore.h"
#include "FirmwareIdentity.h"
#if NRFMAST_PRODUCTION_LUA
#include "PineRuntimePlatform.h"
#endif
#include <cstdio>
#include <cmath>

namespace nrfmast {

static uint32_t read32(const uint8_t* bytes) { uint32_t value; memcpy(&value, bytes, 4); return value; }
static void put32(uint8_t* bytes, uint32_t value) { memcpy(bytes, &value, 4); }

void CompanionInterface::respond(uint8_t code) { transport.writeFrame(&code, 1); }
void CompanionInterface::error(uint8_t code) {
  const uint8_t frame[] = {RESP_CODE_ERR, code};
  transport.writeFrame(frame, sizeof(frame));
}
void CompanionInterface::sent(int result, uint32_t tag, uint32_t timeout) {
  if (result == MSG_SEND_FAILED) { error(ERR_CODE_TABLE_FULL); return; }
  output[0] = RESP_CODE_SENT;
  output[1] = result == MSG_SEND_SENT_FLOOD;
  put32(output + 2, tag);
  put32(output + 6, timeout);
  transport.writeFrame(output, 10);
}

ContactInfo* CompanionInterface::uniqueContact(const uint8_t* prefix, size_t length) {
  auto iter = bot.startContactsIterator();
  ContactInfo contact;
  unsigned count = 0;
  while (iter.hasNext(&bot, contact))
    if (!memcmp(contact.id.pub_key, prefix, length)) ++count;
  return count == 1 ? bot.lookupContactByPubKey(prefix, length) : nullptr;
}

void CompanionInterface::contactFrame(uint8_t code, const ContactInfo& contact) {
  output[0] = code;
  memcpy(output + 1, contact.id.pub_key, PUB_KEY_SIZE);
  output[33] = contact.type;
  output[34] = contact.flags;
  output[35] = contact.out_path_len;
  memcpy(output + 36, contact.out_path, MAX_PATH_SIZE);
  memset(output + 100, 0, 32);
  memcpy(output + 100, contact.name, strnlen(contact.name, 31));
  put32(output + 132, contact.last_advert_timestamp);
  put32(output + 136, contact.gps_lat);
  put32(output + 140, contact.gps_lon);
  put32(output + 144, contact.lastmod);
  static_assert(MAX_PATH_SIZE == 64, "pinned companion contact layout changed");
  transport.writeFrame(output, 148);
}

void CompanionInterface::loop() {
  if (store_) store_->loop(bot.uptimeMillis());
  transport.loop();
  // SerialBLEInterface becomes connected only after the secured callback.
  if (!transport.isConnected()) {
    iterating = false;
    pendingType = 0;
    expectedAck = 0;
    return;
  }
  if (pendingType && uint32_t(bot.uptimeMillis() - pendingAt) > 30000) pendingType = 0;
  if (transport.isWriteBusy()) return;
  const size_t length = transport.checkRecvFrame(input);
  if (length && length <= MAX_FRAME_SIZE) {
    input[length] = 0;
    handle(length);
    memset(input, 0, sizeof(input));
  } else if (iterating) {
    ContactInfo contact;
    if (iterator.hasNext(&bot, contact)) {
      if (contact.lastmod > since) {
        contactFrame(RESP_CODE_CONTACT, contact);
        if (contact.lastmod > lastmod) lastmod = contact.lastmod;
      }
    } else {
      output[0] = RESP_CODE_END_OF_CONTACTS;
      put32(output + 1, lastmod);
      transport.writeFrame(output, 5);
      iterating = false;
    }
  }
}

void CompanionInterface::handle(size_t length) {
  const uint8_t code = input[0];
  auto illegal = [&] { error(ERR_CODE_ILLEGAL_ARG); };
  if (code == CMD_DEVICE_QUERY) {
    if (length != 2) { illegal(); return; }
    version = input[1];
    memset(output, 0, sizeof(output));
    output[0] = RESP_CODE_DEVICE_INFO;
    output[1] = 13;
    output[2] = MAX_CONTACTS / 2;
    output[3] = store_ ? CompanionStore::Channels : 0;
    put32(output + 4, config.ble.getPin());
    memcpy(output + 8, __DATE__, 11);
    memcpy(output + 20, "Seeed Xiao-nrf52", 15);
    memcpy(output + 60, MESHCORE_SLP_PINE_VERSION, sizeof(MESHCORE_SLP_PINE_VERSION) - 1);
    output[80] = 0;  // The companion/bot does not forward; the repeater does.
    output[81] = 2;  // Native hash-mode encoding for three-byte paths.
    transport.writeFrame(output, 82);
  } else if (code == CMD_APP_START) {
    if (length < 8) { illegal(); return; }
    iterating = false;
    CompanionRadioInfo profile = {};
    radioInfo(profile);
    memset(output, 0, sizeof(output));
    output[0] = RESP_CODE_SELF_INFO;
    output[1] = ADV_TYPE_CHAT;
    output[2] = profile.power;
    output[3] = profile.maxPower;
    memcpy(output + 4, bot.self_id.pub_key, PUB_KEY_SIZE);
    put32(output + 48, uint32_t(profile.frequency * 1000));
    put32(output + 52, uint32_t(profile.bandwidth * 1000));
    output[56] = profile.sf;
    output[57] = profile.cr;
    const size_t nameLength = strlen(bot.getName());
    memcpy(output + 58, bot.getName(), nameLength);
    transport.writeFrame(output, 58 + nameLength);
  } else if (code == CMD_GET_CONTACTS) {
    if (length != 1 && length != 5) { illegal(); return; }
    iterator = bot.startContactsIterator();
    since = length == 5 ? read32(input + 1) : 0;
    lastmod = 0;
    iterating = true;
    output[0] = RESP_CODE_CONTACTS_START;
    put32(output + 1, bot.getNumContacts());
    transport.writeFrame(output, 5);
  } else if (code == CMD_GET_CONTACT_BY_KEY || code == CMD_RESET_PATH || code == CMD_REMOVE_CONTACT ||
             code == CMD_SHARE_CONTACT) {
    if (length != 33) { illegal(); return; }
    auto* contact = uniqueContact(input + 1, PUB_KEY_SIZE);
    if (!contact) { error(ERR_CODE_NOT_FOUND); return; }
    if (code == CMD_GET_CONTACT_BY_KEY) contactFrame(RESP_CODE_CONTACT, *contact);
    else if (code == CMD_RESET_PATH) {
      ContactInfo candidate = *contact;
      candidate.out_path_len = OUT_PATH_UNKNOWN;
      candidate.lastmod = bot.getRTCClock()->getCurrentTime();
      if (store_ && !store_->contact(candidate, false, bot.uptimeMillis())) {
        error(ERR_CODE_FILE_IO_ERROR); return;
      }
      bot.resetPathTo(*contact);
      if (store_) contact->lastmod = candidate.lastmod;
      respond(RESP_CODE_OK);
    }
    else if (code == CMD_REMOVE_CONTACT) {
      if (store_ && !store_->contact(*contact, true, bot.uptimeMillis())) {
        error(ERR_CODE_FILE_IO_ERROR); return;
      }
      respond(bot.removeContact(*contact) ? RESP_CODE_OK : RESP_CODE_DISABLED);
    } else respond(bot.shareContactZeroHop(*contact) ? RESP_CODE_OK : RESP_CODE_DISABLED);
  } else if (code == CMD_ADD_UPDATE_CONTACT) {
    if (length != 136 && length != 144 && length != 148) { illegal(); return; }
    if (input[35] != OUT_PATH_UNKNOWN && !mesh::Packet::isValidPathLen(input[35])) { illegal(); return; }
    ContactInfo candidate = {};
    memcpy(candidate.id.pub_key, input + 1, PUB_KEY_SIZE);
    if (candidate.id.matches(bot.self_id)) { illegal(); return; }
    candidate.type = input[33];
    candidate.flags = input[34] & 1;  // Favourite only; this does not grant any permission.
    candidate.out_path_len = input[35];
    memcpy(candidate.out_path, input + 36, MAX_PATH_SIZE);
    memcpy(candidate.name, input + 100, 31);
    candidate.last_advert_timestamp = length >= 136 ? read32(input + 132) : 0;
    candidate.gps_lat = length >= 144 ? read32(input + 136) : 0;
    candidate.gps_lon = length >= 144 ? read32(input + 140) : 0;
    candidate.lastmod = bot.getRTCClock()->getCurrentTime();
    auto* existing = bot.lookupContactByPubKey(candidate.id.pub_key, PUB_KEY_SIZE);
    if (!existing && bot.getNumContacts() >= int(CompanionStore::Contacts)) {
      error(ERR_CODE_TABLE_FULL); return;
    }
    if (store_ && !store_->contact(candidate, false, bot.uptimeMillis())) {
      error(ERR_CODE_FILE_IO_ERROR); return;
    }
    if (existing) { *existing = candidate; respond(RESP_CODE_OK); }
    else if (bot.addContact(candidate)) respond(RESP_CODE_OK);
    else error(ERR_CODE_TABLE_FULL);
  } else if (code == CMD_SEND_TXT_MSG) {
    if (length < 14 || length - 13 > MAX_TEXT_LEN ||
        (input[1] != TXT_TYPE_PLAIN && input[1] != TXT_TYPE_CLI_DATA) ||
        memchr(input + 13, 0, length - 13)) { illegal(); return; }
    auto* contact = uniqueContact(input + 7, 6);
    if (!contact) { error(ERR_CODE_NOT_FOUND); return; }
    if (bot.txBusy()) { error(ERR_CODE_BAD_STATE); return; }
    uint32_t ack = 0, timeout = 0;
    const int result = bot.companionSend(*contact, input[1], input[2], read32(input + 3),
                                         reinterpret_cast<char*>(input + 13), ack, timeout);
    if (result != MSG_SEND_FAILED && ack) expectedAck = ack;
    sent(result, ack, timeout);
  } else if (code == CMD_SYNC_NEXT_MESSAGE) {
    if (length != 1) { illegal(); return; }
    if (!inboxCount) respond(RESP_CODE_NO_MORE_MESSAGES);
    else {
      transport.writeFrame(inbox[0].bytes, inbox[0].length);
      --inboxCount;
      for (size_t i = 0; i < inboxCount; ++i) inbox[i] = inbox[i + 1];
      inbox[inboxCount] = {};
    }
  } else if (code == CMD_EXPORT_CONTACT) {
    if (length != 1 && length != 33) { illegal(); return; }
    if (length == 33) {
      auto *contact = uniqueContact(input + 1, PUB_KEY_SIZE);
      if (!contact) { error(ERR_CODE_NOT_FOUND); return; }
      const uint8_t size = bot.exportContact(*contact, output + 1);
      if (!size) { respond(RESP_CODE_DISABLED); return; }
      output[0] = RESP_CODE_EXPORT_CONTACT;
      transport.writeFrame(output, size + 1);
      return;
    }
    auto* packet = bot.createSelfAdvert(bot.getName());
    if (!packet) { error(ERR_CODE_TABLE_FULL); return; }
    packet->header |= ROUTE_TYPE_FLOOD;
    output[0] = RESP_CODE_EXPORT_CONTACT;
    const size_t bytes = packet->writeTo(output + 1);
    bot.releasePacket(packet);
    transport.writeFrame(output, bytes + 1);
  } else if (code == CMD_IMPORT_CONTACT) {
    if (length < 100) { illegal(); return; }
    if (bot.contactImportPending()) { error(ERR_CODE_BAD_STATE); return; }
    if (bot.importContact(input + 1, length - 1)) respond(RESP_CODE_OK);
    else illegal();
  } else if (code == CMD_GET_DEVICE_TIME) {
    if (length != 1) { illegal(); return; }
    output[0] = RESP_CODE_CURR_TIME;
    put32(output + 1, bot.getRTCClock()->getCurrentTime());
    transport.writeFrame(output, 5);
  } else if (code == CMD_SET_DEVICE_TIME) {
    if (length != 5 || read32(input + 1) < 1715770351u ||
        read32(input + 1) > 4102444800u) { illegal(); return; }
#if NRFMAST_PRODUCTION_LUA
    // This frame is admitted only on the bonded PIN/MITM companion connection.
    if (!trustLuaTime(read32(input + 1), LuaTimeSource::BleCompanion)) {
      illegal(); return;
    }
#else
    if (read32(input + 1) < bot.getRTCClock()->getCurrentTime()) { illegal(); return; }
    bot.getRTCClock()->setCurrentTime(read32(input + 1));
#endif
    respond(RESP_CODE_OK);
  } else if (code == CMD_SEND_SELF_ADVERT) {
    if (length > 2 || (length == 2 && input[1] > 1)) { illegal(); return; }
    respond(bot.advertise(length == 1 || input[1] == 0) ? RESP_CODE_OK : RESP_CODE_DISABLED);
  } else if (code == CMD_SET_ADVERT_NAME) {
    if (length < 2 || length > 32 || memchr(input + 1, 0, length - 1) ||
        !CommandBot::validName(reinterpret_cast<char*>(input + 1))) { illegal(); return; }
    char command[48], reply[160] = {};
    snprintf(command, sizeof(command), "set bot.name %s", input + 1);
    config.handleCommand(1, command, reply);
    if (!strncmp(reply, "OK ", 3)) respond(RESP_CODE_OK);
    else error(ERR_CODE_FILE_IO_ERROR);
  } else if (code == CMD_SET_DEVICE_PIN) {
    if (length != 5 || read32(input + 1) < 100000 || read32(input + 1) > 999999) { illegal(); return; }
    if (config.ble.setPin(read32(input + 1))) respond(RESP_CODE_OK);
    else error(ERR_CODE_FILE_IO_ERROR);
  } else if (code == CMD_GET_BATT_AND_STORAGE) {
    if (length != 1) { illegal(); return; }
    output[0] = RESP_CODE_BATT_AND_STORAGE;
    const uint16_t millivolts = battery ? battery() : 0;
    memcpy(output + 1, &millivolts, 2);
    uint32_t used = 0, total = 0;
    if (storage) storage(used, total);
    put32(output + 3, used);
    put32(output + 7, total);
    transport.writeFrame(output, 11);
  } else if (code == CMD_GET_CHANNEL) {
    if (length != 2) { illegal(); return; }
    ChannelDetails channel{};
    if (!store_ || !store_->ready() || input[1] >= CompanionStore::Channels ||
        !bot.getChannel(input[1], channel)) { error(ERR_CODE_NOT_FOUND); return; }
    output[0] = RESP_CODE_CHANNEL_INFO; output[1] = input[1];
    memcpy(output + 2, channel.name, 32); memcpy(output + 34, channel.channel.secret, 16);
    transport.writeFrame(output, 50);
  } else if (code == CMD_SET_CHANNEL) {
    if (!store_) { respond(RESP_CODE_DISABLED); return; }
    if (length == 66) { error(ERR_CODE_UNSUPPORTED_CMD); return; }
    if (length != 50 || !memchr(input + 2, 0, 32)) { illegal(); return; }
    if (input[1] >= CompanionStore::Channels) { error(ERR_CODE_NOT_FOUND); return; }
    ChannelDetails channel{};
    memcpy(channel.name, input + 2, strnlen(reinterpret_cast<char *>(input + 2), 31));
    memcpy(channel.channel.secret, input + 34, 16);
    if (!store_->channel(input[1], channel, bot.uptimeMillis())) {
      error(ERR_CODE_FILE_IO_ERROR); return;
    }
    respond(bot.setChannel(input[1], channel) ? RESP_CODE_OK : RESP_CODE_DISABLED);
  } else if (code == CMD_SEND_CHANNEL_TXT_MSG) {
    if (!store_) { respond(RESP_CODE_DISABLED); return; }
    if (length < 8 || length - 7 > MAX_TEXT_LEN || memchr(input + 7, 0, length - 7)) {
      illegal(); return;
    }
    if (input[1] != TXT_TYPE_PLAIN) { error(ERR_CODE_UNSUPPORTED_CMD); return; }
    ChannelDetails channel{};
    if (!store_->ready() || input[2] >= CompanionStore::Channels ||
        !bot.getChannel(input[2], channel) || !channel.name[0]) { error(ERR_CODE_NOT_FOUND); return; }
    if (!bot.companionChannelSend(read32(input + 3), channel,
                                 reinterpret_cast<char *>(input + 7), length - 7)) {
      error(ERR_CODE_TABLE_FULL); return;
    }
    respond(RESP_CODE_OK);
  } else if (code == CMD_GET_AUTOADD_CONFIG) {
    if (length != 1) { illegal(); return; }
    const uint8_t frame[] = {RESP_CODE_AUTOADD_CONFIG, 15, 0};  // replace oldest, chat/repeater/room
    transport.writeFrame(frame, sizeof(frame));
  } else if (code == CMD_GET_CUSTOM_VARS) {
    if (length != 1) { illegal(); return; }
    output[0] = RESP_CODE_CUSTOM_VARS;
    const int size = snprintf(reinterpret_cast<char*>(output + 1), sizeof(output) - 1,
                              "radio_authority:repeater,notes:DM-only,BLE_PIN_apply:reboot,contacts:%s,companion_store:%s",
                              store_ ? "QSPI" : "volatile", store_ ? store_->error() : "unavailable");
    transport.writeFrame(output, size + 1);
  } else if (code == CMD_GET_TUNING_PARAMS) {
    if (length != 1) { illegal(); return; }
    CompanionRadioInfo profile = {};
    radioInfo(profile);
    output[0] = RESP_CODE_TUNING_PARAMS;
    put32(output + 1, 0);  // The bot's RX delay is zero, independently of repeater forwarding.
    put32(output + 5, uint32_t(profile.airtimeFactor * 1000));
    transport.writeFrame(output, 9);
  } else if (code == CMD_GET_STATS) {
    if (length != 2 || input[1] > 2) { illegal(); return; }
    output[0] = RESP_CODE_STATS;
    output[1] = input[1];
    if (input[1] == 0) {
      const uint16_t millivolts = battery ? battery() : 0, flags = bot.errorFlags();
      memcpy(output + 2, &millivolts, 2);
      put32(output + 4, bot.uptimeMillis() / 1000);
      memcpy(output + 8, &flags, 2);
      output[10] = bot.queueLength();
      transport.writeFrame(output, 11);
    } else {
      CompanionRadioInfo profile = {};
      radioInfo(profile);
      if (input[1] == 1) {
        memcpy(output + 2, &profile.noiseFloor, 2);
        output[4] = std::isfinite(profile.lastRssi) && profile.lastRssi >= -128 && profile.lastRssi <= 127 ?
                    int8_t(profile.lastRssi) : 0;
        output[5] = std::isfinite(profile.lastSnr) && profile.lastSnr >= -32 && profile.lastSnr <= 31.75 ?
                    int8_t(profile.lastSnr * 4) : 0;
        put32(output + 6, bot.getTotalAirTime() / 1000);
        put32(output + 10, bot.getReceiveAirTime() / 1000);
        transport.writeFrame(output, 14);
      } else {
        put32(output + 2, profile.physicalReceived);
        put32(output + 6, profile.physicalSent);
        put32(output + 10, bot.getNumSentFlood());
        put32(output + 14, bot.getNumSentDirect());
        put32(output + 18, bot.getNumRecvFlood());
        put32(output + 22, bot.getNumRecvDirect());
        put32(output + 26, profile.physicalErrors);
        transport.writeFrame(output, 30);
      }
    }
  } else if (code == CMD_SEND_LOGIN || code == CMD_SEND_STATUS_REQ) {
    if (length < 33 || (code == CMD_SEND_STATUS_REQ && length != 33) ||
        (code == CMD_SEND_LOGIN && (length > 49 || memchr(input + 33, 0, length - 33)))) {
      illegal(); return;
    }
    auto* contact = uniqueContact(input + 1, PUB_KEY_SIZE);
    if (!contact) { error(ERR_CODE_NOT_FOUND); return; }
    if (pendingType) { error(ERR_CODE_BAD_STATE); return; }
    uint32_t tag = 0, timeout = 0;
    const int result = code == CMD_SEND_LOGIN ?
        bot.sendLogin(*contact, reinterpret_cast<char*>(input + 33), timeout) :
        bot.sendRequest(*contact, REQ_TYPE_GET_STATUS, tag, timeout);
    if (result != MSG_SEND_FAILED) {
      pendingType = code;
      memcpy(pendingKey, contact->id.pub_key, PUB_KEY_SIZE);
      pendingAt = bot.uptimeMillis();
      if (code == CMD_SEND_LOGIN) memcpy(&tag, pendingKey, 4);
    }
    sent(result, tag, timeout);
  } else if (code == CMD_SET_RADIO_PARAMS || code == CMD_SET_RADIO_TX_POWER ||
             code == CMD_SET_TUNING_PARAMS || code == CMD_SET_PATH_HASH_MODE ||
             code == CMD_SET_OTHER_PARAMS || code == CMD_EXPORT_PRIVATE_KEY ||
             code == CMD_IMPORT_PRIVATE_KEY || code == CMD_FACTORY_RESET ||
             code == CMD_SEND_CHANNEL_DATA || code == CMD_REBOOT || code == CMD_SIGN_START ||
             code == CMD_SIGN_DATA || code == CMD_SIGN_FINISH || code == CMD_SET_AUTOADD_CONFIG) {
    respond(RESP_CODE_DISABLED);
  } else error(ERR_CODE_UNSUPPORTED_CMD);
}

void CompanionInterface::discovered(const ContactInfo& contact, bool isNew) {
  if (store_) store_->changed(bot.uptimeMillis());
  if (!transport.isConnected() || transport.isWriteBusy()) return;
  if (isNew) contactFrame(PUSH_CODE_NEW_ADVERT, contact);
  else {
    output[0] = PUSH_CODE_ADVERT;
    memcpy(output + 1, contact.id.pub_key, PUB_KEY_SIZE);
    transport.writeFrame(output, 33);
  }
}

void CompanionInterface::pathUpdated(const ContactInfo& contact) {
  if (store_) store_->changed(bot.uptimeMillis());
  if (!transport.isConnected() || transport.isWriteBusy()) return;
  output[0] = PUSH_CODE_PATH_UPDATED;
  memcpy(output + 1, contact.id.pub_key, PUB_KEY_SIZE);
  transport.writeFrame(output, 33);
}

void CompanionInterface::received(const ContactInfo& contact, const mesh::Packet& packet,
                                  uint32_t timestamp, uint8_t type, const char* text) {
  if (store_) store_->changed(bot.uptimeMillis());
  if (inboxCount == 4) {
    ++inboxDrops;
    Serial.println("BLE: ordinary DM inbox full; message not retained");
    return;
  }
  auto& frame = inbox[inboxCount++];
  size_t i = 0;
  if (version >= 3) {
    frame.bytes[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
    frame.bytes[i++] = packet._snr;
    frame.bytes[i++] = 0;
    frame.bytes[i++] = 0;
  } else frame.bytes[i++] = RESP_CODE_CONTACT_MSG_RECV;
  memcpy(frame.bytes + i, contact.id.pub_key, 6); i += 6;
  frame.bytes[i++] = packet.isRouteFlood() ? packet.path_len : 0xFF;
  frame.bytes[i++] = type;
  put32(frame.bytes + i, timestamp); i += 4;
  const size_t textLength = strnlen(text, MAX_FRAME_SIZE - i);
  memcpy(frame.bytes + i, text, textLength);
  frame.length = i + textLength;
  if (transport.isConnected()) respond(PUSH_CODE_MSG_WAITING);
}

void CompanionInterface::channelReceived(int slot, const mesh::Packet &packet,
                                         uint32_t timestamp, const char *text) {
  if (!store_ || !store_->ready() || slot < 0 || slot >= int(CompanionStore::Channels)) return;
  ChannelDetails channel{};
  if (!bot.getChannel(slot, channel) || !channel.name[0]) return;
  if (inboxCount == 4) { ++inboxDrops; return; }
  auto &frame = inbox[inboxCount++];
  unsigned i = 0;
  if (version >= 3) {
    frame.bytes[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
    frame.bytes[i++] = packet._snr;
    frame.bytes[i++] = 0; frame.bytes[i++] = 0;
  } else frame.bytes[i++] = RESP_CODE_CHANNEL_MSG_RECV;
  frame.bytes[i++] = slot;
  frame.bytes[i++] = packet.isRouteFlood() ? packet.path_len : 0xFF;
  frame.bytes[i++] = TXT_TYPE_PLAIN;
  put32(frame.bytes + i, timestamp); i += 4;
  const size_t size = strnlen(text, MAX_FRAME_SIZE - i);
  memcpy(frame.bytes + i, text, size); frame.length = i + size;
  if (transport.isConnected()) respond(PUSH_CODE_MSG_WAITING);
}

void CompanionInterface::confirmed(uint32_t ack, uint32_t elapsed) {
  if (!expectedAck || expectedAck != ack) return;
  expectedAck = 0;
  if (!transport.isConnected()) return;
  output[0] = PUSH_CODE_SEND_CONFIRMED;
  put32(output + 1, ack);
  put32(output + 5, elapsed);
  transport.writeFrame(output, 9);
}

void CompanionInterface::response(const ContactInfo& contact, const uint8_t* data, size_t length) {
  if (!pendingType || memcmp(pendingKey, contact.id.pub_key, PUB_KEY_SIZE) || length < 5 ||
      !transport.isConnected()) return;
  const uint8_t type = pendingType;
  pendingType = 0;
  if (type == CMD_SEND_LOGIN) {
    size_t size = 8;
    output[0] = PUSH_CODE_LOGIN_FAIL;
    output[1] = 0;
    memcpy(output + 2, contact.id.pub_key, 6);
    if (length >= 6 && !memcmp(data + 4, "OK", 2)) output[0] = PUSH_CODE_LOGIN_SUCCESS;
    else if (length >= 13 && data[4] == RESP_SERVER_LOGIN_OK) {
      output[0] = PUSH_CODE_LOGIN_SUCCESS;
      output[1] = data[6];
      memcpy(output + 8, data, 4);
      output[12] = data[7];
      output[13] = data[12];
      size = 14;
    }
    transport.writeFrame(output, size);
  } else {
    if (length - 4 > MAX_FRAME_SIZE - 8) return;
    output[0] = PUSH_CODE_STATUS_RESPONSE;
    output[1] = 0;
    memcpy(output + 2, contact.id.pub_key, 6);
    memcpy(output + 8, data + 4, length - 4);
    transport.writeFrame(output, 8 + length - 4);
  }
}

}  // namespace nrfmast
