// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomSocket.h"
#if (defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM) || defined(CLOUD_ROOM_SOCKET_TEST)
#include "BotHttps.h"
#ifndef CLOUD_ROOM_SOCKET_TEST
#include <WiFi.h> // Before lwIP headers defining INADDR_NONE.
#endif
#include <esp_transport.h>
#include <esp_transport_ws.h>
#include <esp_log.h>
#ifndef CLOUD_ROOM_SOCKET_TEST
#include <esp_idf_version.h>
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(4,4,7)
#error "Recheck the cloud-room transport foundation adapter before changing ESP-IDF"
#endif
#endif
#include "cloudroom/third_party/esp-idf/esp_transport_internal.h"
#include <new>
#include <string.h>
#include <strings.h>
#include <stdio.h>
namespace onchip {
namespace {
constexpr size_t FrameLimit=4096;
constexpr int IoTimeoutMs=15000;
bool ascii(const char *s,size_t max,bool alias=false) {
  if(!s||!*s||strnlen(s,max+1)>max)return false;
  for(;*s;++s) {
    if(alias) {if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return false;}
    else if(static_cast<unsigned char>(*s)<33||static_cast<unsigned char>(*s)>126)return false;
  }
  return true;
}
class EspWss final:public CloudRoomSocket {
  BotHttpsTransport *tls_=nullptr;
  esp_transport_handle_t parent_=nullptr,ws_=nullptr;
  const CloudRoomPeer *peer_=nullptr;
  char address_[16]{};
  char error_[96]{};
  int peek_=-1;
  size_t upgradeBytes_=0;
  uint32_t upgradeStarted_=0;
  char upgradeLine_[128]{};
  size_t upgradeLineSize_=0;
  bool firstUpgradeLine_=true,upgradeOverflow_=false,upgradeStatus_=false,upgradeProtocol_=false;
  bool upgrading_=false,headerStart_=false;
  void upgradeByte(char byte) {
    if(byte!='\n') {
      if(upgradeLineSize_+1<sizeof(upgradeLine_))upgradeLine_[upgradeLineSize_++]=byte;
      else upgradeOverflow_=true;
      return;
    }
    if(upgradeLineSize_&&upgradeLine_[upgradeLineSize_-1]=='\r')--upgradeLineSize_;
    upgradeLine_[upgradeLineSize_]=0;
    if(!upgradeOverflow_) {
      if(firstUpgradeLine_)
        upgradeStatus_=strncmp(upgradeLine_,"HTTP/1.1 101",12)==0 && (upgradeLine_[12]==0||upgradeLine_[12]==' ');
      else if(strncasecmp(upgradeLine_,"Sec-WebSocket-Protocol:",23)==0) {
        char *value=upgradeLine_+23;
        while(*value==' '||*value=='\t')++value;
        size_t len=strlen(value);
        while(len&&(value[len-1]==' '||value[len-1]=='\t'))value[--len]=0;
        upgradeProtocol_=strcmp(value,"aspen-room.v2.json")==0;
      }
    }
    firstUpgradeLine_=false;upgradeLineSize_=0;upgradeOverflow_=false;
  }
  bool timeout(uint32_t start,int ms) const {return uint32_t(tls_->now()-start)>=uint32_t(ms<0?IoTimeoutMs:ms);}
  static EspWss &self(esp_transport_handle_t h) {return *static_cast<EspWss *>(esp_transport_get_context_data(h));}
  static int connect(esp_transport_handle_t h,const char *,int,int) {
    auto &s=self(h);
    const BotHttpsConfig cfg(s.address_,s.peer_->host,s.peer_->ca,"",443,0);
    return s.tls_->open(cfg,s.error_,sizeof(s.error_))?0:-1;
  }
  int read(char *out,int size,int ms) {
    if(size<=0)return 0;
    // IDF4.4 writes its terminating NUL at header_len. Keep it below the
    // configured buffer size, and preserve a coalesced ready frame.
    if(upgrading_ && upgradeBytes_ >= CONFIG_WS_BUFFER_SIZE-1)return -1;
    const int want=upgrading_?1:size;
    const auto start=tls_->now();int have=0;
    if(peek_>=0){out[have++]=char(peek_);peek_=-1;}
    while(have<want) {
      if(upgrading_&&timeout(upgradeStarted_,IoTimeoutMs))return -1;
      if(!tls_->validate(error_,sizeof(error_)))return -1;
      const int n=tls_->read(reinterpret_cast<uint8_t *>(out+have),size_t(want-have));
      if(n<0)return -1;
      have+=n;
      if(have==want)break;
      // SDK frame-header reads require the requested byte count; a partial
      // timeout is a broken stream, never an apparently complete header.
      if(timeout(start,ms))return have?-1:0;
      tls_->idle();
    }
    if(upgrading_) {upgradeBytes_+=size_t(have);upgradeByte(out[0]);}
    if(!upgrading_&&headerStart_) {
      headerStart_=false;
      // Worker sends one bounded text frame. The SDK4.4 API omits FIN, so
      // inspect its initial header here and reject fragments/extensions/masks.
      if(want!=2||(uint8_t(out[0])&0xf0)!=0x80||(uint8_t(out[1])&0x80))return -1;
    }
    return have;
  }
  static int readCallback(esp_transport_handle_t h,char *out,int n,int ms){return self(h).read(out,n,ms);}
  static int writeCallback(esp_transport_handle_t h,const char *data,int n,int ms) {
    auto &s=self(h);const auto start=s.tls_->now();int sent=0;
    while(sent<n) {
      if(s.upgrading_&&s.timeout(s.upgradeStarted_,IoTimeoutMs))return -1;
      if(!s.tls_->validate(s.error_,sizeof(s.error_)))return -1;
      const int count=s.tls_->write(reinterpret_cast<const uint8_t *>(data+sent),size_t(n-sent));
      if(count<0)return -1;
      sent+=count;if(sent==n)break;
      if(s.timeout(start,ms))return -1;
      s.tls_->idle();
    }
    return sent;
  }
  static int pollRead(esp_transport_handle_t h,int ms) {
    auto &s=self(h);if(s.peek_>=0)return 1;
    const auto start=s.tls_->now();uint8_t byte=0;
    do {
      if(!s.tls_->validate(s.error_,sizeof(s.error_)))return -1;
      const int n=s.tls_->read(&byte,1);if(n<0)return -1;if(n>0){s.peek_=byte;return 1;}
      if(s.timeout(start,ms))return 0;
      s.tls_->idle();
    }while(true);
  }
  static int pollWrite(esp_transport_handle_t,int){return 1;}
  static int closeCallback(esp_transport_handle_t h){self(h).tls_->close();return 0;}
  static int destroyCallback(esp_transport_handle_t h) {
    esp_transport_destroy_foundation_transport(h->foundation);
    h->foundation=nullptr;return 0;
  }
public:
  ~EspWss() override {close();delete tls_;}
  bool open(const CloudRoomPeer &peer,char *error,size_t capacity) override {
    close();
    if(!ascii(peer.host,127)||!ascii(peer.token,256)||!ascii(peer.alias,64,true)||!peer.ca||strnlen(peer.ca,4097)>4096) {
      snprintf(error,capacity,"Invalid cloud-room peer settings");return false;
    }
    if(peer.address&&*peer.address) {
      if(!ascii(peer.address,15)){snprintf(error,capacity,"Invalid cloud-room IPv4 address");return false;}
      strcpy(address_,peer.address);
    }else {
#ifndef CLOUD_ROOM_SOCKET_TEST
      IPAddress ip;
      if(WiFi.hostByName(peer.host,ip)!=1){snprintf(error,capacity,"Cloud-room DNS lookup failed");return false;}
      snprintf(address_,sizeof(address_),"%u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]);
#else
      snprintf(error,capacity,"Host test requires fixed peer address");return false;
#endif
    }
    if(!tls_)tls_=createPersistentBotTlsTransport();
    if(!tls_){snprintf(error,capacity,"Cloud-room TLS transport unavailable");return false;}
    peer_=&peer;
    parent_=esp_transport_init();
    if(parent_) {
      // SDK WS requires a foundation/error container. This is the sole pinned
      // internal-header dependency; framing still uses the unmodified SDK.
      parent_->foundation=esp_transport_init_foundation_transport();
      esp_transport_set_context_data(parent_,this);
      esp_transport_set_func(parent_,connect,readCallback,writeCallback,closeCallback,pollRead,pollWrite,destroyCallback);
      if(parent_->foundation)ws_=esp_transport_ws_init(parent_);
    }
    if(!ws_){close();snprintf(error,capacity,"Cloud-room WSS allocation failed");return false;}
    char path[96];snprintf(path,sizeof(path),"/v1/aliases/%s/socket",peer.alias);
    char headers[288];snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",peer.token);
    // SDK transport debug/error paths can print the upgrade header. Suppress
    // this tag entirely; our diagnostics contain fixed text, never credentials.
    esp_log_level_set("TRANSPORT_WS",ESP_LOG_NONE);
    esp_transport_ws_config_t config{};config.ws_path=path;config.sub_protocol="aspen-room.v2.json";
    config.user_agent="AspenSharedRoom/0.1";config.headers=headers;config.propagate_control_frames=true;
    const auto configured=esp_transport_ws_set_config(ws_,&config);
    memset(headers,0,sizeof(headers));
    upgradeBytes_=upgradeLineSize_=0;upgradeOverflow_=upgradeStatus_=upgradeProtocol_=false;
    firstUpgradeLine_=true;upgrading_=true;
    upgradeStarted_=tls_->now();
    const int status=configured==ESP_OK?esp_transport_connect(ws_,peer.host,443,IoTimeoutMs):-1;
    upgrading_=false;peer_=nullptr;
    if(status<0||!upgradeStatus_||!upgradeProtocol_){close();snprintf(error,capacity,"Cloud-room WSS connection/upgrade failed");return false;}
    return true;
  }
  bool send(char *json,size_t size) override {
    if(!ws_||!json||!size||size>FrameLimit)return false;
    // SDK masks in place, restores afterwards; use a caller-owned mutable
    // outgoing buffer through this API, never a string literal/flash pointer.
    const int n=esp_transport_ws_send_raw(ws_,ws_transport_opcodes_t(WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN),json,int(size),IoTimeoutMs);
    if(n!=int(size)){close();return false;}return true;
  }
  int receive(char *frame,size_t capacity) override {
    if(!ws_||!frame||capacity<FrameLimit)return -1;
    const int ready=esp_transport_poll_read(ws_,0);
    if(ready<0){close();return -1;}
    if(!ready)return 0;
    headerStart_=true;
    const int n=esp_transport_read(ws_,frame,int(FrameLimit),IoTimeoutMs);
    const int size=esp_transport_ws_get_read_payload_len(ws_);
    const auto op=esp_transport_ws_get_read_opcode(ws_);
    if(n<0||size<0||size>int(FrameLimit)){close();return -1;}
    if(op==WS_TRANSPORT_OPCODES_PING||op==WS_TRANSPORT_OPCODES_PONG||op==WS_TRANSPORT_OPCODES_CLOSE) {
      if(size>125||n!=size){close();return -1;}
      if(op==WS_TRANSPORT_OPCODES_PING) {
        if(esp_transport_ws_send_raw(ws_,ws_transport_opcodes_t(WS_TRANSPORT_OPCODES_PONG|WS_TRANSPORT_OPCODES_FIN),frame,n,IoTimeoutMs)!=n){close();return -1;}
      }else if(op==WS_TRANSPORT_OPCODES_CLOSE){close();return -1;}
      return 0;
    }
    if(op!=WS_TRANSPORT_OPCODES_TEXT||!size||n!=size){close();return -1;}
    return n;
  }
  void close() override {
    if(ws_){esp_transport_destroy(ws_);ws_=nullptr;}
    if(parent_){esp_transport_destroy(parent_);parent_=nullptr;}
    if(tls_)tls_->close();
    peek_=-1;peer_=nullptr;upgrading_=headerStart_=false;upgradeBytes_=0;
  }
};
}
CloudRoomSocket *createCloudRoomSocket(){return new(std::nothrow) EspWss;}
} // namespace onchip
#else
namespace onchip {CloudRoomSocket *createCloudRoomSocket(){return nullptr;}}
#endif
