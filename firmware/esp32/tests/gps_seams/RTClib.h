#pragma once
#include <cstdint>
#include <ctime>
class DateTime {
  std::tm value{};
public:
  DateTime(int year, int month, int day, int hour, int minute, int second) {
    value.tm_year = year - 1900; value.tm_mon = month - 1; value.tm_mday = day;
    value.tm_hour = hour; value.tm_min = minute; value.tm_sec = second;
  }
  uint32_t unixtime() { return uint32_t(timegm(&value)); }
};
