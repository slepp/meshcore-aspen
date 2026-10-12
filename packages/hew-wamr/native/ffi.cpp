// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "hew.h"
#include <cstring>
extern "C" {
int32_t hwamr_ffi_pool(int64_t bytes){return hwamr_pool(bytes>0&&bytes<=64*1024*1024?uint32_t(bytes):0);}
HewBytes hwamr_ffi_pool_error(){const auto *e=hwamr_pool_error();return hew_bytes_copy(reinterpret_cast<const uint8_t *>(e),uint32_t(std::strlen(e)));}
HwamrInstance *hwamr_ffi_open(const HewBytes *bytes,int64_t source,int64_t pages,int64_t stack,int64_t fuel,int64_t us) {
  HwamrLimits limits{};
  if(source>0&&source<=1048576&&pages>0&&pages<=256&&stack>0&&stack<=65536&&fuel>0&&fuel<=2147483647&&us>0)
    limits={uint32_t(source),uint32_t(pages),uint32_t(stack),uint32_t(fuel),uint64_t(us)};
  return hwamr_open(hew_bytes_data(bytes),bytes->len,limits);
}
int32_t hwamr_ffi_valid(HwamrInstance *s){return s!=nullptr;}
int32_t hwamr_ffi_argument(HwamrInstance *s,int32_t kind,int64_t integer,double number){return hwamr_argument(s,{kind,integer,number});}
int32_t hwamr_ffi_call(HwamrInstance *s,const HewBytes *name){return hwamr_call(s,hew_bytes_data(name),name->len);}
int32_t hwamr_ffi_kind(HwamrInstance *s,int64_t index){return hwamr_value(s,size_t(index)).kind;}
int64_t hwamr_ffi_integer(HwamrInstance *s,int64_t index){return hwamr_value(s,size_t(index)).integer;}
double hwamr_ffi_number(HwamrInstance *s,int64_t index){return hwamr_value(s,size_t(index)).number;}
int32_t hwamr_ffi_readable(HwamrInstance *s,uint64_t offset,uint64_t size){return hwamr_readable(s,size<=16777216?offset:UINT64_MAX,size<=16777216?size_t(size):0);}
HewBytes hwamr_ffi_read(HwamrInstance *s,uint64_t offset,uint64_t size) {
  HewBytes out{};if(size>16777216||!hwamr_readable(s,offset,size_t(size)))return out;
  if(size){out.ptr=hew_bytes_new(uint32_t(size));out.len=uint32_t(size);hwamr_read(s,offset,out.ptr,size_t(size));}
  return out;
}
int32_t hwamr_ffi_write(HwamrInstance *s,uint64_t offset,const HewBytes *bytes){return hwamr_write(s,offset,hew_bytes_data(bytes),bytes->len);}
HewBytes hwamr_ffi_error(HwamrInstance *s){const auto *e=hwamr_error(s);return hew_bytes_copy(reinterpret_cast<const uint8_t *>(e),uint32_t(std::strlen(e)));}
}
