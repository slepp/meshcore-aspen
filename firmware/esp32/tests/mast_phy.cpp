// SPDX-License-Identifier: Apache-2.0
#define main radio_only_mux_tests
#include "../../../test_support/phy_parity/combined.cpp"
#undef main

static void role_claims() {
  Radio radio;
  RNG rng;
  WifiKissMultiplexer mux;
  mux.attachRadio(radio, rng, configure, power);
  uint8_t native[2][32]{};
  native[0][0] = 0x31; // modem
  native[1][0] = 0x32; // active repeater
  assert(mux.setNativeRolePresence(1, native[0], 2));
  WiFiServer server;
  int peers[3];
  for (int &peer : peers) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    server.add(WiFiClient(fds[0]));
    peer = fds[1];
  }
  mux.poll(server);
  auto command = [&](int peer, const std::vector<uint8_t>& payload) {
    auto wire = encode(6, payload);
    assert(send(peers[peer], wire.data(), wire.size(), 0) ==
           static_cast<ssize_t>(wire.size()));
    mux.poll(server);
    return receive(peers[peer]);
  };
  auto claim = [&](int peer, uint8_t role, uint8_t key) {
    std::vector<uint8_t> payload{queued_tx::ROLE_PRESENCE, 1, role};
    payload.resize(3 + queued_tx::ROLE_KEY_SIZE);
    payload[3] = key;
    return command(peer, payload);
  };
  auto result = [](const std::vector<std::vector<uint8_t>>& frames,
                   uint8_t reason, uint8_t mask, uint8_t warnings) {
    assert(frames.size() == 1 && frames[0].size() == 10 &&
           frames[0][0] == 6 && frames[0][1] == 0xa6 &&
           frames[0][2] == 1 && frames[0][3] == reason &&
           frames[0][8] == mask && frames[0][9] == warnings);
    return queued_tx::get32(frames[0].data() + 4);
  };
  assert(result(claim(0, 1, 0x41), queued_tx::INVALID, 1, 0) != 0);
  assert(mux.clientCount() == 3);
  uint32_t generations[3]{};
  for (int i = 0; i < 3; ++i) {
    auto frames = command(i, {queued_tx::HELLO, 1, 0});
    assert(frames.size() == 1 && frames[0][3] == queued_tx::NONE);
    generations[i] = queued_tx::get32(frames[0].data() + 4);
  }
  assert(result(claim(0, 0, 0x31), queued_tx::NONE, 1,
                queued_tx::NATIVE_ROLE_PRESENT | queued_tx::NATIVE_KEY_PRESENT) ==
         generations[0]);
  assert(result(claim(1, 0, 0x43), queued_tx::NONE, 1,
                queued_tx::NATIVE_ROLE_PRESENT | queued_tx::TCP_ROLE_PRESENT) ==
         generations[1]);
  assert(result(claim(2, 1, 0x31), queued_tx::NONE, 1,
                queued_tx::NATIVE_KEY_PRESENT | queued_tx::TCP_KEY_PRESENT) ==
         generations[2]);
  assert(result(claim(0, 0, 0x31), queued_tx::NONE, 1, 0x0f) ==
         generations[0]);
  mux.setActiveRolePresence(0);
  assert(result(claim(0, 0, 0x31), queued_tx::NONE, 1,
                queued_tx::TCP_ROLE_PRESENT | queued_tx::NATIVE_KEY_PRESENT |
                    queued_tx::TCP_KEY_PRESENT) == generations[0]);
  mux.setActiveRolePresence(1);
  assert(result(claim(0, 0, 0x31), queued_tx::NONE, 1, 0x0f) ==
         generations[0]);
  assert(mux.clientCount() == 3);
  for (int &peer : peers) close(peer);
  mux.poll(server);
  assert(mux.clientCount() == 0);

  int sockets[3][2];
  for (auto &pair : sockets) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    server.add(WiFiClient(pair[0]));
  }
  for (int i = 0; i < 3; ++i) peers[i] = sockets[i][1];
  mux.poll(server);
  for (int i = 0; i < 3; ++i) {
    auto frames = command(i, {queued_tx::HELLO, 1, 0});
    assert(frames.size() == 1 && frames[0][3] == queued_tx::NONE);
    generations[i] = queued_tx::get32(frames[0].data() + 4);
  }
  assert(result(claim(0, 1, 0x41), queued_tx::NONE, 1, 0) == generations[0]);
  assert(result(claim(0, 1, 0x41), queued_tx::NONE, 1, 0) == generations[0]);
  assert(result(claim(1, 2, 0x42), queued_tx::NONE, 1, 0) == generations[1]);
  assert(result(claim(2, 3, 0x41), queued_tx::NONE, 1,
                queued_tx::TCP_KEY_PRESENT) == generations[2]);
  assert(result(claim(2, 1, 0x41), queued_tx::NONE, 1,
                queued_tx::TCP_ROLE_PRESENT | queued_tx::TCP_KEY_PRESENT) ==
         generations[2]);
  assert(mux.clientCount() == 3);
  close(peers[2]);
  mux.poll(server);
  assert(result(claim(0, 1, 0x41), queued_tx::NONE, 1, 0) == generations[0]);
  close(peers[0]);
  mux.poll(server);
  int reconnect[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, reconnect) == 0);
  peers[0] = reconnect[1];
  server.add(WiFiClient(reconnect[0]));
  mux.poll(server);
  auto hello = command(0, {queued_tx::HELLO, 1, 0});
  const uint32_t fresh = queued_tx::get32(hello[0].data() + 4);
  assert(fresh != generations[0]);
  assert(result(claim(0, 2, 0x42), queued_tx::NONE, 1,
                queued_tx::TCP_ROLE_PRESENT | queued_tx::TCP_KEY_PRESENT) ==
         fresh);
  close(peers[1]);
  mux.poll(server);
  assert(result(claim(0, 2, 0x42), queued_tx::NONE, 1, 0) == fresh);
  close(peers[0]);
  mux.poll(server);
  std::vector<std::vector<uint8_t>> malformed = {
      {queued_tx::ROLE_PRESENCE, 2, 3},
      {queued_tx::ROLE_PRESENCE, 1, 4},
      {queued_tx::ROLE_PRESENCE, 1, 3, 0x4a}};
  malformed.emplace_back(3 + queued_tx::ROLE_KEY_SIZE, 0);
  malformed.back()[0] = queued_tx::ROLE_PRESENCE;
  malformed.back()[1] = 1;
  malformed.back()[2] = 3;
  for (auto payload : malformed) {
    int invalid[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, invalid) == 0);
    peers[0] = invalid[1];
    server.add(WiFiClient(invalid[0]));
    mux.poll(server);
    hello = command(0, {queued_tx::HELLO, 1, 0});
    assert(hello[0][3] == queued_tx::NONE);
    if (payload.size() == 3)
      payload.resize(3 + queued_tx::ROLE_KEY_SIZE, 0x4a);
    assert(result(command(0, payload), queued_tx::INVALID, 1, 0) ==
           queued_tx::get32(hello[0].data() + 4));
    assert(mux.clientCount() == 1);
    assert(result(claim(0, 3, 0x4a), queued_tx::NONE, 1, 0) ==
           queued_tx::get32(hello[0].data() + 4));
    close(peers[0]);
    mux.poll(server);
  }
  assert(mux.clientCount() == 0);
}

static void overlapping_presence_during_transmit() {
  nvs_test::reset();
  Radio radio;
  RNG rng;
  WifiKissMultiplexer bootstrap;
  bootstrap.attachRadio(radio, rng, configure, power);
  assert(bootstrap.setInitialConfiguration({910525000, 62500, 7, 5, 20}, true));
  Fixture mast(1, true, true);
  auto request = [](uint8_t role, uint8_t key) {
    std::vector<uint8_t> payload{queued_tx::ROLE_PRESENCE, 1, role};
    payload.resize(35);
    payload[3] = key;
    return payload;
  };
  mast.hardware(0, request(1, 0x52));
  auto answer = receive(mast.peers[0]);
  assert(answer.size() == 1 && answer[0].size() == 10 &&
         answer[0][1] == 0xa6 && answer[0][3] == queued_tx::NONE &&
         queued_tx::get32(answer[0].data() + 4) == mast.generations[0] &&
         answer[0][8] == 0 && answer[0][9] == 0);
  mast.job(0, 1, 0, 0, 0x5a);
  assert(mast.radio.sending &&
         mast.radio.transmitted[0] == std::vector<uint8_t>{0x5a});
  assert(event(receive(mast.peers[0]), queued_tx::ACCEPTED));
  mast.hardware(1, request(1, 0x52));
  answer = receive(mast.peers[1]);
  assert(answer.size() == 1 && answer[0].size() == 10 &&
         answer[0][3] == queued_tx::NONE &&
         answer[0][9] == (queued_tx::TCP_ROLE_PRESENT |
                          queued_tx::TCP_KEY_PRESENT));
  assert(mast.mux.clientCount() == 2 && mast.radio.sending);
  mast.job(1, 1, 0, 0, 0x5b);
  assert(event(receive(mast.peers[1]), queued_tx::ACCEPTED));
  mast.finish();
  assert(event(receive(mast.peers[0]), queued_tx::SUCCEEDED));
  assert(mast.radio.transmitted.size() == 2 &&
         mast.radio.transmitted[1] == std::vector<uint8_t>{0x5b});
  mast.finish();
  assert(event(receive(mast.peers[1]), queued_tx::SUCCEEDED));
}

int main() {
  role_claims();
  overlapping_presence_during_transmit();
  nvs_test::reset();
  Fixture mast(1, true, true);
  assert(nvs_test::store.writes == 1 && nvs_test::store.commits == 1);
  const auto saved = nvs_test::store.durable;
  const unsigned changes = applied.changes, powers = applied.power_changes;
  auto request = readConfigForUpdate(mast);
  const auto original = request;
  const uint8_t changed_offsets[] = {7, 11, 15, 16, 17, 18, 22, 23};
  for (uint8_t offset : changed_offsets) {
    request = original;
    ++request[offset];
    mast.hardware(0, request);
    const auto frames = receive(mast.peers[0]);
    assert(frames.size() == 1 && frames[0].size() == 26 &&
           frames[0][3] == queued_tx::NOT_OWNER);
    assert(queued_tx::get32(frames[0].data() + 4) ==
           queued_tx::get32(original.data() + 3));
    assert(memcmp(frames[0].data() + 8, original.data() + 7,
                  queued_tx::PROFILE_SIZE) == 0);
    assert(nvs_test::store.durable == saved && nvs_test::store.writes == 1 &&
           applied.changes == changes && applied.power_changes == powers);
  }
  mast.hardware(0, original);
  assert(receive(mast.peers[0])[0][3] == queued_tx::NONE);
  assert(nvs_test::store.writes == 1 && applied.changes == changes);
  mast.hardware(1, {queued_tx::CAPACITY, queued_tx::VERSION});
  assert(receive(mast.peers[1]) == std::vector<std::vector<uint8_t>>(
      {{6, HW_RESP(queued_tx::CAPACITY), queued_tx::VERSION, 4, 4}}));

  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  mast.server.add(WiFiClient(fds[0]));
  mast.step();
  auto wire = encode(6, {queued_tx::CAPACITY, queued_tx::VERSION});
  assert(send(fds[1], wire.data(), wire.size(), 0) ==
         static_cast<ssize_t>(wire.size()));
  mast.step();
  assert(receive(fds[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
  close(fds[1]);

  mast.hardware(0, {queued_tx::CONFIG, queued_tx::VERSION, 0});
  assert(receive(mast.peers[0])[0][3] == queued_tx::NONE);
  mast.job(1, 1, 0, 0, 0xaa);
  assert(mast.radio.transmitted == std::vector<std::vector<uint8_t>>{{0xaa}});
  {
    Fixture rebooted(1, true, true);
    assert(nvs_test::store.durable == saved && nvs_test::store.writes == 1);
    assert(readConfigForUpdate(rebooted) == original);
  }
  puts("combined mast advisory presence, profile guard and read-only capacity passed");
}
