// The MiiModel system data (0100000000000802, the firmware's Mii head models) as a RomFS of stand-in files, for fsp-srv's OpenDataStorageByDataId. A title links the SDK's Mii rendering
// code and reads two kinds of file whole, building each part of a head from them; the firmware's files are not the host's to ship, so these have the same layout with every part absent
// (layout as nxnative's hle_mii.c documents it for Mario Kart 8 Deluxe):
//   texture file: +0x0c u32 largest decompressed size of each of the 11 part kinds, +0x38 their alignments, then from +0x64 one 0x1c-byte entry per part (327 of them), all zero: a texture
//     whose size is 0 is skipped when the model is built;
//   shape file: +0x0c the 12 largest sizes, +0x3c their alignments, from +0x6c one 0x58-byte entry per part (857), then the hair transforms (132 x 0x48) and the faceline transforms (12 x 0x24).
//     Shapes cannot all be absent (the faceline is drawn without a check), so every entry points at one shared mesh: one vertex at the origin (attribute 0, 4 halves) and the triangle
//     (0, 0, 0), 0x20 bytes of zeros stored as a zlib stream (one raw deflate block). It builds and draws nothing.
// The largest sizes size the model's memory pool and the first alignment is its alignment, so they are one page each.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { TEX_SIZE = 0x64 + 0x1c * 327, MESH_AT = 0x14de0, MESH = 0x20 };
static const uint8_t mesh_z[] = {0x78, 0x01, 0x01, MESH, 0, (uint8_t)~MESH, 0xff,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, MESH, 0x00, 0x01};   // zlib header, stored block, 32 zeros, Adler-32 of them
enum { SHAPE_SIZE = MESH_AT + sizeof mesh_z };

static uint8_t *model_file(int parts, size_t size) {
    uint8_t *b = calloc(1, size);
    for (int i = 0; i < parts; i++) { uint32_t page = 0x1000; memcpy(b + 0xc + 4 * i, &page, 4); memcpy(b + 0xc + 4 * parts + 4 * i, &page, 4); }
    if (parts == 12) {
        for (int i = 0; i < 857; i++) {
            uint32_t *e = (uint32_t *)(b + 0x6c + 0x58 * i);
            e[0] = MESH_AT; e[1] = MESH; e[2] = sizeof mesh_z;             // data offset, inflated size, stored size
            e[4 + 5] = 8;                                                   // attribute 0 (offset 0) is one 8-byte vertex; attributes 1-4 absent
            e[14] = 0x10; e[15] = 6;                                        // index offset and size: three u16 indices
        }
        memcpy(b + MESH_AT, mesh_z, sizeof mesh_z);
    }
    return b;
}

static uint32_t hash_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = 123456789u ^ parent;
    for (const char *p = name; *p; p++) h = ((h >> 5) | (h << 27)) ^ (uint8_t)*p;
    return h;
}

// A standard RomFS with the six files in its root (names in byte order, data on 0x10 boundaries). Returns a buffer that lives for the whole run.
const uint8_t *mii_standin_romfs(size_t *size_out) {
    static uint8_t *img; static size_t img_size;
    if (img) { *size_out = img_size; return img; }
    static const struct { const char *name; int parts; } F[6] = {
        {"NXTextureLowLinear.dat", 11}, {"NXTextureLowSRGB.dat", 11}, {"NXTextureMidLinear.dat", 11}, {"NXTextureMidSRGB.dat", 11}, {"ShapeHigh.dat", 12}, {"ShapeMid.dat", 12}};
    uint32_t nf = 6, fh = hash_count(nf), foff[6], fsize[6], dataoff[6], mt = 0, dpos = 0;
    for (uint32_t i = 0; i < nf; i++) { foff[i] = mt; mt += 32 + ((uint32_t)strlen(F[i].name) + 3 & ~3u); fsize[i] = F[i].parts == 12 ? SHAPE_SIZE : TEX_SIZE; dataoff[i] = dpos; dpos = (dpos + fsize[i] + 15) & ~15u; }
    uint32_t dh_off = 0x200, dm_off = dh_off + 12, fh_off = dm_off + 24, fm_off = fh_off + 4 * fh, data_off = (fm_off + mt + 0x1ff) & ~0x1ffu;
    img_size = data_off + dpos; img = calloc(1, img_size);
    uint64_t h[10] = {0x50, dh_off, 12, dm_off, 24, fh_off, 4 * fh, fm_off, mt, data_off}; memcpy(img, h, sizeof h);
    memset(img + dh_off, 0xff, 12);
    memcpy(img + dh_off + 4 * (name_hash(0, "") % 3), &(uint32_t){0}, 4);                   // the root directory (offset 0) is in the directory hash table too: the SDK finds a file's parent through it
    uint32_t root[6] = {0, 0xffffffff, 0xffffffff, foff[0], 0xffffffff, 0}; memcpy(img + dm_off, root, sizeof root);   // the root: first file at foff[0]
    memset(img + fh_off, 0xff, 4 * fh);
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t slot = name_hash(0, F[i].name) % fh, prev; memcpy(&prev, img + fh_off + 4 * slot, 4); memcpy(img + fh_off + 4 * slot, &foff[i], 4);
        uint32_t sib = i + 1 < nf ? foff[i + 1] : 0xffffffffu, len = (uint32_t)strlen(F[i].name);
        uint8_t *e = img + fm_off + foff[i];
        memcpy(e, &(uint32_t){0}, 4); memcpy(e + 4, &sib, 4); uint64_t d = dataoff[i], z = fsize[i]; memcpy(e + 8, &d, 8); memcpy(e + 16, &z, 8);
        memcpy(e + 24, &prev, 4); memcpy(e + 28, &len, 4); memcpy(e + 32, F[i].name, len);
        uint8_t *f = model_file(F[i].parts, fsize[i]); memcpy(img + data_off + dataoff[i], f, fsize[i]); free(f);
    }
    *size_out = img_size;
    return img;
}
