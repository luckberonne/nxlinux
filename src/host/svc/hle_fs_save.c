// nn::fs save data entry points the host answers itself. EnsureSaveData(uid) creates the user's save data if it is missing: here the save directory is made when it is opened
// (ipc_fs.c, fsfs_open_savedata), so it is always there. The SDK's own version (SDK 8.2.1, Super Smash Bros. Ultimate) calls a service object that is not set up in the host and jumps
// through a null function pointer; nxnative answers it the same way.
#include "hle.h"

HLE_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "save data always exists: the host creates its directory when it is opened", Result, h_fs_EnsureSave, const void *uid) { (void)uid; return RES_OK; }
