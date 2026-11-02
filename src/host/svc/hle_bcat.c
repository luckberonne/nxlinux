// nn::bcat (background content acquisition: the delivery cache of news and similar content): the host has no bcat service process and acquires nothing,
// so the delivery cache is always empty. Mounting it succeeds (on a device the storage is there even when empty) and every directory listing is empty;
// opening a directory or a file fails, and a sync request fails since there is no network.
#include "hle.h"

HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "the delivery cache storage is always there, and empty (cut)", Result, h_bcat_Mount, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "the delivery cache storage is never really mounted (cut)", Result, h_bcat_Unmount, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", "no background content is ever acquired (cut)",
          Result, h_bcat_Enumerate, int *count, void *dirs, int n) { (void)dirs; (void)n; if (count) *count = 0; return RES_OK; }

// Directories / files
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", "the delivery cache is empty (cut)", Result, h_bcat_DirOpen, void *self, const void *name) {
    (void)self; (void)name; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", "the delivery cache is empty (cut)",
          Result, h_bcat_DirRead, void *self, int *count, void *entries, int n) { (void)self; (void)entries; (void)n; if (count) *count = 0; return RES_OK; }
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv", "no directory is open (cut)", Result, h_bcat_DirClose, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", "the delivery cache is empty (cut)", Result, h_bcat_FileOpen, void *self, const void *dir, const void *name) {
    (void)self; (void)dir; (void)name; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", "no file is open (cut)", Result, h_bcat_FileRead, void *self, uint64_t *read, size_t off, void *buf, size_t n) {
    (void)self; (void)off; (void)buf; (void)n; if (read) *read = 0; return RES_OK;   // an open file is at its end: zero bytes read
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile5CloseEv", "no file is open (cut)", Result, h_bcat_FileClose, void *self) { (void)self; return RES_OK; }

// Progress of a sync that never starts
HLE_FORCE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressEPKc", "no network to acquire background content from (cut)",
          Result, h_bcat_Sync, void *progress, const char *name) { (void)progress; (void)name; return RES_FAIL; }
HLE_FORCE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", "no network to acquire background content from (cut)",
          Result, h_bcat_SyncAll, void *progress) { (void)progress; return RES_FAIL; }
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", "no sync ever runs (cut)", int, h_bcat_ProgressUpdate, void *self) { (void)self; return 0; }
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv", "no sync ever runs (cut)", int, h_bcat_ProgressStatus, const void *self) { (void)self; return 0; }
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv", "no sync ever runs (cut)", Result, h_bcat_ProgressResult, const void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress18GetWholeDownloadedEv", "no sync ever runs (cut)", uint64_t, h_bcat_ProgressDone, const void *self) { (void)self; return 0; }
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress13GetWholeTotalEv", "no sync ever runs (cut)", uint64_t, h_bcat_ProgressTotal, const void *self) { (void)self; return 0; }
