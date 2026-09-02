/**

  @file       storage.h
  @brief      Region layer: bounds-checked named spans of the nonvolatile half of the QSPI device.
  @details    Layer 1 of the storage design in docs/internal/storage_api.md. The upper half of the
              QSPI device is reserved for nonvolatile data; the program lives in the lower half.
              This header carves that reservation into fixed, sector-aligned regions and exposes
              the only four operations anything above it may perform on them: read, erase,
              program, and a layout query.

              @par What this layer promises
              - No client ever holds a raw flash offset. A storage_region_t is resolved from an
                enum and every call bounds-checks against it, so a bug in a client cannot reach
                the program image. On the single-device layout this arithmetic is the only thing
                between a storage bug and an unbootable satellite, which is why the checks are
                enforced three times: at compile time on the region table, once at boot against
                the linker's idea of where the program ends, and on every call.
              - Erase and program run with interrupts disabled, one sector or one page per
                critical section, through flash_safe_execute(). See storage_api.md §6 before
                calling either from anything with a watchdog deadline.
              - storage_region_write() does not erase. Bits only go 1 -> 0; read-modify-write is
                explicit and belongs to the layer above.

              @par Flash semantics, by assumption
              The flight part is MRAM whose boot behaviour hardware has not yet confirmed. Software
              treats it as a NOR flash of the same size (storage_api.md §0), which is the stricter
              contract. Nothing here is MRAM-specific.

              @par Threading
              All calls take the storage mutex with a finite timeout and report STORAGE_ERR_BUSY
              rather than blocking indefinitely. Callable from any task; never from an ISR.
              storage_init() must run before the scheduler starts.

  @author     Viola Case
  @date       02.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hardware/flash.h>
#include <pico/config.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Partition
//
// The nonvolatile reservation is the upper half of whatever device is fitted, derived from
// PICO_FLASH_SIZE_BYTES rather than written down, because the dev board (4 MB) and the flight
// part (2 MiB) are not the same size. A hard-coded base is correct on exactly one device and
// silently overlaps the program on every other.
//
// Anchoring to the top rather than to __flash_binary_end is the safety argument: the binary end
// moves every build, a top-anchored base does not, so stored data survives a firmware update.
// ---------------------------------------------------------------------------

/// Bytes reserved for nonvolatile data: the upper half of the device.
#define STORAGE_RESERVE_BYTES ((uint32_t) PICO_FLASH_SIZE_BYTES / 2u)

/// Offset of the reservation from XIP_BASE. Offsets passed to the SDK are relative to this base.
#define STORAGE_BASE_OFFSET ((uint32_t) PICO_FLASH_SIZE_BYTES - STORAGE_RESERVE_BYTES)

/**
 * Slack demanded between the end of the program image and STORAGE_BASE_OFFSET at boot.
 *
 * Not the firmware-update staging area, which lives in the same gap and is sized separately
 * (storage_api.md §8). This is only the margin the boot-time overlap check insists on so that a
 * build one byte short of the boundary is refused rather than allowed to sit against it.
 */
#define STORAGE_PROGRAM_MARGIN_BYTES ((uint32_t) FLASH_SECTOR_SIZE)


// ---------------------------------------------------------------------------
// Region map  (offsets relative to STORAGE_BASE_OFFSET; storage_api.md §3)
//
// Every region is sector-aligned and a whole number of sectors, because 4 KB is the erase unit
// and a region sharing a sector with its neighbour cannot be erased without destroying the
// neighbour. Enforced by static_assert in storage.c, not by convention.
// ---------------------------------------------------------------------------

/// Compile-time identity of a reserved span.
typedef enum {
  REGION_CONFIG_A = 0, ///< Key/value bank A, 16 KB.
  REGION_CONFIG_B,     ///< Key/value bank B, 16 KB.
  REGION_LATCH,        ///< Write-once latched flags, 4 KB = 16 pages, one flag per page.
  REGION_SCRATCH,      ///< Firmware-update metadata, 4 KB. Never the image itself.
  REGION_COUNT
} storage_region_id_t;

#define STORAGE_REGION_CONFIG_A_OFFSET 0x00000u
#define STORAGE_REGION_CONFIG_A_SIZE   0x04000u
#define STORAGE_REGION_CONFIG_B_OFFSET 0x04000u
#define STORAGE_REGION_CONFIG_B_SIZE   0x04000u
#define STORAGE_REGION_LATCH_OFFSET    0x08000u
#define STORAGE_REGION_LATCH_SIZE      0x01000u
#define STORAGE_REGION_SCRATCH_OFFSET  0x09000u
#define STORAGE_REGION_SCRATCH_SIZE    0x01000u

/// First byte after the last allocated region. Everything from here to the end of the
/// reservation is unallocated; storage_api.md §3 says why it stays that way.
#define STORAGE_REGIONS_END (STORAGE_REGION_SCRATCH_OFFSET + STORAGE_REGION_SCRATCH_SIZE)


// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// Result of every call in this layer. Zero is success; everything else names what went wrong.
typedef enum {
  STORAGE_OK = 0,
  STORAGE_ERR_BOUNDS,    ///< Access fell outside the region.
  STORAGE_ERR_ALIGN,     ///< Offset or length violated page/sector alignment.
  STORAGE_ERR_BUSY,      ///< Could not take the storage mutex before timeout.
  STORAGE_ERR_VERIFY,    ///< Read-back after program did not match.
  STORAGE_ERR_CORRUPT,   ///< Header magic or CRC rejected. Reserved for layer 2.
  STORAGE_ERR_NOT_FOUND, ///< No such key. Reserved for layer 2.
  STORAGE_ERR_NO_SPACE,  ///< Value would not fit in the bank. Reserved for layer 2.
  STORAGE_ERR_UNSAFE,    ///< Writes are disarmed: init failed, or flash_safe_execute refused.
  STORAGE_ERR_PARAM,     ///< Null pointer or invalid region id.
} storage_err_t;

/**
 * @brief Opaque handle to a region. Holds the resolved base and size; never a raw pointer.
 *
 * @c base is an offset from XIP_BASE, i.e. what the SDK's flash functions want. @c cs is the QMI
 * chip select and is always 0 on the single-device layout; it stays in the struct so the
 * two-device split recorded in storage_api.md §2.2 is cheap to revive.
 */
typedef struct {
  uint32_t base;
  uint32_t size;
  uint8_t  cs;
} storage_region_t;

/// Diagnostic snapshot of the partition, for the console and for bring-up.
typedef struct {
  uint32_t device_bytes;      ///< PICO_FLASH_SIZE_BYTES as built.
  uint32_t reserve_bytes;     ///< STORAGE_RESERVE_BYTES.
  uint32_t base_offset;       ///< STORAGE_BASE_OFFSET.
  uint32_t program_end;       ///< __flash_binary_end as an offset from XIP_BASE.
  uint32_t allocated_bytes;   ///< Sum of the regions in the map.
  bool     armed;             ///< True if storage_init() passed every check and writes are allowed.
} storage_layout_t;


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Creates the storage mutex and runs the boot-time overlap check.
 *
 * Call once, before the scheduler starts. Confirms that STORAGE_BASE_OFFSET lies beyond the end
 * of the program image plus STORAGE_PROGRAM_MARGIN_BYTES — the one check that cannot be a
 * static_assert because the binary size is not known to the preprocessor.
 *
 * On failure the write path stays disarmed for the life of the boot: erase and program return
 * STORAGE_ERR_UNSAFE, reads still work, and the fault is reported on the console. It does not
 * configASSERT — a satellite that cannot save config should still fly.
 *
 * @return STORAGE_OK, or STORAGE_ERR_UNSAFE if the program overlaps the reservation, or
 *         STORAGE_ERR_BUSY if the mutex could not be created.
 */
storage_err_t storage_init(void);

/// @return True once storage_init() has passed every check. False before init or after a fault.
bool storage_is_armed(void);

/// @return A snapshot of the partition as built and as checked at boot.
storage_layout_t storage_layout(void);

/// Prints the layout and the region map on the console. Bring-up aid; safe to call any time.
void storage_print_layout(void);

/// @return A short, stable name for @p err, for log lines.
const char *storage_err_str(storage_err_t err);


// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

/**
 * @brief Resolves a region id into a handle.
 * @param id  Which region.
 * @param out Filled on success.
 * @return STORAGE_OK, or STORAGE_ERR_PARAM for a bad id or null @p out.
 */
storage_err_t storage_region_open(storage_region_id_t id, storage_region_t *out);

/**
 * @brief Reads from a region. Offsets are region-relative. No alignment requirement.
 *
 * Reads through the XIP window, so the flash cache applies. This layer flushes the cache after
 * every erase and program, so a read that follows a write through this API sees the new data.
 *
 * @param r   Region handle.
 * @param off Byte offset within the region.
 * @param buf Destination, at least @p len bytes.
 * @param len Bytes to copy.
 * @return STORAGE_OK, STORAGE_ERR_BOUNDS, STORAGE_ERR_BUSY or STORAGE_ERR_PARAM.
 */
storage_err_t storage_region_read(const storage_region_t *r, uint32_t off, void *buf, size_t len);

/**
 * @brief Erases whole sectors. @p off and @p len must be FLASH_SECTOR_SIZE multiples.
 *
 * One sector per critical section: interrupts are re-enabled between sectors so the tick catches
 * up and the watchdog task gets to run. Each sector still costs one interrupts-off window of the
 * fitted part's sector-erase time — tens of milliseconds typical, hundreds worst case on NOR.
 *
 * @return STORAGE_OK, STORAGE_ERR_ALIGN, STORAGE_ERR_BOUNDS, STORAGE_ERR_BUSY,
 *         STORAGE_ERR_UNSAFE or STORAGE_ERR_VERIFY (the sector did not read back as 0xFF).
 */
storage_err_t storage_region_erase(const storage_region_t *r, uint32_t off, size_t len);

/**
 * @brief Programs into already-erased space. @p off and @p len must be FLASH_PAGE_SIZE multiples.
 *
 * Does NOT erase first. Programming a page that is not 0xFF produces the bitwise AND of old and
 * new contents, which this call detects on verify and reports as STORAGE_ERR_VERIFY — but by
 * then the page is already wrong. Erase first, at the layer that knows what else shares the
 * sector.
 *
 * One page per critical section. Verified by reading back through the uncached XIP alias, so the
 * flash cache cannot vouch for a write that did not happen.
 *
 * @return STORAGE_OK, STORAGE_ERR_ALIGN, STORAGE_ERR_BOUNDS, STORAGE_ERR_BUSY,
 *         STORAGE_ERR_UNSAFE or STORAGE_ERR_VERIFY.
 */
storage_err_t storage_region_write(const storage_region_t *r, uint32_t off, const void *buf, size_t len);

#ifdef __cplusplus
} // extern "C"
#endif
