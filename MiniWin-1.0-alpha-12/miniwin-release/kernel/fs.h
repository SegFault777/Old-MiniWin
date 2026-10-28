#ifndef FS_H
#define FS_H
#include "io.h"
#include "ata.h"

/* Extremely minimal on-disk "file system": a small fixed number of file
 * slots, not a real directory structure like FAT. Each slot has a fixed,
 * pre-assigned filename -- there's no free-form renaming or arbitrary
 * filenames, just a small set of numbered slots that together implement
 * "don't silently overwrite an existing save": saving always picks the
 * first empty slot (or updates the slot the current document is already
 * bound to), giving NEW_TXT_DOC.TXT, NEW_TXT_DOC_2.TXT, NEW_TXT_DOC_3.TXT,
 * NEW_TXT_DOC_4.TXT.
 *
 * Disk layout (LBA = logical sector number, 512 bytes each):
 *   LBA 0        - boot sector
 *   LBA 1-64     - kernel image
 *   LBA 100+     - file slots, 9 sectors each (1 header + 8 data):
 *                    slot 0: LBA 100-108  -> "NEW_TXT_DOC.TXT"
 *                    slot 1: LBA 109-117  -> "NEW_TXT_DOC_2.TXT"
 *                    slot 2: LBA 118-126  -> "NEW_TXT_DOC_3.TXT"
 *                    slot 3: LBA 127-135  -> "NEW_TXT_DOC_4.TXT"
 */

#define FS_MAGIC           0x31573154u
#define FS_BASE_LBA        100
#define FS_SLOT_SECTORS    9      /* 1 header + 8 data sectors per slot */
#define FS_DATA_SECTORS    8
#define FS_MAX_FILE_BYTES  (FS_DATA_SECTORS * 512)  /* 4096 bytes per file */
#define FS_MAX_FILES       4

static const char *fs_slot_names[FS_MAX_FILES] = {
    "NEW_TXT_DOC.TXT",
    "NEW_TXT_DOC_2.TXT",
    "NEW_TXT_DOC_3.TXT",
    "NEW_TXT_DOC_4.TXT",
};

static inline u32 fs_slot_header_lba(int slot) { return FS_BASE_LBA + (u32)slot * FS_SLOT_SECTORS; }
static inline u32 fs_slot_data_lba(int slot)   { return fs_slot_header_lba(slot) + 1; }

static inline void fs_zero(u8 *buf, u32 n) {
    for (u32 i = 0; i < n; i++) buf[i] = 0;
}

static inline void fs_strcpy_fixed(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
}

/* Write `len` bytes from `data` to the given slot (0..FS_MAX_FILES-1)
 * using that slot's canonical filename. Returns 1 on success. */
static inline int fs_save_slot(int slot, const char *data, u32 len) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    if (len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES;

    u8 header[512];
    fs_zero(header, 512);
    *(u32*)(header + 0) = FS_MAGIC;
    *(u32*)(header + 4) = len;
    fs_strcpy_fixed((char*)(header + 8), fs_slot_names[slot], 32);
    if (!ata_write_sector(fs_slot_header_lba(slot), header)) return 0;

    u32 remaining = len;
    const char *src = data;
    u32 data_lba = fs_slot_data_lba(slot);
    for (int s = 0; s < FS_DATA_SECTORS; s++) {
        u8 sector[512];
        fs_zero(sector, 512);
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) sector[i] = (u8)src[i];
        if (!ata_write_sector(data_lba + s, sector)) return 0;
        src += chunk;
        remaining -= chunk;
    }
    return 1;
}

/* Returns 1 if `slot` contains a saved file, filling *out_len. */
static inline int fs_check_slot(int slot, u32 *out_len) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    u8 header[512];
    if (!ata_read_sector(fs_slot_header_lba(slot), header)) return 0;
    u32 magic = *(u32*)(header + 0);
    if (magic != FS_MAGIC) return 0;
    u32 len = *(u32*)(header + 4);
    if (len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES;
    if (out_len) *out_len = len;
    return 1;
}

/* Loads `slot`'s content into dst (up to maxlen bytes). Returns the
 * number of bytes actually loaded, or 0 if the slot is empty. */
static inline u32 fs_load_slot(int slot, char *dst, u32 maxlen) {
    u32 len = 0;
    if (!fs_check_slot(slot, &len)) return 0;
    if (len > maxlen) len = maxlen;

    u32 remaining = len;
    char *dstp = dst;
    u32 data_lba = fs_slot_data_lba(slot);
    for (int s = 0; s < FS_DATA_SECTORS && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(data_lba + s, sector)) break;
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) dstp[i] = (char)sector[i];
        dstp += chunk;
        remaining -= chunk;
    }
    return len - remaining;
}

/* Returns the index of the first slot with no saved file, or -1 if all
 * FS_MAX_FILES slots are occupied (storage full). */
static inline int fs_find_empty_slot(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        u32 len;
        if (!fs_check_slot(i, &len)) return i;
    }
    return -1;
}

#endif
