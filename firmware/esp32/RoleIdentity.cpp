// SPDX-License-Identifier: Apache-2.0
#include "RoleIdentity.h"
#include <Arduino.h>
#include <esp_system.h>
#include <nvs.h>

namespace onchip {
namespace {
constexpr uint8_t MAGIC[] = {'M', 'C', 'I', 1};
constexpr uint8_t STAGED = 1, RESET = 2;
constexpr const char *IDENTITY_NAMES[] = {
    "modem", "repeater", "room", "companion", "observer", "management", "command-bot"};
struct Record {
  uint8_t magic[4];
  uint8_t flags;
  uint8_t active[PRV_KEY_SIZE];
  uint8_t pending[PRV_KEY_SIZE];
};
static_assert(sizeof(Record) == 5 + 2 * PRV_KEY_SIZE,
              "Identity record layout changed");

void wipe(void *memory, size_t size) {
  auto p = static_cast<volatile uint8_t *>(memory);
  while (size--)
    *p++ = 0;
}
void fresh(mesh::LocalIdentity &identity) {
  HardwareRNG rng;
  do {
    identity = mesh::LocalIdentity(&rng);
  } while (identity.pub_key[0] == 0 || identity.pub_key[0] == 255);
}
bool validName(const char *name) {
  if (name)
    for (const char *known : IDENTITY_NAMES)
      if (!strcmp(name, known)) return true;
  return false;
}
bool error(const char *name, const char *operation, esp_err_t result) {
  Serial.printf("On-chip %s identity %s failed: %s\n", name ? name : "invalid",
                operation, esp_err_to_name(result));
  return false;
}
bool read(const char *name, Record &record, bool &found) {
  if (!validName(name))
    return error(name, "name", ESP_ERR_INVALID_ARG);
  record = {};
  memcpy(record.magic, MAGIC, sizeof(MAGIC));
  found = false;
  nvs_handle_t handle;
  esp_err_t result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND)
    return true;
  if (result != ESP_OK)
    return error(name, "open", result);
  size_t size = 0;
  result = nvs_get_blob(handle, name, nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return true;
  }
  if (result == ESP_OK) {
    if (size == PRV_KEY_SIZE)
      result = nvs_get_blob(handle, name, record.active, &size);
    else if (size == sizeof(record))
      result = nvs_get_blob(handle, name, &record, &size);
    else
      result = ESP_ERR_INVALID_STATE;
  }
  nvs_close(handle);
  if (result != ESP_OK)
    return error(name, "read", result);
  if (memcmp(record.magic, MAGIC, sizeof(MAGIC)) ||
      (size == sizeof(record) && !(record.flags & STAGED)) ||
      record.flags & ~(STAGED | RESET) ||
      ((record.flags & RESET) && !(record.flags & STAGED)) ||
      !mesh::LocalIdentity::validatePrivateKey(record.active) ||
      ((record.flags & STAGED) &&
       !mesh::LocalIdentity::validatePrivateKey(record.pending)))
    return error(name, "validation", ESP_ERR_INVALID_STATE);
  found = true;
  return true;
}
bool identityConflict(const char *name, const uint8_t publicKey[32], bool &conflict) {
  conflict = false;
  for (const char *other : IDENTITY_NAMES) {
    if (!strcmp(name, other)) continue;
    Record record{};
    bool found = false;
    const bool ok = read(other, record, found);
    if (ok && found) {
      mesh::LocalIdentity identity;
      identity.readFrom(record.active, PRV_KEY_SIZE);
      conflict = !memcmp(identity.pub_key, publicKey, PUB_KEY_SIZE);
      if (!conflict && (record.flags & STAGED)) {
        identity.readFrom(record.pending, PRV_KEY_SIZE);
        conflict = !memcmp(identity.pub_key, publicKey, PUB_KEY_SIZE);
      }
      wipe(&identity, sizeof(identity));
    }
    wipe(&record, sizeof(record));
    if (!ok || conflict) return ok;
  }
  return true;
}
bool write(const char *name, const Record &record) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK)
    return error(name, "open", result);
  result = record.flags
               ? nvs_set_blob(handle, name, &record, sizeof(record))
               : nvs_set_blob(handle, name, record.active, PRV_KEY_SIZE);
  if (result == ESP_OK)
    result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK || error(name, "commit", result);
}
bool activate(const char *name, Record &record) {
  memcpy(record.active, record.pending, PRV_KEY_SIZE);
  wipe(record.pending, PRV_KEY_SIZE);
  record.flags = 0;
  return write(name, record);
}
bool commitVerified(const char *name, const Record &record) {
  Record actual{};
  bool found = false;
  const bool ok = write(name, record) && read(name, actual, found) && found &&
                  !memcmp(&record, &actual, sizeof(record));
  wipe(&actual, sizeof(actual));
  return ok || error(name, "commit/readback; outcome unknown", ESP_ERR_INVALID_STATE);
}
} // namespace

void HardwareRNG::random(uint8_t *data, size_t size) {
  esp_fill_random(data, size);
}

bool loadIdentity(const char *name, mesh::LocalIdentity &identity) {
  Record record{};
  bool found;
  bool ok = read(name, record, found);
  if (ok && !found) {
    mesh::LocalIdentity generated;
    fresh(generated);
    generated.writeTo(record.active, PRV_KEY_SIZE);
    ok = write(name, record);
    wipe(&generated, sizeof(generated));
  }
  if (ok && (record.flags & RESET))
    ok = error(name, "reset not finished", ESP_ERR_INVALID_STATE);
  if (ok && (record.flags & STAGED))
    ok = activate(name, record);
  if (ok)
    identity.readFrom(record.active, PRV_KEY_SIZE);
  wipe(&record, sizeof(record));
  return ok;
}

bool stageIdentity(Role role, const mesh::LocalIdentity &identity) {
  const char *name = roleName(role);
  Record record{};
  bool found;
  bool ok = read(name, record, found);
  if (ok && (!found || record.flags & RESET))
    ok = error(name, "stage state", ESP_ERR_INVALID_STATE);
  if (ok) {
    mesh::LocalIdentity copy = identity;
    copy.writeTo(record.pending, PRV_KEY_SIZE);
    wipe(&copy, sizeof(copy));
    if (!mesh::LocalIdentity::validatePrivateKey(record.pending))
      ok = error(name, "stage validation", ESP_ERR_INVALID_ARG);
    else {
      record.flags = STAGED;
      ok = write(name, record);
    }
  }
  wipe(&record, sizeof(record));
  return ok;
}
bool identityPublicKey(const char *name, uint8_t publicKey[32], bool pending) {
  Record record{};
  bool found = false;
  bool ok = read(name, record, found);
  if (ok && (!found || (pending && !(record.flags & STAGED))))
    ok = error(name, "identity/pending key absent", ESP_ERR_INVALID_STATE);
  if (ok) {
    mesh::LocalIdentity identity;
    identity.readFrom(pending ? record.pending : record.active, PRV_KEY_SIZE);
    memcpy(publicKey, identity.pub_key, PUB_KEY_SIZE);
    wipe(&identity, sizeof(identity));
  }
  wipe(&record, sizeof(record));
  return ok;
}
bool rotateIdentity(const char *name, uint8_t pendingPublicKey[32]) {
  Record record{}, actual{};
  bool found = false;
  bool ok = read(name, record, found);
  if (ok && (!found || record.flags))
    ok = error(name, "rotation requires initialized identity without pending change", ESP_ERR_INVALID_STATE);
  mesh::LocalIdentity replacement;
  if (ok) {
    fresh(replacement);
    replacement.writeTo(record.pending, PRV_KEY_SIZE);
    record.flags = STAGED;
    ok = write(name, record);
    if (ok) {
      ok = read(name, actual, found) && found && !memcmp(&record, &actual, sizeof(record));
      if (!ok) error(name, "rotation readback; outcome unknown", ESP_ERR_INVALID_STATE);
    }
    if (ok) memcpy(pendingPublicKey, replacement.pub_key, PUB_KEY_SIZE);
  }
  wipe(&replacement, sizeof(replacement));
  wipe(&record, sizeof(record)); wipe(&actual, sizeof(actual));
  return ok;
}

IdentityChange importIdentity(const char *name, const uint8_t privateKey[PRV_KEY_SIZE],
                              uint8_t publicKey[32]) {
  if (!mesh::LocalIdentity::validatePrivateKey(privateKey)) {
    error(name, "import validation", ESP_ERR_INVALID_ARG);
    return IdentityChange::Rejected;
  }
  Record record{};
  bool found = false;
  IdentityChange result = IdentityChange::Rejected;
  if (read(name, record, found) && found) {
    if ((record.flags & RESET) ||
        ((record.flags & STAGED) && memcmp(record.pending, privateKey, PRV_KEY_SIZE))) {
      error(name, "different pending identity or reset; import refused", ESP_ERR_INVALID_STATE);
      result = IdentityChange::Conflict;
    } else {
      mesh::LocalIdentity identity;
      identity.readFrom(privateKey, PRV_KEY_SIZE);
      bool conflict = false;
      if (!identityConflict(name, identity.pub_key, conflict)) {
        error(name, "import cannot verify other role identities", ESP_ERR_INVALID_STATE);
      } else if (conflict) {
        error(name, "import duplicates another active/pending role identity", ESP_ERR_INVALID_STATE);
        result = IdentityChange::Duplicate;
      } else if (record.flags & STAGED) {
        result = IdentityChange::Staged;
      } else if (!memcmp(record.active, privateKey, PRV_KEY_SIZE)) {
        result = IdentityChange::Active;
      } else {
        memcpy(record.pending, privateKey, PRV_KEY_SIZE);
        record.flags = STAGED;
        result = commitVerified(name, record) ? IdentityChange::Staged : IdentityChange::Unknown;
      }
      wipe(&identity, sizeof(identity));
    }
  } else {
    error(name, "import requires a readable initialized identity", ESP_ERR_INVALID_STATE);
  }
  if (result == IdentityChange::Active || result == IdentityChange::Staged) {
    mesh::LocalIdentity identity;
    identity.readFrom(privateKey, PRV_KEY_SIZE);
    memcpy(publicKey, identity.pub_key, PUB_KEY_SIZE);
    wipe(&identity, sizeof(identity));
  }
  wipe(&record, sizeof(record));
  return result;
}
IdentityChange cancelIdentityChange(const char *name, uint8_t publicKey[32]) {
  Record record{};
  bool found = false;
  IdentityChange result = IdentityChange::Rejected;
  if (read(name, record, found) && found) {
    if (record.flags & RESET) {
      error(name, "role reset in progress; cancellation refused", ESP_ERR_INVALID_STATE);
      result = IdentityChange::Conflict;
    } else {
      const bool staged = record.flags & STAGED;
      record.flags = 0;
      wipe(record.pending, sizeof(record.pending));
      result = (!staged || commitVerified(name, record)) ? IdentityChange::Active : IdentityChange::Unknown;
      if (result == IdentityChange::Active) {
        mesh::LocalIdentity identity;
        identity.readFrom(record.active, PRV_KEY_SIZE);
        memcpy(publicKey, identity.pub_key, PUB_KEY_SIZE);
        wipe(&identity, sizeof(identity));
      }
    }
  } else {
    error(name, "cancel requires a readable initialized identity", ESP_ERR_INVALID_STATE);
  }
  wipe(&record, sizeof(record));
  return result;
}

bool resetPending(Role role, bool &pending) {
  Record record{};
  bool found;
  const bool ok = read(roleName(role), record, found);
  if (ok)
    pending = found && (record.flags & RESET);
  wipe(&record, sizeof(record));
  return ok;
}

bool prepareIdentityReset(Role role) {
  Record record{};
  bool found;
  const char *name = roleName(role);
  bool ok = read(name, record, found);
  if (ok && !found)
    ok = error(name, "reset state", ESP_ERR_INVALID_STATE);
  if (ok && !(record.flags & RESET)) {
    mesh::LocalIdentity replacement;
    fresh(replacement);
    replacement.writeTo(record.pending, PRV_KEY_SIZE);
    wipe(&replacement, sizeof(replacement));
    record.flags = STAGED | RESET;
    ok = write(name, record);
  }
  wipe(&record, sizeof(record));
  return ok;
}

bool finishIdentityReset(Role role) {
  Record record{};
  bool found;
  const char *name = roleName(role);
  bool ok = read(name, record, found);
  if (ok && (!found || !(record.flags & RESET)))
    ok = error(name, "reset completion", ESP_ERR_INVALID_STATE);
  if (ok)
    ok = activate(name, record);
  wipe(&record, sizeof(record));
  return ok;
}
} // namespace onchip
