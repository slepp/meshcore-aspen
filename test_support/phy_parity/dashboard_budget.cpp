#include "RadioDashboard.h"
#include <cstdio>
#include <type_traits>
#include <utility>

template <class T, class = void> struct HasContacts : std::false_type {};
template <class T>
struct HasContacts<T, std::void_t<decltype(std::declval<T>().contacts)>> : std::true_type {};

#if defined(MESHCORE_ONCHIP) && defined(ESP32) && !defined(NRF52_PLATFORM)
static_assert(HasContacts<RadioDashboard::RadioStatus>::value, "ESP32 contact projection missing");
static_assert(RadioDashboard::JSON_CAPACITY == 40960, "ESP32 JSON budget changed");
#else
static_assert(!HasContacts<RadioDashboard::RadioStatus>::value, "Contacts increased a non-ESP32 snapshot");
static_assert(RadioDashboard::JSON_CAPACITY == 24576, "Non-ESP32 JSON budget changed");
#endif

int main() {
  printf("Dashboard budget: RadioStatus=%zu Snapshot=%zu RadioDashboard=%zu JSON=%zu contacts=%u\n",
         sizeof(RadioDashboard::RadioStatus), sizeof(RadioDashboard::Snapshot),
         sizeof(RadioDashboard), RadioDashboard::JSON_CAPACITY,
         unsigned(HasContacts<RadioDashboard::RadioStatus>::value));
}
