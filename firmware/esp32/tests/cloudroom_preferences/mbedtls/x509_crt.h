// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
struct mbedtls_x509_crt {};
namespace cloudroom_certificate_test {
inline int result = 0;
inline unsigned parsed = 0, freed = 0;
}
inline void mbedtls_x509_crt_init(mbedtls_x509_crt *) {}
inline int mbedtls_x509_crt_parse(mbedtls_x509_crt *, const unsigned char *, size_t) {
  ++cloudroom_certificate_test::parsed;
  return cloudroom_certificate_test::result;
}
inline void mbedtls_x509_crt_free(mbedtls_x509_crt *) { ++cloudroom_certificate_test::freed; }
