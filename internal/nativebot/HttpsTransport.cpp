// SPDX-License-Identifier: Apache-2.0
#include "HttpsTransport.h"
#include "NativeClock.h"
#include <Arduino.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <chrono>
#include <thread>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <new>

namespace onchip {
NativeBotHttpsTransport::NativeBotHttpsTransport() {
  size_t received = 0;
  auto *bytes = reinterpret_cast<uint8_t *>(&nonce_);
  while (received < sizeof(nonce_)) {
    const ssize_t count = getrandom(bytes + received, sizeof(nonce_) - received, 0);
    if (count > 0) received += size_t(count);
    else if (count < 0 && errno == EINTR) continue;
    else { nonce_ = 0; break; }
  }
}
bool nativeBotHttpsClockTrusted() {
  NativeClockSample sample;
  nativeBotClockSample(sample);
  return sample.httpsTrusted;
}
bool NativeBotHttpsTransport::validate(char *error, size_t size) {
  if (!nonce_) {
    snprintf(error, size, "Host HTTPS request-ID entropy unavailable");
    return false;
  }
  if (!nativeBotHttpsClockTrusted()) {
    snprintf(error, size, "Host HTTPS requires a synchronized system clock");
    return false;
  }
  if (ssl_) {
    const auto *chain = SSL_get0_verified_chain(ssl_);
    for (int i = 0; chain && i < sk_X509_num(chain); ++i) {
      const auto *certificate = sk_X509_value(chain, i);
      if (X509_cmp_current_time(X509_get0_notBefore(certificate)) != -1 ||
          X509_cmp_current_time(X509_get0_notAfter(certificate)) != 1) {
        snprintf(error, size, "Host HTTPS certificate date is outside current trusted time");
        return false;
      }
    }
  }
  return true;
}
bool NativeBotHttpsTransport::open(const BotHttpsConfig &config, char *error, size_t size) {
  close();
  transmitted = 0;
  const auto fail = [&]() {
    snprintf(error, size, "Host TLS connection or CA/hostname/date verification failed");
    close();
    return false;
  };
  if (!config.validPeer() || !validate(error, size)) return false;
  context_ = SSL_CTX_new(TLS_client_method());
  if (!context_ || !SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION)) return fail();
  SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
  BIO *bundle = BIO_new_mem_buf(config.ca, -1);
  if (!bundle) return fail();
  unsigned certificates = 0;
  while (X509 *certificate = PEM_read_bio_X509(bundle, nullptr, nullptr, nullptr)) {
    const int added = X509_STORE_add_cert(SSL_CTX_get_cert_store(context_), certificate);
    X509_free(certificate);
    if (added != 1) { BIO_free(bundle); return fail(); }
    ++certificates;
  }
  BIO_free(bundle);
  if (!certificates) return fail();
  socket_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (socket_ < 0) return fail();
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(config.port);
  if (inet_pton(AF_INET, config.address, &target.sin_addr) != 1) return fail();
  const uint32_t deadline = now() + 2000;
  const auto ready = [&](short events) {
    while (int32_t(deadline - now()) > 0) {
      pollfd descriptor{socket_, events, 0};
      const int result = poll(&descriptor, 1, int(deadline - now()));
      if (result > 0) return (descriptor.revents & events) != 0;
      if (result < 0 && errno == EINTR) continue;
      return false;
    }
    return false;
  };
  if (connect(socket_, reinterpret_cast<sockaddr *>(&target), sizeof(target))) {
    if (errno != EINPROGRESS || !ready(POLLOUT)) return fail();
    int outcome = 0;
    socklen_t length = sizeof(outcome);
    if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &outcome, &length) || outcome) return fail();
  }
  ssl_ = SSL_new(context_);
  if (!ssl_ || SSL_set_fd(ssl_, socket_) != 1 ||
      SSL_set_tlsext_host_name(ssl_, config.host) != 1 ||
      SSL_set1_host(ssl_, config.host) != 1) return fail();
  X509_VERIFY_PARAM_set_hostflags(SSL_get0_param(ssl_), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  for (;;) {
    const int result = SSL_connect(ssl_);
    if (result == 1) break;
    const int condition = SSL_get_error(ssl_, result);
    if (condition != SSL_ERROR_WANT_READ && condition != SSL_ERROR_WANT_WRITE) return fail();
    if (!ready(condition == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT)) return fail();
  }
  if (SSL_get_verify_result(ssl_) != X509_V_OK || !validate(error, size)) return fail();
  return true;
}
int NativeBotHttpsTransport::write(const uint8_t *bytes, size_t size) {
  if (!ssl_) return -1;
  const int result = SSL_write(ssl_, bytes, int(size));
  if (result > 0) { transmitted += unsigned(result); return result; }
  const int condition = SSL_get_error(ssl_, result);
  return condition == SSL_ERROR_WANT_READ || condition == SSL_ERROR_WANT_WRITE ? 0 : -1;
}
int NativeBotHttpsTransport::read(uint8_t *bytes, size_t size) {
  if (!ssl_) return -1;
  const int result = SSL_read(ssl_, bytes, int(size));
  if (result > 0) return result;
  const int condition = SSL_get_error(ssl_, result);
  return condition == SSL_ERROR_WANT_READ || condition == SSL_ERROR_WANT_WRITE ? 0 : -1;
}
void NativeBotHttpsTransport::close() {
  if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
  if (context_) { SSL_CTX_free(context_); context_ = nullptr; }
  if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
}
uint32_t NativeBotHttpsTransport::now() const {
  return uint32_t(millis());
}
void NativeBotHttpsTransport::idle() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
BotHttpsTransport *createNativeBotHttpsTransport() {
  return new (std::nothrow) NativeBotHttpsTransport;
}
}
