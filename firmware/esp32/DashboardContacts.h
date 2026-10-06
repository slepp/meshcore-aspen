// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RadioDashboard.h"
#include <cstring>

#if defined(MESHCORE_ONCHIP) && defined(ESP32) && !defined(NRF52_PLATFORM)
namespace onchip {
// Called only by the radio dispatch task, which also owns the native contacts.
template <class Contact, class Mesh>
void copyDashboardContacts(Mesh &mesh, RadioDashboard::RadioStatus &status) {
  status.contacts_available = false;
  status.contact_count = status.contact_total = 0;
  const int total = mesh.getNumContacts();
  if (total < 0 || total > UINT16_MAX) return;
  auto iterator = mesh.startContactsIterator();
  Contact contact{};
  while (status.contact_count < RadioDashboard::CONTACT_CAPACITY &&
         iterator.hasNext(&mesh, contact)) {
    auto &entry = status.contacts[status.contact_count++];
    memcpy(entry.public_key, contact.id.pub_key, sizeof(entry.public_key));
    memcpy(entry.name, contact.name, sizeof(entry.name));
    entry.name[sizeof(entry.name) - 1] = 0;
    entry.type = contact.type;
  }
  if (status.contact_count > total) {
    status.contact_count = 0;
    return;
  }
  status.contact_total = total;
  status.contacts_available = true;
}
} // namespace onchip
#endif
