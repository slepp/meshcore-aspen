// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
static char output[16];
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "wadd", 4, "a:int:-1000000:1000000,b:int:-1000000:1000000", 45,
             "Add two integers", 16, MC_PUBLIC);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t command) {
  int32_t a, b;
  (void)command;
  mc_read(job, MC_ARG0, &a, sizeof(a)); mc_read(job, MC_ARG1, &b, sizeof(b));
  int32_t sum = a + b;
  unsigned n = 0, negative = sum < 0;
  uint32_t value = negative ? (uint32_t)-sum : (uint32_t)sum;
  do { output[n++] = '0' + value % 10; value /= 10; } while (value);
  if (negative) output[n++] = '-';
  for (unsigned i = 0; i < n / 2; ++i) {
    char c = output[i]; output[i] = output[n - i - 1]; output[n - i - 1] = c;
  }
  mc_reply(job, output, n); return MC_DONE;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  (void)job; (void)operation; (void)ok; return MC_DONE;
}
