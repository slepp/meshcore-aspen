// SPDX-License-Identifier: Apache-2.0
#define main pineNativeTests
#include "native.cpp"
#undef main
#include "CompanionStore.h"
#include "PineFilesystem.h"
#include "platform/nvs.h"
extern "C" void *pvPortMalloc(size_t size) { return std::malloc(size); }
extern "C" void vPortFree(void *pointer) { std::free(pointer); }
static bool failCompanionAllocation;
void *operator new(size_t size, const std::nothrow_t &) noexcept {
  if (failCompanionAllocation && size == sizeof(nrfmast::CompanionStore::Record)) return nullptr;
  try { return ::operator new(size); } catch (const std::bad_alloc &) { return nullptr; }
}
void operator delete(void *pointer, const std::nothrow_t &) noexcept { ::operator delete(pointer); }

namespace {
struct CompanionFlash : nrfmast::NoteFlash {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x200000, 0xff);
  unsigned writes = 0;
  int cut = -1;
  bool powered = true;
  bool begin() override { return powered; }
  bool partitionFree(uint32_t, uint32_t) override { return false; }
  bool retiredPartition(uint32_t, uint32_t) override { return false; }
  bool read(uint32_t at, void *out, size_t size) override {
    assert(at + size <= nrfmast::PineFilesystem::Bytes);
    if (!powered) return false;
    memcpy(out, bytes.data() + at, size); return true;
  }
  bool program(uint32_t at, const void *in, size_t size) override {
    assert(at + size <= nrfmast::PineFilesystem::Bytes);
    if (!powered) return false;
    const bool fail = int(++writes) == cut;
    const auto *input = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < (fail ? size / 2 : size); ++i) {
      assert((bytes[at + i] & input[i]) == input[i]); bytes[at + i] &= input[i];
    }
    if (fail) powered = false;
    return !fail;
  }
  bool erase(uint32_t at) override {
    assert(at < nrfmast::PineFilesystem::Bytes);
    if (!powered) return false;
    memset(bytes.data() + at, 0xff, 4096); return true;
  }
};
struct CompanionHarness {
  Clock clock;
  RTC rtc;
  Random rng;
  Physical radio;
  SimpleMeshTables tables;
  StaticPoolPacketManager pool{8};
  NativeFilesystem internal;
  mesh::LocalIdentity relay{&rng};
  nrfmast::CommandBot bot{radio, clock, rng, rtc, pool, tables};
  nrfmast::RuntimeConfig config{internal, relay, bot};
  Frames frames;
  nrfmast::CompanionInterface companion{bot, config, frames,
    [](nrfmast::CompanionRadioInfo &info) { info = {}; }};
  nrfmast::CompanionStore store{bot};
  CompanionHarness() {
    bot.self_id = mesh::LocalIdentity(&rng);
    bot.begin();
    bot.setCompanion(companion);
    companion.setStore(store);
  }
  std::vector<uint8_t> command(std::vector<uint8_t> bytes) {
    frames.incoming.push_back(bytes);
    auto before = frames.outgoing.size();
    companion.loop();
    assert(frames.outgoing.size() == before + 1);
    return frames.outgoing.back();
  }
};
std::vector<uint8_t> channelFrame(unsigned slot, const char *name, uint8_t key) {
  std::vector<uint8_t> bytes(50);
  bytes[0] = CMD_SET_CHANNEL; bytes[1] = slot;
  memcpy(bytes.data() + 2, name, strlen(name)); bytes[34] = key;
  return bytes;
}
std::vector<uint8_t> contactFrame(const mesh::LocalIdentity &id, unsigned i) {
  std::vector<uint8_t> bytes(148);
  bytes[0] = CMD_ADD_UPDATE_CONTACT;
  memcpy(bytes.data() + 1, id.pub_key, 32);
  bytes[33] = ADV_TYPE_CHAT; bytes[34] = 1; bytes[35] = 1; bytes[36] = i;
  snprintf(reinterpret_cast<char *>(bytes.data() + 100), 32, "contact-%u", i);
  uint32_t epoch = 1900000000; memcpy(bytes.data() + 132, &epoch, 4);
  return bytes;
}
void remount(CompanionFlash &flash) {
  flash.powered = true; flash.cut = -1;
  assert(nrfmast::botFilesystem.end() && nrfmast::botFilesystem.begin(flash, false));
}
void persistenceAndProtocol() {
  CompanionFlash flash;
  assert(nrfmast::botFilesystem.begin(flash, true));
  std::vector<mesh::LocalIdentity> contacts;
  {
    CompanionHarness h;
    assert(h.store.begin(h.clock.now));
    assert(h.command({CMD_DEVICE_QUERY, 3})[3] == 4);
    const auto initialWrites = flash.writes;
    h.frames.secured = false;
    h.frames.incoming.push_back(channelFrame(0, "private", 7));
    h.companion.loop();
    ChannelDetails channel{};
    assert(h.bot.getChannel(0, channel) && !channel.name[0] && flash.writes == initialWrites);
    h.frames.secured = true;
    h.companion.loop();
    assert(h.frames.outgoing.back() == std::vector<uint8_t>{RESP_CODE_OK});
    auto response = h.command({CMD_GET_CHANNEL, 0});
    assert(response.size() == 50 && response[0] == RESP_CODE_CHANNEL_INFO &&
           response[34] == 7 && !strcmp(reinterpret_cast<char *>(response.data() + 2), "private"));
    assert(h.command({CMD_GET_CHANNEL, 4}) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_NOT_FOUND}));
    auto malformed = channelFrame(0, "bad", 1); malformed.pop_back();
    assert(h.command(malformed) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_ILLEGAL_ARG}));
    malformed.resize(66);
    assert(h.command(malformed) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_UNSUPPORTED_CMD}));
    for (unsigned i = 0; i < 8; ++i) {
      contacts.emplace_back(&h.rng);
      h.clock.now += nrfmast::CompanionStore::RefillMs;
      assert(h.command(contactFrame(contacts.back(), i)) == std::vector<uint8_t>{RESP_CODE_OK});
    }
    mesh::LocalIdentity extra(&h.rng);
    assert(h.command(contactFrame(extra, 9)) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_TABLE_FULL}));
    // Persistent epoch metadata is retained, but shared-secret/session/ACK state is not serialized.
    auto *first = h.bot.lookupContactByPubKey(contacts[0].pub_key, 32);
    assert(first); first->sync_since = 1850000000;
    first->getSharedSecret(h.bot.self_id);
    h.store.changed(h.clock.now);
    auto writes = flash.writes;
    h.clock.now += nrfmast::CompanionStore::FlushMs - 1;
    h.companion.loop(); assert(flash.writes == writes);
    ++h.clock.now; h.companion.loop(); assert(flash.writes > writes);
    writes = flash.writes; h.clock.now += 100000; h.companion.loop(); assert(flash.writes == writes);
  }
  remount(flash);
  {
    CompanionHarness h; h.clock.now = 2;
    assert(h.store.begin(h.clock.now) && h.bot.getNumContacts() == 8 && !h.bot.txBusy());
    auto *first = h.bot.lookupContactByPubKey(contacts[0].pub_key, 32);
    assert(first && first->sync_since == 1850000000 && !first->shared_secret_valid &&
           first->last_advert_timestamp == 1900000000 && first->out_path_len == 1);
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 7);
    std::vector<uint8_t> reset(33); reset[0] = CMD_RESET_PATH;
    memcpy(reset.data() + 1, contacts[0].pub_key, 32);
    assert(h.command(reset) == std::vector<uint8_t>{RESP_CODE_OK});
    reset[0] = CMD_REMOVE_CONTACT;
    assert(h.command(reset) == std::vector<uint8_t>{RESP_CODE_OK});
    assert(h.bot.getNumContacts() == 7);
    assert(h.command({CMD_SET_RADIO_TX_POWER, 22}) == std::vector<uint8_t>{RESP_CODE_DISABLED});
    assert(h.command({CMD_EXPORT_PRIVATE_KEY}) == std::vector<uint8_t>{RESP_CODE_DISABLED});
    h.command({CMD_DEVICE_QUERY, 3});
    mesh::Packet packet{}; packet.header = ROUTE_TYPE_FLOOD; packet._snr = 8;
    h.companion.channelReceived(0, packet, h.rtc.now, "peer: hello");
    auto inbox = h.command({CMD_SYNC_NEXT_MESSAGE});
    assert(inbox[0] == RESP_CODE_CHANNEL_MSG_RECV_V3 && inbox[1] == 8 && inbox[4] == 0 &&
           inbox[6] == TXT_TYPE_PLAIN && !memcmp(inbox.data() + 11, "peer: hello", 11));
    h.command({CMD_DEVICE_QUERY, 2});
    h.companion.channelReceived(0, packet, h.rtc.now, "old");
    assert(h.command({CMD_SYNC_NEXT_MESSAGE})[0] == RESP_CODE_CHANNEL_MSG_RECV);
    for (unsigned i = 0; i < 5; ++i) h.companion.channelReceived(0, packet, h.rtc.now, "bounded");
    assert(h.companion.inboxDrops == 1);
    std::vector<uint8_t> send{CMD_SEND_CHANNEL_TXT_MSG, TXT_TYPE_PLAIN, 0, 1, 2, 3, 4, 'h', 'i'};
    assert(h.command(send) == std::vector<uint8_t>{RESP_CODE_OK});
    assert(h.bot.queueLength() == 1);
    {
      CompanionHarness receiver;
      assert(receiver.store.begin(receiver.clock.now));
      receiver.command({CMD_DEVICE_QUERY, 3});
      auto *outgoing = h.pool.getOutboundByIdx(0);
      assert(outgoing);
      std::vector<uint8_t> raw(MAX_TRANS_UNIT);
      raw.resize(outgoing->writeTo(raw.data()));
      receiver.radio.input.push_back(raw);
      receiver.bot.loop();
      const auto received = receiver.command({CMD_SYNC_NEXT_MESSAGE});
      assert(received[0] == RESP_CODE_CHANNEL_MSG_RECV_V3 &&
             received[4] == 0 && received.size() > 11);
      assert(std::string(received.begin() + 11, received.end()).find(": hi") != std::string::npos);
    }
    send[2] = 3;
    assert(h.command(send) == (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_NOT_FOUND}));
    assert(h.command(channelFrame(0, "", 0)) == std::vector<uint8_t>{RESP_CODE_OK});
  }
  remount(flash);
  {
    CompanionHarness h;
    assert(h.store.begin(h.clock.now) && h.bot.getNumContacts() == 7);
    assert(!h.bot.lookupContactByPubKey(contacts[0].pub_key, 32));
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 0);
  }
  unsigned files = 0;
  assert(nrfmast::botFilesystem.visit("/metadata", [](const char *path, void *out) {
    if (!strcmp(path, "/metadata/pine-companion/state")) ++*static_cast<unsigned *>(out);
    return true;
  }, &files));
  assert(files == 1);
  assert(nrfmast::botFilesystem.end());
  puts("Pine companion: secured raw protocol, durable channels/contacts/routes, epochs, bounded inbox and backup inventory");
}
void adverts() {
  CompanionFlash flash; assert(nrfmast::botFilesystem.begin(flash, true));
  std::vector<uint8_t> key;
  {
    CompanionHarness h, source;
    assert(h.store.begin(h.clock.now));
    source.bot.self_id = mesh::LocalIdentity(&h.rng);
    key.assign(source.bot.self_id.pub_key, source.bot.self_id.pub_key + 32);
    auto *packet = source.bot.createSelfAdvert("signed");
    packet->header |= ROUTE_TYPE_FLOOD;
    std::vector<uint8_t> frame(MAX_TRANS_UNIT + 1); frame[0] = CMD_IMPORT_CONTACT;
    frame.resize(packet->writeTo(frame.data() + 1) + 1); source.bot.releasePacket(packet);
    assert(h.command(frame) == std::vector<uint8_t>{RESP_CODE_OK});
    h.bot.loop();
    assert(h.bot.lookupContactByPubKey(key.data(), 32));
    std::vector<uint8_t> exportFrame(33); exportFrame[0] = CMD_EXPORT_CONTACT;
    memcpy(exportFrame.data() + 1, key.data(), 32);
    auto saved = h.command(exportFrame);
    assert(saved[0] == RESP_CODE_EXPORT_CONTACT && saved.size() == frame.size());
    assert(!memcmp(saved.data() + 1, frame.data() + 1, frame.size() - 1));
    h.clock.now += nrfmast::CompanionStore::FlushMs;
    h.companion.loop();
  }
  remount(flash);
  {
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    std::vector<uint8_t> frame(33); memcpy(frame.data() + 1, key.data(), 32);
    frame[0] = CMD_EXPORT_CONTACT;
    assert(h.command(frame)[0] == RESP_CODE_EXPORT_CONTACT);
    frame[0] = CMD_SHARE_CONTACT;
    assert(h.command(frame) == std::vector<uint8_t>{RESP_CODE_OK} && h.bot.queueLength() == 1);
  }
  assert(nrfmast::botFilesystem.end());
  puts("Pine companion: signed advert import/export/share survives restart");
}
void failures() {
  CompanionFlash flash; assert(nrfmast::botFilesystem.begin(flash, true));
  {
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    for (unsigned i = 1; i <= 8; ++i)
      assert(h.command(channelFrame(0, "saved", i)) == std::vector<uint8_t>{RESP_CODE_OK});
    assert(h.command(channelFrame(0, "limited", 9)) ==
           (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_FILE_IO_ERROR}));
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 8);
    h.clock.now += nrfmast::CompanionStore::RefillMs;
    failCompanionAllocation = true;
    assert(h.command(channelFrame(0, "no memory", 9)) ==
           (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_FILE_IO_ERROR}));
    failCompanionAllocation = false;
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 8);
    h.clock.now += nrfmast::CompanionStore::RefillMs;
    flash.cut = flash.writes + 1;
    assert(h.command(channelFrame(0, "failed", 10)) ==
           (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_FILE_IO_ERROR}));
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 8);
  }
  remount(flash);
  {
    CompanionHarness wrongOwner;
    wrongOwner.bot.self_id = mesh::LocalIdentity(&wrongOwner.rng);
    assert(!wrongOwner.store.begin(wrongOwner.clock.now));
  }
  {
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 8);
    auto file = nrfmast::botFilesystem.open("/metadata/pine-companion/state", "r+");
    uint8_t corrupt = 0; assert(file && file.write(&corrupt, 1) == 1); file.close();
  }
  remount(flash);
  {
    CompanionHarness h; assert(!h.store.begin(h.clock.now));
    assert(h.bot.getNumContacts() == 0);
    assert(h.command(channelFrame(0, "blocked", 1)) ==
           (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_FILE_IO_ERROR}));
    assert(nrfmast::botFilesystem.exists("/metadata/pine-companion/state"));
  }
  assert(nrfmast::botFilesystem.end());
  puts("Pine companion: write budget, torn stage, retained previous state and corrupt-journal refusal");
}
void interruptedPublicationAndFullStorage() {
  CompanionFlash flash; assert(nrfmast::botFilesystem.begin(flash, true));
  {
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    assert(h.command(channelFrame(0, "before", 1)) == std::vector<uint8_t>{RESP_CODE_OK});
  }
  assert(nrfmast::botFilesystem.end());
  const auto baseline = flash.bytes;
  unsigned writes;
  {
    remount(flash);
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    writes = flash.writes;
    assert(h.command(channelFrame(0, "after", 2)) == std::vector<uint8_t>{RESP_CODE_OK});
    writes = flash.writes - writes;
    assert(nrfmast::botFilesystem.end());
  }
  for (unsigned cut = 1; cut <= writes; ++cut) {
    flash.bytes = baseline; remount(flash);
    {
      CompanionHarness h; assert(h.store.begin(h.clock.now));
      flash.cut = flash.writes + cut;
      assert(h.command(channelFrame(0, "after", 2)) ==
             (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_FILE_IO_ERROR}));
      ChannelDetails channel{};
      assert(h.bot.getChannel(0, channel) && channel.channel.secret[0] == 1);
    }
    remount(flash);
    {
      CompanionHarness h; assert(h.store.begin(h.clock.now));
      auto frame = h.command({CMD_GET_CHANNEL, 0});
      assert(frame[34] == 1 || frame[34] == 2);
    }
    assert(nrfmast::botFilesystem.end());
  }
  flash.bytes = baseline; remount(flash);
  {
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    uint8_t block[4096]{};
    for (unsigned i = 0; i < 512; ++i) {
      char path[48]; snprintf(path, sizeof(path), "/command-bot/full-%u", i);
      auto filler = nrfmast::botFilesystem.open(path, "w");
      if (!filler) break;
      const auto written = filler.write(block, sizeof(block));
      filler.close();
      if (written != sizeof(block) || !nrfmast::botFilesystem.ready()) break;
      assert(i < 511);
    }
    assert(h.command(channelFrame(0, "full", 9)) ==
           (std::vector<uint8_t>{RESP_CODE_ERR, ERR_CODE_FILE_IO_ERROR}));
    ChannelDetails channel{};
    assert(h.bot.getChannel(0, channel) && channel.channel.secret[0] == 1);
  }
  remount(flash);
  {
    CompanionHarness h; assert(h.store.begin(h.clock.now));
    assert(h.command({CMD_GET_CHANNEL, 0})[34] == 1);
  }
  assert(nrfmast::botFilesystem.end());
  printf("Pine companion: all %u publication write cuts and full filesystem retain recoverable state\n", writes);
}
}
int main() {
  testCompanion();
  persistenceAndProtocol(); adverts(); failures(); interruptedPublicationAndFullStorage();
  printf("Companion journal bytes=%zu; store handle bytes=%zu\n",
         sizeof(nrfmast::CompanionStore::Record), sizeof(nrfmast::CompanionStore));
}
