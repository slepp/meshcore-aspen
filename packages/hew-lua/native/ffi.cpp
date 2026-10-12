// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "hew.h"
#include <cstring>
extern "C" {
HluaSession *hlua_ffi_open(int64_t heap,int64_t fuel,int64_t parser,int64_t us,int64_t source,int64_t value,int64_t hosts) {
  if(heap<0||fuel<0||parser<0||us<0||source<0||source>1048576||value<0||value>65536||hosts<0||hosts>1000000)return nullptr;
  return hlua_open({uint64_t(heap),uint64_t(fuel),uint64_t(parser),uint64_t(us),uint32_t(source),uint32_t(value),uint32_t(hosts)});
}
int32_t hlua_ffi_valid(HluaSession *s){return s!=nullptr;}
int32_t hlua_ffi_load(HluaSession *s,const HewBytes *b){return hlua_load(s,hew_bytes_data(b),b->len);}
int32_t hlua_ffi_call(HluaSession *s,const HewBytes *b){return hlua_call(s,hew_bytes_data(b),b->len);}
int32_t hlua_ffi_argument(HluaSession *s,int32_t kind,int64_t integer,double number,const HewBytes *b) {
  return hlua_argument(s,{kind,integer,number,hew_bytes_data(b),b->len});
}
int32_t hlua_ffi_kind(HluaSession *s,int64_t index){return hlua_value(s,size_t(index)).kind;}
int64_t hlua_ffi_integer(HluaSession *s,int64_t index){return hlua_value(s,size_t(index)).integer;}
double hlua_ffi_number(HluaSession *s,int64_t index){return hlua_value(s,size_t(index)).number;}
HewBytes hlua_ffi_bytes(HluaSession *s,int64_t index){const auto v=hlua_value(s,size_t(index));return hew_bytes_copy(v.bytes,uint32_t(v.size));}
HewBytes hlua_ffi_operation(HluaSession *s){size_t size;const auto *b=hlua_operation(s,&size);return hew_bytes_copy(b,uint32_t(size));}
HewBytes hlua_ffi_error(HluaSession *s){const auto *e=hlua_error(s);return hew_bytes_copy(reinterpret_cast<const uint8_t *>(e),uint32_t(std::strlen(e)));}
}
