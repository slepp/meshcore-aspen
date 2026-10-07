// SPDX-License-Identifier: Apache-2.0
#include "OpaqueFrontend.h"
#include <assert.h>
#include <string>
using namespace cloudroom;
int main() {
 RadioBuffers radioBuffers;RadioBridge radio;radio.bind(radioBuffers);
 OpaqueBuffers buffers;OpaqueAlias alias;alias.id="A";alias.name="Room";memset(alias.publicKey,0x11,32);
 OpaqueFrontend f(radio,&alias,1,buffers);f.opened(0,radio.generation(0));
 const std::string ready="{\"type\":\"ready\",\"version\":2,\"alias\":\"A\",\"name\":\"Room\",\"publicKey\":\""+std::string(64,'1')+"\"}";
 assert(f.frame(0,ready.data(),ready.size()));
 Reception rx;rx.size=3;rx.bytes[0]=0;rx.bytes[1]=1;rx.bytes[2]=255;f.received(rx);
 char out[512]{};size_t n=f.operation(0,out,sizeof(out));assert(n);assert(std::string(out,n).find("AAH/")!=std::string::npos);
 const char *tx="{\"type\":\"transmit\",\"alias\":\"A\",\"dispatchId\":\"00000000-0000-0000-0000-000000000001\",\"packet\":\"AAH/\",\"delayMs\":300,\"priority\":0}";
 assert(f.frame(0,tx,strlen(tx)));assert(f.frame(0,tx,strlen(tx)));
 Transmission job;assert(radio.tx.pop(job));assert(job.size==3&&job.bytes[2]==255&&job.delayMs==300);assert(!radio.tx.pop(job));
 for(unsigned i=0;i<QueueDepth;++i)f.received(rx);
 Receipt receipt;receipt.cookie=job.cookie;receipt.alias=0;receipt.generation=job.generation;receipt.outcome=Receipt::Sent;f.receipt(receipt);
 n=f.operation(0,out,sizeof(out));assert(n);assert(std::string(out,n).find("\"outcome\":\"sent\"")!=std::string::npos);
 f.received(rx);radio.disconnect(0);f.disconnected(0);f.opened(0,radio.generation(0));
 assert(!f.operation(0,out,sizeof(out)));assert(!f.frame(0,tx,strlen(tx)));
 std::string wrong=ready;wrong.replace(wrong.find("Room"),4,"Else");assert(!f.frame(0,wrong.data(),wrong.size()));
 assert(f.frame(0,ready.data(),ready.size()));assert(!f.operation(0,out,sizeof(out)));
}
