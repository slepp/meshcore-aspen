// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>
#define EXPORT(name) __attribute__((export_name(name)))
EXPORT("add") int32_t add(int32_t a,int32_t b){return a+b;}
EXPORT("wide") int64_t wide(int64_t a,int64_t b){return a+b;}
EXPORT("single") float single(float a){return a*2.0f;}
EXPORT("double") double twice(double a){return a*2.0;}
EXPORT("store") void store(int32_t offset,int32_t byte){*(volatile uint8_t *)(uintptr_t)offset=(uint8_t)byte;}
EXPORT("load") int32_t load(int32_t offset){return *(volatile uint8_t *)(uintptr_t)offset;}
EXPORT("trap") void trap(void){__builtin_trap();}
EXPORT("spin") void spin(void){for(;;)__asm__ volatile("");}
EXPORT("grow") int32_t grow(int32_t pages){return __builtin_wasm_memory_grow(0,pages);}
#ifdef IMPORT
__attribute__((import_module("env"),import_name("host"))) extern int host(void);
EXPORT("imported") int imported(void){return host();}
#endif
#ifdef IMPLICIT
EXPORT("_initialize") void implicit(void){spin();}
#endif
