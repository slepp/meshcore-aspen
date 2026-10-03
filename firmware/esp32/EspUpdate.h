// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace onchip {
// The IDF adapter owns hashing, image validation and inactive-slot selection.
class EspUpdate {
public:
  struct Backend {
    virtual ~Backend() = default;
    virtual bool begin(size_t size) = 0;
    virtual bool write(const uint8_t *data, size_t size) = 0;
    virtual bool verify() = 0;
    virtual bool activate() = 0;
    virtual void abort() = 0;
  };
  enum State { Idle, Receiving, Failed, Verified };
  bool start(Backend &backend, size_t size, size_t capacity, bool authorized) {
    if (busy()) return false;
    received_ = 0; size_ = size; backend_ = nullptr;
    if (!authorized) return fail("Signed operator manifest required");
    if (!size || size > capacity) return fail("Image exceeds inactive application partition");
    backend_ = &backend;
    if (!backend.begin(size)) return fail("Inactive application erase failed");
    state_ = Receiving; error_ = "";
    return true;
  }
  bool write(const uint8_t *data, size_t size) {
    if (state_ != Receiving) return false;
    if (!size || size > size_ - received_ || !backend_->write(data, size))
      return fail("Application transfer write failed");
    received_ += size;
    return true;
  }
  bool finish() {
    if (state_ != Receiving) return false;
    if (received_ != size_) return fail("Application transfer incomplete");
    if (!backend_->verify()) return fail("Application chip, image or SHA256 validation failed");
    if (!backend_->activate()) return fail("Application boot selection failed");
    backend_ = nullptr; state_ = Verified;
    return true;
  }
  bool fail(const char *error) {
    if (backend_) backend_->abort();
    backend_ = nullptr; error_ = error; state_ = Failed;
    return false;
  }
  bool busy() const { return state_ == Receiving || state_ == Verified; }
  State state() const { return state_; }
  size_t received() const { return received_; }
  size_t size() const { return size_; }
  const char *error() const { return error_; }
private:
  Backend *backend_ = nullptr;
  State state_ = Idle;
  size_t received_ = 0, size_ = 0;
  const char *error_ = "";
};
} // namespace onchip
