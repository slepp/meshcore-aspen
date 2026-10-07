// SPDX-License-Identifier: Apache-2.0
// Thin allocation-free ABI over the pinned native crypto, not new primitives.
#include "ed25519/ed_25519.h"
#include "crypto/AES.h"
#include "crypto/SHA256.h"
#include "crypto/Crypto.h"
#include <string.h>
static unsigned char arena[2048];
extern "C" {
void *memcpy(void *d,const void *s,size_t n){auto *a=(unsigned char*)d;auto *b=(const unsigned char*)s;for(size_t i=0;i<n;++i)a[i]=b[i];return d;}
void *memset(void *d,int c,size_t n){auto *a=(unsigned char*)d;for(size_t i=0;i<n;++i)a[i]=(unsigned char)c;return d;}
int memcmp(const void *a,const void *b,size_t n){auto *x=(const unsigned char*)a;auto *y=(const unsigned char*)b;for(size_t i=0;i<n;++i)if(x[i]!=y[i])return x[i]-y[i];return 0;}
void __cxa_pure_virtual(){__builtin_trap();}
unsigned char *mc_arena(){return arena;}
void mc_pub(){ed25519_derive_pub(arena+64,arena);}
void mc_seed(){ed25519_create_keypair(arena+64,arena,arena+96);}
int mc_shared(){ed25519_key_exchange(arena+128,arena+96,arena);unsigned n=0;for(unsigned i=128;i<160;++i)n|=arena[i];return n!=0;}
void mc_sha(unsigned n){SHA256 sha;sha.update(arena+160,n);sha.finalize(arena+1024,32);}
// MeshCore d929643 TransportKey::calcTransportCode: HMAC over type+payload,
// little-endian first two bytes, with 0000/FFFF reserved. Public scope key: 16B.
unsigned mc_transport(unsigned kind,unsigned n){
 if(kind>15||n>184)return 0;
 SHA256 sha;unsigned char type=kind;
 sha.resetHMAC(arena+128,16);sha.update(&type,1);sha.update(arena+160,n);
 sha.finalizeHMAC(arena+128,16,arena+1024,2);
 unsigned code=arena[1024]|(unsigned(arena[1025])<<8);
 return code==0?1:code==0xFFFF?0xFFFE:code;
}
void mc_sign(unsigned n){ed25519_sign(arena+1024,arena+160,n,arena+64,arena);}
int mc_verify(unsigned n){return ed25519_verify(arena+1088,arena+160,n,arena+64);}
int mc_crypt(unsigned n,int decrypt){
 if(n>256||!n)return 0;
 AES128 aes;aes.setKey(arena+128,16);
 SHA256 sha;
 if(decrypt){
  if(n<18||(n-2)%16)return 0;
  unsigned char mac[2];sha.resetHMAC(arena+128,32);sha.update(arena+162,n-2);sha.finalizeHMAC(arena+128,32,mac,2);
  if((mac[0]^arena[160])|(mac[1]^arena[161]))return 0;
  for(unsigned i=0;i<n-2;i+=16)aes.decryptBlock(arena+1024+i,arena+162+i);
  return n-2;
 }
 const unsigned count=(n+15)&~15u;
 if(count>256)return 0;
 memset(arena+160+n,0,count-n);
 for(unsigned i=0;i<count;i+=16)aes.encryptBlock(arena+1026+i,arena+160+i);
 sha.resetHMAC(arena+128,32);sha.update(arena+1026,count);sha.finalizeHMAC(arena+128,32,arena+1024,2);
 return count+2;
}
}
void operator delete(void*) noexcept {}
void operator delete(void*,size_t) noexcept {}
