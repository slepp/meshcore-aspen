#pragma once
#include <cstdint>
using err_t = int;
constexpr int ERR_OK = 0, ERR_INPROGRESS = -5, ERR_MEM = -1;
constexpr int LWIP_DNS_ADDRTYPE_IPV4 = 0;
struct ip_addr_t { uint32_t value = 0; };
#define IP_IS_V4(address) true
#define ip_2_ip4(address) (address)
#define ip4_addr_get_u32(address) ((address)->value)
namespace dns_test {
inline bool deferred = false, failure = false, queueFull = false;
inline unsigned queries = 0, clears = 0;
inline uint32_t answer = 0x010200c0;
inline void (*callback)(const char *, const ip_addr_t *, void *) = nullptr;
inline void *context = nullptr;
inline void finish() {
  ip_addr_t address{answer};
  auto cb = callback; callback = nullptr;
  cb(nullptr, failure ? nullptr : &address, context);
}
}
inline void dns_clear_cache() { ++dns_test::clears; }
inline err_t dns_gethostbyname_addrtype(const char *, ip_addr_t *out,
    void (*callback)(const char *, const ip_addr_t *, void *), void *context, int) {
  ++dns_test::queries;
  if (dns_test::deferred) {
    dns_test::callback = callback; dns_test::context = context; return ERR_INPROGRESS;
  }
  out->value = dns_test::answer;
  return dns_test::failure ? -1 : ERR_OK;
}
