#include "tusb.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdbool.h>

// Last 1 MB of 4 MB flash (offset 0x300000)
#define DISK_FLASH_OFFSET    (3u * 1024u * 1024u)
#define DISK_SECTOR_SIZE     512u
#define DISK_SECTOR_COUNT    2048u  // 1 MB / 512

// 8 logical sectors per 4 KB flash erase block
#define FLASH_SECTORS_PER_DISK_ERASE  (FLASH_SECTOR_SIZE / DISK_SECTOR_SIZE)

// FAT12 layout (2048 sectors, 1 sec/cluster):
//   Sector 0       : boot
//   Sectors  1-6   : FAT1 (6 sectors)
//   Sectors  7-12  : FAT2 (6 sectors)
//   Sectors 13-16  : root dir (64 entries × 32 B = 2048 B = 4 sectors)
//   Sectors 17+    : data

static uint8_t  write_cache[FLASH_SECTOR_SIZE];
static int32_t  cached_erase_block = -1;
static bool     cache_dirty = false;

static void cache_flush(void) {
    if (!cache_dirty || cached_erase_block < 0) return;
    uint32_t offset = DISK_FLASH_OFFSET + (uint32_t)cached_erase_block * FLASH_SECTOR_SIZE;
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, write_cache, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
    cache_dirty = false;
}

static void cache_load(int32_t erase_block) {
    if (cached_erase_block == erase_block) return;
    cache_flush();
    uint32_t offset = DISK_FLASH_OFFSET + (uint32_t)erase_block * FLASH_SECTOR_SIZE;
    memcpy(write_cache, (const uint8_t *)(XIP_BASE + offset), FLASH_SECTOR_SIZE);
    cached_erase_block = erase_block;
}

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

static bool disk_is_formatted(void) {
    const uint8_t *sector0 = (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET);
    return sector0[510] == 0x55 && sector0[511] == 0xAA;
}

static void write_erase_block(uint32_t block_index, const uint8_t *data) {
    uint32_t offset = DISK_FLASH_OFFSET + block_index * FLASH_SECTOR_SIZE;
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
}

static void format_disk(void) {
    // Erase all 256 flash erase blocks in the disk area
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t off = 0; off < 1024u * 1024u; off += FLASH_SECTOR_SIZE)
        flash_range_erase(DISK_FLASH_OFFSET + off, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);

    // --- Flash erase block 0 (disk bytes 0-4095) ---
    // Contains: boot sector, FAT1 (6 sectors), first 512 B of FAT2
    memset(write_cache, 0x00, sizeof(write_cache));

    // Boot sector BPB
    uint8_t *b = write_cache;
    b[0] = 0xEB; b[1] = 0xFE; b[2] = 0x90;           // jmp $; NOP
    memcpy(b + 3, "MSDOS5.0", 8);
    b[11] = 0x00; b[12] = 0x02;                        // BytsPerSec = 512
    b[13] = 1;                                          // SecPerClus = 1
    b[14] = 1;    b[15] = 0;                            // RsvdSecCnt = 1
    b[16] = 2;                                          // NumFATs = 2
    b[17] = 64;   b[18] = 0;                            // RootEntCnt = 64
    b[19] = 0x00; b[20] = 0x08;                         // TotSec16 = 2048
    b[21] = 0xF8;                                       // Media = fixed
    b[22] = 6;    b[23] = 0;                            // FATSz16 = 6
    b[24] = 1;    b[25] = 0;                            // SecPerTrk = 1
    b[26] = 1;    b[27] = 0;                            // NumHeads = 1
    b[36] = 0x80;                                       // DrvNum
    b[38] = 0x29;                                       // ExtBootSig
    b[39] = 0x01; b[40] = 0x02; b[41] = 0x03; b[42] = 0x04; // VolID
    memcpy(b + 43, "OWLSAT     ", 11);
    memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55; b[511] = 0xAA;

    // FAT1 at offset 512: entry 0 = media descriptor, entry 1 = EOC
    write_cache[512] = 0xF8; write_cache[513] = 0xFF; write_cache[514] = 0xFF;

    // FAT2 starts at offset 3584 (sector 7): same header
    write_cache[3584] = 0xF8; write_cache[3585] = 0xFF; write_cache[3586] = 0xFF;

    write_erase_block(0, write_cache);

    // --- Flash erase block 1 (disk bytes 4096-8191) ---
    // Contains: FAT2 remainder + first 3 root dir sectors — all zeros
    memset(write_cache, 0x00, sizeof(write_cache));
    write_erase_block(1, write_cache);

    // --- Flash erase block 2 (disk bytes 8192-12287) ---
    // First 512 bytes = last root dir sector (zeros); rest = data area (leave as 0xFF)
    memset(write_cache, 0xFF, sizeof(write_cache));
    memset(write_cache, 0x00, 512);
    write_erase_block(2, write_cache);

    cached_erase_block = -1;
    cache_dirty = false;
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

void msc_disk_init(void) {
    if (!disk_is_formatted()) format_disk();
}

// ---------------------------------------------------------------------------
// TinyUSB MSC callbacks
// ---------------------------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,   "OwlSat  ", 8);
    memcpy(product_id,  "Storage         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = DISK_SECTOR_COUNT;
    *block_size  = DISK_SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start) cache_flush();
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    if (lba >= DISK_SECTOR_COUNT) return -1;

    uint32_t disk_offset = lba * DISK_SECTOR_SIZE + offset;
    int32_t  erase_block = (int32_t)(disk_offset / FLASH_SECTOR_SIZE);

    // Serve from cache if dirty block matches; else read directly via XIP
    if (cache_dirty && cached_erase_block == erase_block) {
        uint32_t cache_off = disk_offset % FLASH_SECTOR_SIZE;
        memcpy(buffer, write_cache + cache_off, bufsize);
    } else {
        memcpy(buffer, (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET + disk_offset), bufsize);
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (lba >= DISK_SECTOR_COUNT) return -1;

    uint32_t disk_offset = lba * DISK_SECTOR_SIZE + offset;
    int32_t  erase_block = (int32_t)(disk_offset / FLASH_SECTOR_SIZE);

    cache_load(erase_block);  // flushes previous block if different
    memcpy(write_cache + (disk_offset % FLASH_SECTOR_SIZE), buffer, bufsize);
    cache_dirty = true;

    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
        case 0x1E:  // PREVENT_ALLOW_MEDIUM_REMOVAL
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}
