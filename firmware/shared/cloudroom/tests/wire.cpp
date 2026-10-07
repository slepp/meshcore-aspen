// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomWire.h"
#include <assert.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <new>
#include <cstdlib>
#include <cstring>

static bool parsing = false;
static size_t allocations = 0;
void *operator new(size_t n) { if (parsing) ++allocations; if (void *p = std::malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
using namespace cloudroom;
static void checks() {
 Document doc;
 assert(!doc.parse("[]",2)); assert(!doc.parse("{\"x\":}",6));
 const char *text = "{\"x\":\"hi\\n\\u00e9\\ud83d\\ude00\",\"n\":4294967295,\"b\":\"AAH+/w==\"}";
 assert(doc.parse(text,strlen(text)));
 char out[32]; size_t n = 0;
 assert(decodeString(Document::field(doc.root(),"x"),out,sizeof(out),n));
 assert(std::string(out,n) == "hi\n\xc3\xa9\xf0\x9f\x98\x80");
 uint64_t integer = 0;
 assert(Document::field(doc.root(),"n").unsignedNumber(integer)); assert(integer == UINT32_MAX);
 uint8_t bytes[4];
 assert(decodeBytes(Document::field(doc.root(),"b"),bytes,sizeof(bytes),n));
 assert(n == 4 && bytes[0] == 0 && bytes[1] == 1 && bytes[2] == 254 && bytes[3] == 255);
 assert(!decodeBytes({"AB==",4,JSONString},bytes,sizeof(bytes),n)); // Noncanonical unused bits.
 assert(!decodeBytes({"AA==AAAA",8,JSONString},bytes,sizeof(bytes),n));
 assert(!decodeBytes({"AAAA",4,JSONString},bytes,2,n));
 assert(!decodeString({"\\ud800",6,JSONString},out,sizeof(out),n));
 assert(!decodeString({"abc",3,JSONString},out,3,n));
 Event event;
 assert(!event.parse("{\"type\":\"ready\",\"version\":2}",28));
 std::string large(FrameLimit+1,' '); assert(!event.parse(large.data(),large.size()));
}
int main(int argc, char **argv) {
 checks();
 if (argc != 2) return 1;
 std::cout << "host View=" << sizeof(View) << " Event=" << sizeof(Event) << " Document=" << sizeof(Document) << " bytes\n";
 std::cout << "frame,bytes,host ns/op,parser allocations,decoded bytes copied\n";
 for (const char *name : {"login", "post", "history-request", "history-delivery", "notification", "ack", "login-result", "ack-result", "ready", "maximum-delivery"}) {
  std::ifstream file(std::string(argv[1])+"/"+name+".json");
  std::string data{std::istreambuf_iterator<char>(file),{}};
  assert(!data.empty());
  size_t copied = 0; Event event; Document document;
  auto start = std::chrono::steady_clock::now();
  parsing = true;
  for (unsigned i=0;i<20000;++i) {
   if (data.find("\"operation\"") != std::string::npos) {
    assert(document.parse(data.data(),data.size()));
    auto operation = Document::field(document.root(),"operation");
    auto op = Document::field(operation,"op"); assert(op.type == JSONString);
    assert(Document::field(operation,"client").hex(32));
    uint64_t stamp = 0;
    auto timestamp = Document::field(operation,"timestamp");
    if (timestamp.type != JSONInvalid) assert(timestamp.unsignedNumber(stamp));
   } else {
    assert(event.parse(data.data(),data.size()));
    if (event.type == Event::Delivery) {
     char text[152]; uint8_t route[RadioLimit]; size_t textSize = 0, routeSize = 0;
     assert(decodeString(Document::field(event.body,"text"),text,sizeof(text),textSize));
     assert(decodeBytes(event.route,route,sizeof(route),routeSize));
     copied = textSize + routeSize;
    }
   }
  }
  parsing = false;
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()/20000;
  assert(allocations == 0);
  std::cout << name << ',' << data.size() << ',' << ns << ',' << allocations << ',' << copied << '\n';
 }
}
