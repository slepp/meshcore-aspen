// SPDX-License-Identifier: Apache-2.0
#pragma once
#if MESHCORE_NODE_BACKUP
namespace onchip {
void beginEspNodeBackup();
bool backupWorkerReady();
void wakeBackupWorker();
}
#endif
