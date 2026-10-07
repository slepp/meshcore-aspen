// SPDX-License-Identifier: Apache-2.0
#include "RadioBridge.h"
#include <assert.h>
#include <thread>
using namespace cloudroom;
int main() {
 RadioBuffers buffers; RadioBridge bridge; bridge.bind(buffers);
 const uint8_t bytes[]={0,1,255};
 assert(!bridge.received(bytes,sizeof(bytes),-80,4,true));
 assert(bridge.received(bytes,sizeof(bytes),-80,4,false));
 Reception packet; assert(bridge.rx.pop(packet)); assert(packet.size==3&&packet.bytes[2]==255);
 Transmission job; job.cookie=1;job.alias=0;job.generation=bridge.generation(0);job.size=3;
 assert(bridge.submit(job)); bridge.disconnect(0);
 assert(bridge.tx.pop(job)); assert(!bridge.current(job));
 assert(!bridge.submit(job)); // Stale queued dispatch cannot be sent after reconnect.
 job.generation=bridge.generation(0);
 for(unsigned i=0;i<QueueDepth;++i)assert(bridge.submit(job));
 assert(!bridge.submit(job));
 for(unsigned i=0;i<QueueDepth;++i)assert(bridge.tx.pop(job));
 // A bounded SPSC transfer across real host threads, preserving opaque bytes.
 std::thread producer([&]{for(uint32_t i=1;i<=10000;++i){Transmission p;p.cookie=i;p.size=1;p.bytes[0]=uint8_t(i);while(!bridge.tx.push(p))std::this_thread::yield();}});
 for(uint32_t i=1;i<=10000;++i){while(!bridge.tx.pop(job))std::this_thread::yield();assert(job.cookie==i&&job.bytes[0]==uint8_t(i));}
 producer.join();
}
