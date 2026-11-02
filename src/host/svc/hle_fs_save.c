// nn::fs save data entry points the host answers itself. EnsureSaveData(uid) creates the user's save data if it is missing: here the save directory is made when it is opened
// (ipc_fs.c, fsfs_open_savedata), so it is always there. The SDK's own version (SDK 8.2.1, Super Smash Bros. Ultimate) calls a service object that is not set up in the host and jumps
// through a null function pointer; nxnative answers it the same way.
#include "hle.h"

HLE_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "save data always exists: the host creates its directory when it is opened", Result, h_fs_EnsureSave, const void *uid) { (void)uid; return RES_OK; }

// GetSaveDataSize(&normal, &journal, uid): the size of the user's save data. The SDK's version asks the same missing service object (a null call in Super Smash Bros. Ultimate's start); the answer is
// a save as big as the title's NACP declares for it (96 MB and 10 MB there, the most a title that checks "is it big enough" can want of this host, which keeps its save in a directory).
HLE_FORCE("_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", "the save data is as big as a title can ask: it lives in a host directory", Result, h_fs_SaveSize, int64_t *normal, int64_t *journal, const void *uid) {
    (void)uid; if (normal) *normal = 0x6000000; if (journal) *journal = 0xa00000; return RES_OK;
}
