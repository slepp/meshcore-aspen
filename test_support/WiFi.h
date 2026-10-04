#pragma once

#include <deque>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

class IPAddress {
public:
  std::string toString() const { return "127.0.0.1"; }
};

class WiFiClient {
  struct State {
    int fd = -1;
    ~State() { if (fd >= 0) ::close(fd); }
  };
  std::shared_ptr<State> state;
public:
  WiFiClient() : state(std::make_shared<State>()) {}
  explicit WiFiClient(int fd) : state(std::make_shared<State>()) { state->fd = fd; }
  explicit operator bool() const { return state->fd >= 0; }
  int fd() const { return state->fd; }
  void setNoDelay(bool) {}
  IPAddress remoteIP() const { return {}; }
  int available() {
    if (state->fd < 0) return 0;
    int bytes = 0;
    ioctl(state->fd, FIONREAD, &bytes);
    return bytes;
  }
  int read() {
    unsigned char byte;
    return ::recv(state->fd, &byte, 1, 0) == 1 ? byte : -1;
  }
  size_t write(const uint8_t* data, size_t length) {
    return state->fd < 0 ? 0 : ::send(state->fd, data, length, 0);
  }
  void flush() {}
  void stop() {
    if (state->fd >= 0) ::close(state->fd);
    state->fd = -1;
  }
};

class WiFiServer {
  std::deque<WiFiClient> clients;
public:
  explicit WiFiServer(uint16_t = 0) {}
  void add(WiFiClient client) { clients.push_back(client); }
  WiFiClient accept() {
    if (clients.empty()) return {};
    auto client = clients.front();
    clients.pop_front();
    return client;
  }
};
