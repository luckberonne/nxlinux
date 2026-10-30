// fsp-srv's writable file systems: save data lives in a directory of the host (NX_SAVE_DIR, default work/save/<title id>), shared by every save data attribute a title opens. IFileSystem,
// IFile and IDirectory are IPC objects whose state is a path, a file descriptor and a directory stream. Paths in the requests are absolute from the file system root ("/save.dat").
#include "ipc.h"
#include "mem.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define R_NOT_IMPL 0xF201u
#define R_PATH_NOT_FOUND 0x202u        // fs ResultPathNotFound (2002-0001)
#define R_PATH_EXISTS 0x402u           // ResultPathAlreadyExists
#define R_IO 0x2ee202u                 // an I/O failure
#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))
#define OUT64(v) do { uint64_t _v = (v); OUT32(_v); OUT32(_v >> 32); } while (0)

typedef struct { char root[PATH_MAX]; } fs_t;
typedef struct { int fd; } file_t;
typedef struct { char path[PATH_MAX]; DIR *d; int filter; uint64_t count; } dir_t;

static void host_path(const fs_t *fs, const char *guest, char *out) {      // root + the guest path; ".." components never leave the root. Too long: "", which no call finds
    char clean[PATH_MAX]; size_t n = 0;
    for (const char *p = guest; *p; ) {
        while (*p == '/') p++;
        const char *e = p; while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') { while (n && clean[n - 1] != '/') n--; if (n) n--; }
        else if (len && !(len == 1 && p[0] == '.')) { if (len >= sizeof clean - 2 - n) { out[0] = 0; return; } clean[n++] = '/'; memcpy(clean + n, p, len); n += len; }
        p = e;
    }
    clean[n] = 0;
    if (strlen(fs->root) + n >= PATH_MAX) { out[0] = 0; return; }
    snprintf(out, PATH_MAX, "%s%s", fs->root, clean);
}
static const char *in_path(const ipc_in_t *in, unsigned i) { return i < in->nsend ? (const char *)(uintptr_t)in->send[i].addr : ""; }
static int errno_result(void) { return errno == ENOENT ? R_PATH_NOT_FOUND : errno == EEXIST ? R_PATH_EXISTS : R_IO; }
static int rm_entry(const char *p, const struct stat *st, int type, struct FTW *ftw) { (void)st; (void)ftw; return type == FTW_DP ? rmdir(p) : unlink(p); }
static int rm_tree(const char *p) { return nftw(p, rm_entry, 16, FTW_DEPTH | FTW_PHYS); }   // a directory and everything in it; symbolic links are removed, not followed
static int mkdir_p(const char *path) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(p, 0755) && errno != EEXIST) return -1; *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}
// The objects' state, freed when the last session or domain object holding it closes
static void file_free(void *data) { file_t *f = data; close(f->fd); free(f); }
static void dir_free(void *data) { dir_t *d = data; closedir(d->d); free(d); }

static uint32_t ifile(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    file_t *f = in->data;
    switch (in->cmd) {
    case 0: {                                                        // Read(option, offset, size) → bytes read, into the output buffer
        uint64_t off = in->args[2] | (uint64_t)in->args[3] << 32, size = in->args[4] | (uint64_t)in->args[5] << 32;
        if (!in->nrecv) return R_IO;
        uint64_t n = size < in->recv[0].size ? size : in->recv[0].size; ssize_t r = pread(f->fd, (void *)(uintptr_t)in->recv[0].addr, n, (off_t)off);
        OUT64(r < 0 ? 0 : (uint64_t)r); return 0;
    }
    case 1: {                                                        // Write(option, offset, size) from the input buffer
        uint64_t off = in->args[2] | (uint64_t)in->args[3] << 32, size = in->args[4] | (uint64_t)in->args[5] << 32;
        if (!in->nsend) return R_IO;
        uint64_t n = size < in->send[0].size ? size : in->send[0].size;
        return pwrite(f->fd, (const void *)(uintptr_t)in->send[0].addr, n, (off_t)off) < 0 ? R_IO : 0;
    }
    case 2: fsync(f->fd); return 0;                                  // Flush
    case 3: return ftruncate(f->fd, (off_t)(in->args[0] | (uint64_t)in->args[1] << 32)) ? R_IO : 0;   // SetSize
    case 4: { struct stat st; OUT64(fstat(f->fd, &st) ? 0 : (uint64_t)st.st_size); return 0; }       // GetSize
    default: fprintf(stderr, "[RED] ipc: fsp IFile command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

static uint32_t idirectory(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    dir_t *d = in->data;
    switch (in->cmd) {
    case 0: {                                                        // Read → entries (0x310 bytes each: name[0x301], type at +0x304, size at +0x308), count
        if (!in->nrecv) return R_IO;
        uint8_t *buf = (uint8_t *)(uintptr_t)in->recv[0].addr; uint64_t cap = in->recv[0].size / 0x310, n = 0;
        struct dirent *e;
        while (n < cap && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[PATH_MAX]; struct stat st; snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
            if (stat(p, &st)) continue;
            int isdir = S_ISDIR(st.st_mode);
            if (isdir ? !(d->filter & 1) : !(d->filter & 2)) continue;          // the filter: 1 = directories, 2 = files
            uint8_t *ent = buf + n * 0x310; memset(ent, 0, 0x310);
            strncpy((char *)ent, e->d_name, 0x300); ent[0x304] = isdir ? 0 : 1; int64_t sz = isdir ? 0 : (int64_t)st.st_size; memcpy(ent + 0x308, &sz, 8);
            n++;
        }
        OUT64(n); return 0;
    }
    case 1: {                                                        // GetEntryCount
        uint64_t n = 0; DIR *c = opendir(d->path); struct dirent *e;
        while (c && (e = readdir(c))) { if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++; }
        if (c) closedir(c);
        OUT64(n); return 0;
    }
    default: fprintf(stderr, "[RED] ipc: fsp IDirectory command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

static uint32_t ifilesystem(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    const fs_t *fs = in->data; char p[PATH_MAX], q[PATH_MAX]; struct stat st;
    switch (in->cmd) {
    case 0: {                                                        // CreateFile(mode, size, path)
        host_path(fs, in_path(in, 0), p);
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) return errno_result();
        uint64_t size = in->args[2] | (uint64_t)in->args[3] << 32; if (size) (void)!ftruncate(fd, (off_t)size);
        close(fd); return 0;
    }
    case 1: return unlink((host_path(fs, in_path(in, 0), p), p)) ? errno_result() : 0;     // DeleteFile
    case 2: return mkdir((host_path(fs, in_path(in, 0), p), p), 0755) ? errno_result() : 0;   // CreateDirectory
    case 3: return rmdir((host_path(fs, in_path(in, 0), p), p)) ? errno_result() : 0;         // DeleteDirectory
    case 4: case 13: host_path(fs, in_path(in, 0), p);              // DeleteDirectoryRecursively / CleanDirectoryRecursively
        if (stat(p, &st)) return R_PATH_NOT_FOUND;
        if (rm_tree(p) || (in->cmd == 13 && mkdir(p, 0755))) return R_IO;
        return 0;
    case 5: case 6: host_path(fs, in_path(in, 0), p); host_path(fs, in_path(in, 1), q); return rename(p, q) ? errno_result() : 0;   // RenameFile / RenameDirectory
    case 7: host_path(fs, in_path(in, 0), p);                       // GetEntryType → 0 directory, 1 file
        if (stat(p, &st)) return R_PATH_NOT_FOUND;
        OUT32(S_ISDIR(st.st_mode) ? 0 : 1); return 0;
    case 8: {                                                        // OpenFile(mode, path) → IFile
        host_path(fs, in_path(in, 0), p);
        int mode = (int)in->args[0], fd = open(p, (mode & 6) ? O_RDWR : O_RDONLY);
        if (fd < 0) return errno_result();
        file_t *f = calloc(1, sizeof *f); f->fd = fd;
        out->move[out->nmove++] = ipc_new_session_owned("IFile", ifile, f, file_free); return 0;
    }
    case 9: {                                                        // OpenDirectory(filter, path) → IDirectory
        host_path(fs, in_path(in, 0), p);
        DIR *dd = opendir(p); if (!dd) return errno_result();
        dir_t *d = calloc(1, sizeof *d); snprintf(d->path, sizeof d->path, "%s", p); d->d = dd; d->filter = (int)in->args[0];
        out->move[out->nmove++] = ipc_new_session_owned("IDirectory", idirectory, d, dir_free); return 0;
    }
    case 10: return 0;                                               // Commit
    case 11: OUT64(0x40000000ull); return 0;                         // GetFreeSpaceSize
    case 12: OUT64(0x80000000ull); return 0;                         // GetTotalSpaceSize
    case 14: for (int i = 0; i < 8; i++) OUT32(0); return 0;         // GetFileTimeStampRaw: created, modified, accessed, flags
    default: fprintf(stderr, "[RED] ipc: fsp IFileSystem command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

static int savedata_root(char *root) {                              // the save data directory, created if need be
    const char *base = getenv("NX_SAVE_DIR");
    snprintf(root, PATH_MAX, "%s/%016llx", base ? base : "work/save", (unsigned long long)mem_default_profile().program_id);
    return mkdir_p(root);
}
// Creates the save data directory (CreateSaveDataFileSystem).
uint32_t fsfs_create_savedata(void) { char root[PATH_MAX]; return savedata_root(root) ? R_IO : 0; }
// Opens (creating it if need be) the save data directory and returns it as an IFileSystem in out.
uint32_t fsfs_open_savedata(ipc_out_t *out) {
    fs_t *fs = calloc(1, sizeof *fs);
    if (savedata_root(fs->root)) { free(fs); return R_IO; }
    out->move[out->nmove++] = ipc_new_session_owned("IFileSystem", ifilesystem, fs, free);
    return 0;
}
