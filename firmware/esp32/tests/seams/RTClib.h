#pragma once
#include <cstdint>
#include <ctime>
class DateTime {
  std::tm value{};

public:
  explicit DateTime(uint32_t epoch) {
    const time_t time = epoch;
    gmtime_r(&time, &value);
  }
  int year() const { return value.tm_year + 1900; }
  int month() const { return value.tm_mon + 1; }
  int day() const { return value.tm_mday; }
  int hour() const { return value.tm_hour; }
  int minute() const { return value.tm_min; }
  int second() const { return value.tm_sec; }
};
