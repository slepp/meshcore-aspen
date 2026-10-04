#pragma once
#include <cstdint>
namespace mesh {
class RTCClock {
public:
  virtual ~RTCClock() = default;
  virtual void setCurrentTime(uint32_t) = 0;
};
}
