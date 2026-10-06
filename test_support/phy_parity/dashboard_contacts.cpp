// Exercise the generated bridge with the pinned native contact iterator API.
#include "RadioDashboard.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

struct ContactInfo {
  struct { uint8_t pub_key[32]{}; } id;
  char name[32]{};
  uint8_t type = 0;
  uint8_t secret[32]{};
};
struct NativeContacts {
  std::vector<ContactInfo> contacts;
  unsigned visited = 0;
  int getNumContacts() const { return contacts.size(); }
  struct Iterator {
    size_t at = 0;
    bool hasNext(NativeContacts *mesh, ContactInfo &contact) {
      if (at == mesh->contacts.size()) return false;
      contact = mesh->contacts[at++];
      ++mesh->visited;
      return true;
    }
  };
  Iterator startContactsIterator() { return {}; }
};
namespace onchip {
enum class Role { Companion };
static NativeContacts *meshInstance = nullptr;
static bool busy = false;
bool lifecycleBusy(Role) { return busy; }
}
#include "dashboard-contacts-bridge.inc"

int main() {
  using namespace onchip;
  RadioDashboard::RadioStatus status;
  companionDashboardContacts(status);
  assert(!status.contacts_available);
  NativeContacts mesh;
  meshInstance = &mesh;
  companionDashboardContacts(status);
  assert(status.contacts_available && !status.contact_total && !status.contact_count);
  ContactInfo contact;
  memset(contact.id.pub_key, 0xab, sizeof(contact.id.pub_key));
  strcpy(contact.name, "D1\"\\\n<script>");
  contact.type = 2;
  memset(contact.secret, 0xee, sizeof(contact.secret));
  mesh.contacts.push_back(contact);
  companionDashboardContacts(status);
  assert(status.contact_total == 1 && status.contact_count == 1);
  assert(!strcmp(status.contacts[0].name, contact.name) && status.contacts[0].type == 2);
  RadioDashboard dashboard;
  assert(dashboard.publish(500, status));
  RadioDashboard::Snapshot snapshot;
  assert(dashboard.snapshot(snapshot));
  mesh.contacts[0].name[0] = 'X';
  char json[RadioDashboard::JSON_CAPACITY];
  assert(RadioDashboard::formatJSON(snapshot, "Test", json, sizeof(json)));
  assert(strstr(json, "\"contacts\":{\"capacity\":32,\"total\":1,\"truncated\":false"));
  assert(strstr(json, "\"name\":\"D1\\\"\\\\\\u000a<script>\""));
  assert(strstr(json, "\"type\":2"));
  assert(!strstr(json, "eeeeeeee") && !strstr(json, "secret") && !strstr(json, "channel"));
  busy = true;
  companionDashboardContacts(status);
  assert(!status.contacts_available && !status.contact_count);
  assert(dashboard.publish(1000, status) && dashboard.snapshot(snapshot));
  assert(RadioDashboard::formatJSON(snapshot, "Test", json, sizeof(json)));
  assert(!strstr(json, "\"contacts\""));
  busy = false;
  mesh.contacts.assign(256, contact);
  memset(mesh.contacts[0].name, 'n', sizeof(contact.name));
  mesh.visited = 0;
  companionDashboardContacts(status);
  assert(status.contact_total == 256 && status.contact_count == 32 && mesh.visited == 32);
  assert(strlen(status.contacts[0].name) == 31);
  assert(dashboard.publish(1500, status) && dashboard.snapshot(snapshot));
  assert(RadioDashboard::formatJSON(snapshot, "Test", json, sizeof(json)));
  assert(strstr(json, "\"total\":256,\"truncated\":true"));
  snapshot.event_count = RadioDashboard::HISTORY;
  snapshot.event_next = 0;
  snapshot.radio.role_count = RadioDashboard::ROLE_CAPACITY;
  snapshot.uptime_ms = UINT32_MAX;
  snapshot.publication = snapshot.events_total = snapshot.events_overwritten =
      snapshot.rx_packets = snapshot.rx_estimated_ms = snapshot.tx_accepted =
      snapshot.tx_rejected = snapshot.tx_succeeded = snapshot.tx_failed =
      snapshot.tx_unknown = snapshot.tx_rf_ms = UINT64_MAX;
  for (auto &entry : snapshot.radio.contacts)
    memset(entry.name, '\n', sizeof(entry.name) - 1);
  for (auto &role : snapshot.radio.roles) {
    memset(role.name, '\n', sizeof(role.name) - 1);
    memset(role.fault, '\n', sizeof(role.fault) - 1);
    role.profile_generation = UINT64_MAX;
  }
  for (auto &client : snapshot.radio.client) {
    client.connected = client.negotiated = true;
    client.generation = client.credit_ms = client.rf_ms = UINT32_MAX;
    client.factor = std::numeric_limits<float>::max();
  }
  for (auto &event : snapshot.events) {
    event.sequence = event.at_ms = UINT64_MAX;
    event.length = 255;
    event.preview_length = RadioDashboard::PREVIEW_BYTES;
    event.source_generation = event.job_id = event.queue_ms = event.rf_ms = event.estimated_ms = UINT32_MAX;
  }
  for (uint64_t second = snapshot.uptime_ms / 1000 - 59; second <= snapshot.uptime_ms / 1000; ++second) {
    auto &bucket = snapshot.traffic[second % RadioDashboard::TRAFFIC_SECONDS];
    bucket.second = second;
    bucket.rx_packets = bucket.tx_packets = bucket.tx_rf_ms = bucket.rx_estimated_ms = UINT32_MAX;
  }
  const size_t size = RadioDashboard::formatJSON(snapshot, "Test", json, sizeof(json));
  assert(size && size < sizeof(json));
  char small[10];
  assert(!RadioDashboard::formatJSON(snapshot, "Test", small, sizeof(small)) && !small[0]);
  printf("Dashboard native contact bridge: availability, public-only escaping, 32/256 bounds and worst-size JSON %zu/%zu passed\n", size, sizeof(json));
}
