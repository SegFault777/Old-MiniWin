#ifndef FS_H
#define FS_H
#include "io.h"
#include "ata.h"

/* An extremely minimal on-disk "file system": a single fixed file slot,
 * not a real directory structure like FAT. This is intentionally as
 * simple as possible while still being genuinely persistent -- the goal
 * is real, working save/load, not a full filesystem implementation.
 *
 * Disk layout (LBA = logical sector number, 512 bytes each):
 *   LBA 0        - boot sector
 *   LBA 1-64     - kernel image
 *   LBA 100      - file header sector (magic, length, filename)
 *   LBA 101-108  - file data (up to 8 sectors = 4096 bytes)
 * (LBAs 65-99 are left unused as headroom for the kernel to grow.) */

#define FS_MAGIC          0x31573154u  /* arbitrary signature marking "a file is saved here" */
#define FS_HEADER_LBA     100
#define FS_DATA_LBA       101
#define FS_DATA_SECTORS   8             /* 8 * 512 = 4096 bytes max file size */
#define FS_MAX_FILE_BYTES (FS_DATA_SECTORS * 512)

static inline void fs_zero(u8 *buf, u32 n) {
    for (u32 i = 0; i < n; i++) buf[i] = 0;
}

static inline void fs_strcpy_fixed(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
}

/* Write `len` bytes from `data` to the fixed file slot on disk, under the
 * name "NEW_TXT_DOC.TXT". Returns 1 on success, 0 if any sector write
 * failed. */
static inline int fs_save_file(const char *data, u32 len) {
    if (len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES;

    u8 header[512];
    fs_zero(header, 512);
    *(u32*)(header + 0) = FS_MAGIC;
    *(u32*)(header + 4) = len;
    fs_strcpy_fixed((char*)(header + 8), "NEW_TXT_DOC.TXT", 32);
    if (!ata_write_sector(FS_HEADER_LBA, header)) return 0;

    u32 remaining = len;
    const char *src = data;
    for (int s = 0; s < FS_DATA_SECTORS; s++) {
        u8 sector[512];
        fs_zero(sector, 512);
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) sector[i] = (u8)src[i];
        if (!ata_write_sector(FS_DATA_LBA + s, sector)) return 0;
        src += chunk;
        remaining -= chunk;
    }
    return 1;
}

/* Check whether a previously-saved file exists on disk (call once at
 * boot). Returns 1 and fills *out_len if found, 0 otherwise. */
static inline int fs_check_existing_file(u32 *out_len) {
    u8 header[512];
    if (!ata_read_sector(FS_HEADER_LBA, header)) return 0;
    u32 magic = *(u32*)(header + 0);
    if (magic != FS_MAGIC) return 0;
    u32 len = *(u32*)(header + 4);
    if (len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES;
    if (out_len) *out_len = len;
    return 1;
}

/* Load the saved file's contents into `dst` (up to `maxlen` bytes).
 * Returns the number of bytes actually loaded, or 0 if no file exists. */
static inline u32 fs_load_file(char *dst, u32 maxlen) {
    u32 len = 0;
    if (!fs_check_existing_file(&len)) return 0;
    if (len > maxlen) len = maxlen;

    u32 remaining = len;
    char *dstp = dst;
    for (int s = 0; s < FS_DATA_SECTORS && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(FS_DATA_LBA + s, sector)) break;
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) dstp[i] = (char)sector[i];
        dstp += chunk;
        remaining -= chunk;
    }
    return len - remaining;
}

#endif
