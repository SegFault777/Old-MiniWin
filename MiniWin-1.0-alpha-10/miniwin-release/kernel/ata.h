#ifndef ATA_H
#define ATA_H
#include "io.h"

/* Primary ATA bus, PIO mode, LBA28 addressing. This talks directly to the
 * disk controller hardware -- no BIOS calls, since BIOS interrupts aren't
 * usable once the CPU is in 32-bit protected mode. QEMU's default
 * `-drive format=raw,file=...` (no explicit if=) attaches as legacy IDE,
 * which is exactly what the bootloader's BIOS int 0x13 already read from
 * to load this kernel -- so these are the same underlying disk. */

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_CACHE_FLUSH    0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

static inline void ata_wait_bsy_clear(void) {
    while (inb(ATA_STATUS) & ATA_SR_BSY) { }
}

/* Wait for the drive to signal it's ready to transfer data (DRQ set), or
 * that an error occurred (ERR set). Returns 1 if ready, 0 on error. */
static inline int ata_wait_drq(void) {
    u8 status;
    while (1) {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) return 1;
    }
}

static inline void ata_select_lba(u32 lba) {
    /* 0xE0 = master drive, LBA mode; top 4 bits of a 28-bit LBA go in the
     * low nibble of the drive/head register. */
    outb(ATA_DRIVE_HEAD, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO,  (u8)(lba & 0xFF));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (u8)((lba >> 16) & 0xFF));
}

/* Read one 512-byte sector at `lba` into `buf` (must have room for 512
 * bytes). Returns 1 on success, 0 on error. */
static inline int ata_read_sector(u32 lba, void *buf) {
    ata_wait_bsy_clear();
    ata_select_lba(lba);
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    if (!ata_wait_drq()) return 0;

    u16 *p = (u16*)buf;
    for (int i = 0; i < 256; i++) {
        p[i] = inw(ATA_DATA);
    }
    return 1;
}

/* Write one 512-byte sector at `lba` from `buf`. Returns 1 on success.
 * Flushes the drive's write cache afterward so the data is actually
 * committed to the underlying image file, not just sitting in a volatile
 * cache that a "power off" (closing QEMU) could lose. */
static inline int ata_write_sector(u32 lba, const void *buf) {
    ata_wait_bsy_clear();
    ata_select_lba(lba);
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (!ata_wait_drq()) return 0;

    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, p[i]);
    }

    ata_wait_bsy_clear();
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy_clear();
    return 1;
}

#endif
