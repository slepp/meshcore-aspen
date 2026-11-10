#pragma once
#include "dns.h"
inline err_t tcpip_try_callback(void (*callback)(void *), void *context) {
  if (dns_test::queueFull) return ERR_MEM;
  callback(context); return ERR_OK;
}
