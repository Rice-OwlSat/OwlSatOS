/**
 * @file storage.c
 * @brief Region layer over the nonvolatile half of the QSPI device. See OwlSat/storage.h.
 *
 * Three places enforce "do not touch the program":
 *
 *   1. The static_asserts below, on the region table and the partition arithmetic.
 *   2. storage_init(), once, against __flash_binary_end.
 *   3. bounds_check(), on every call, with the addition checked for overflow first.
 *
 * Every erase and program goes through flash_safe_execute(), which disables interrupts for the
 * duration. This port is single-core (RP2350_ARM_NTZ, core 1 never started) and the build sets
 * PICO_FLASH_ASSUME_CORE1_SAFE=1, so the call reduces to an interrupts-off critical section —
 * but routing through it anyway means the day someone starts core 1 this file is not the bug.
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <hardware/flash.h>
#include <hardware/regs/addressmap.h>
#include <pico/flash.h>

#include <OwlSat/storage.h>


// ---------------------------------------------------------------------------
// Compile-time checks  (storage_api.md §4.2)
// ---------------------------------------------------------------------------

#define IS_SECTOR_ALIGNED(x) (((x) % FLASH_SECTOR_SIZE) == 0u)

// The reservation must be strictly smaller than the device. With the base derived by
// subtraction, an oversized reservation does not fail loudly — it wraps, and lands storage on
// top of the program. This is the assertion that makes the subtraction safe to write.
_Static_assert(STORAGE_RESERVE_BYTES > 0u, "storage reservation is empty");
_Static_assert(STORAGE_RESERVE_BYTES < (uint32_t) PICO_FLASH_SIZE_BYTES,
               "storage reservation does not fit the device");
_Static_assert(STORAGE_BASE_OFFSET + STORAGE_RESERVE_BYTES == (uint32_t) PICO_FLASH_SIZE_BYTES,
               "storage reservation is not anchored to the top of the device");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_BASE_OFFSET), "STORAGE_BASE_OFFSET is not sector-aligned");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_RESERVE_BYTES), "STORAGE_RESERVE_BYTES is not sector-aligned");

// Every region sector-aligned in base and size.
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_CONFIG_A_OFFSET), "CONFIG_A offset");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_CONFIG_A_SIZE),   "CONFIG_A size");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_CONFIG_B_OFFSET), "CONFIG_B offset");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_CONFIG_B_SIZE),   "CONFIG_B size");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_LATCH_OFFSET),    "LATCH offset");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_LATCH_SIZE),      "LATCH size");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_SCRATCH_OFFSET),  "SCRATCH offset");
_Static_assert(IS_SECTOR_ALIGNED(STORAGE_REGION_SCRATCH_SIZE),    "SCRATCH size");

// No two regions overlap. The table is laid out in ascending order, so adjacency suffices.
_Static_assert(STORAGE_REGION_CONFIG_A_OFFSET + STORAGE_REGION_CONFIG_A_SIZE <= STORAGE_REGION_CONFIG_B_OFFSET,
               "CONFIG_A overlaps CONFIG_B");
_Static_assert(STORAGE_REGION_CONFIG_B_OFFSET + STORAGE_REGION_CONFIG_B_SIZE <= STORAGE_REGION_LATCH_OFFSET,
               "CONFIG_B overlaps LATCH");
_Static_assert(STORAGE_REGION_LATCH_OFFSET + STORAGE_REGION_LATCH_SIZE <= STORAGE_REGION_SCRATCH_OFFSET,
               "LATCH overlaps SCRATCH");

// The last region ends inside the reservation.
_Static_assert(STORAGE_REGIONS_END <= STORAGE_RESERVE_BYTES, "region map overruns the reservation");

// The latch region holds one flag per page, and the design counts on exactly sixteen.
_Static_assert(STORAGE_REGION_LATCH_SIZE / FLASH_PAGE_SIZE == 16u, "latch region is not 16 pages");


// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

/// Linker-provided end of the program image, as an address in the XIP window.
extern char __flash_binary_end;

/// How long a caller waits for the storage mutex before STORAGE_ERR_BUSY.
#define STORAGE_LOCK_TIMEOUT_MS 100u

/// How long flash_safe_execute() may wait to enter its critical section.
#define STORAGE_SAFE_EXECUTE_TIMEOUT_MS 100u

static const struct {
  uint32_t    offset;
  uint32_t    size;
  const char *name;
} k_regions[REGION_COUNT] = {
  [REGION_CONFIG_A] = { STORAGE_REGION_CONFIG_A_OFFSET, STORAGE_REGION_CONFIG_A_SIZE, "CONFIG_A" },
  [REGION_CONFIG_B] = { STORAGE_REGION_CONFIG_B_OFFSET, STORAGE_REGION_CONFIG_B_SIZE, "CONFIG_B" },
  [REGION_LATCH]    = { STORAGE_REGION_LATCH_OFFSET,    STORAGE_REGION_LATCH_SIZE,    "LATCH"    },
  [REGION_SCRATCH]  = { STORAGE_REGION_SCRATCH_OFFSET,  STORAGE_REGION_SCRATCH_SIZE,  "SCRATCH"  },
};

static SemaphoreHandle_t g_lock = NULL;

/// True only after storage_init() passed every check. Cleared by any later fault.
static bool g_armed = false;

/// __flash_binary_end as an offset from XIP_BASE, captured at init for storage_layout().
static uint32_t g_program_end = 0;


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * Takes the storage mutex.
 *
 * Before the scheduler starts there is nobody to contend with and blocking is impossible, so the
 * take is attempted with a zero timeout; FreeRTOS would otherwise try to suspend a task that
 * does not exist yet.
 */
static storage_err_t lock(void) {
  if (g_lock == NULL) {
    return STORAGE_ERR_UNSAFE;
  }
  const TickType_t wait = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
                              ? pdMS_TO_TICKS(STORAGE_LOCK_TIMEOUT_MS)
                              : (TickType_t) 0;
  return xSemaphoreTake(g_lock, wait) == pdTRUE ? STORAGE_OK : STORAGE_ERR_BUSY;
}

static void unlock(void) {
  xSemaphoreGive(g_lock);
}

/**
 * Confirms [off, off + len) lies within the region, checking the addition for overflow before
 * the comparison so that a huge @p len cannot wrap into an apparently valid range.
 */
static storage_err_t bounds_check(const storage_region_t *r, uint32_t off, size_t len) {
  if (r == NULL) {
    return STORAGE_ERR_PARAM;
  }
  if (len > UINT32_MAX || off > UINT32_MAX - (uint32_t) len) {
    return STORAGE_ERR_BOUNDS;
  }
  if (off + (uint32_t) len > r->size) {
    return STORAGE_ERR_BOUNDS;
  }
  // Belt and braces: the handle itself must sit inside the reservation. A handle is only ever
  // produced by storage_region_open(), but a corrupted one is exactly the failure this layer
  // exists to stop reaching the program.
  if (r->base < STORAGE_BASE_OFFSET || r->size > STORAGE_RESERVE_BYTES ||
      r->base - STORAGE_BASE_OFFSET > STORAGE_RESERVE_BYTES - r->size) {
    return STORAGE_ERR_BOUNDS;
  }
  return STORAGE_OK;
}

/// Cached XIP pointer for a device offset. Reads through the flash cache.
static const uint8_t *xip_ptr(uint32_t device_offset) {
  return (const uint8_t *) (XIP_BASE + device_offset);
}

/// Uncached XIP pointer for a device offset. For verify-after-write, where the cache cannot be
/// trusted to reflect what is actually in the device.
static const uint8_t *xip_nocache_ptr(uint32_t device_offset) {
  return (const uint8_t *) (XIP_NOCACHE_NOALLOC_BASE + device_offset);
}

// --- flash_safe_execute callbacks ---

typedef struct {
  uint32_t device_offset;
} erase_job_t;

typedef struct {
  uint32_t       device_offset;
  const uint8_t *data;
} program_job_t;

static void do_erase_sector(void *param) {
  const erase_job_t *job = (const erase_job_t *) param;
  flash_range_erase(job->device_offset, FLASH_SECTOR_SIZE);
}

static void do_program_page(void *param) {
  const program_job_t *job = (const program_job_t *) param;
  flash_range_program(job->device_offset, job->data, FLASH_PAGE_SIZE);
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

storage_err_t storage_init(void) {
  if (g_lock == NULL) {
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == NULL) {
      printf("[storage] FATAL: could not create the storage mutex\n");
      return STORAGE_ERR_BUSY;
    }
  }

  g_program_end = (uint32_t) ((uintptr_t) &__flash_binary_end - XIP_BASE);

  // The one check that has to happen at runtime: the preprocessor does not know how big the
  // binary is. On the single-device layout this comparison is what stands between a storage
  // write and the program image, so a failure disarms every write path for the life of the boot.
  const uint32_t limit = STORAGE_BASE_OFFSET - STORAGE_PROGRAM_MARGIN_BYTES;
  if (g_program_end > limit) {
    g_armed = false;
    printf("[storage] FAULT: program ends at 0x%08lX, storage begins at 0x%08lX (margin %lu B);"
           " writes DISARMED\n",
           (unsigned long) g_program_end,
           (unsigned long) STORAGE_BASE_OFFSET,
           (unsigned long) STORAGE_PROGRAM_MARGIN_BYTES);
    return STORAGE_ERR_UNSAFE;
  }

  g_armed = true;
  return STORAGE_OK;
}

bool storage_is_armed(void) {
  return g_armed;
}

storage_layout_t storage_layout(void) {
  storage_layout_t out;
  out.device_bytes    = (uint32_t) PICO_FLASH_SIZE_BYTES;
  out.reserve_bytes   = STORAGE_RESERVE_BYTES;
  out.base_offset     = STORAGE_BASE_OFFSET;
  out.program_end     = g_program_end;
  out.allocated_bytes = STORAGE_REGIONS_END;
  out.armed           = g_armed;
  return out;
}

void storage_print_layout(void) {
  const storage_layout_t l = storage_layout();

  printf("[storage] device %lu KB; program ends 0x%08lX; nonvolatile half at 0x%08lX (%lu KB), %s\n",
         (unsigned long) (l.device_bytes / 1024u),
         (unsigned long) l.program_end,
         (unsigned long) l.base_offset,
         (unsigned long) (l.reserve_bytes / 1024u),
         l.armed ? "armed" : "DISARMED");

  for (int i = 0; i < REGION_COUNT; ++i) {
    printf("[storage]   %-8s  +0x%05lX  %5lu B  (device 0x%08lX)\n",
           k_regions[i].name,
           (unsigned long) k_regions[i].offset,
           (unsigned long) k_regions[i].size,
           (unsigned long) (STORAGE_BASE_OFFSET + k_regions[i].offset));
  }
  printf("[storage]   reserved  +0x%05lX  %lu KB unallocated\n",
         (unsigned long) STORAGE_REGIONS_END,
         (unsigned long) ((STORAGE_RESERVE_BYTES - STORAGE_REGIONS_END) / 1024u));
}

const char *storage_err_str(storage_err_t err) {
  switch (err) {
    case STORAGE_OK:            return "ok";
    case STORAGE_ERR_BOUNDS:    return "bounds";
    case STORAGE_ERR_ALIGN:     return "align";
    case STORAGE_ERR_BUSY:      return "busy";
    case STORAGE_ERR_VERIFY:    return "verify";
    case STORAGE_ERR_CORRUPT:   return "corrupt";
    case STORAGE_ERR_NOT_FOUND: return "not-found";
    case STORAGE_ERR_NO_SPACE:  return "no-space";
    case STORAGE_ERR_UNSAFE:    return "unsafe";
    case STORAGE_ERR_PARAM:     return "param";
  }
  return "?";
}


// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

storage_err_t storage_region_open(storage_region_id_t id, storage_region_t *out) {
  if (out == NULL || (int) id < 0 || id >= REGION_COUNT) {
    return STORAGE_ERR_PARAM;
  }
  out->base = STORAGE_BASE_OFFSET + k_regions[id].offset;
  out->size = k_regions[id].size;
  out->cs   = 0;
  return STORAGE_OK;
}

storage_err_t storage_region_read(const storage_region_t *r, uint32_t off, void *buf, size_t len) {
  if (buf == NULL) {
    return STORAGE_ERR_PARAM;
  }
  storage_err_t rc = bounds_check(r, off, len);
  if (rc != STORAGE_OK) {
    return rc;
  }
  if (len == 0) {
    return STORAGE_OK;
  }

  // Reads are serialised too. Not because the XIP window is unsafe to read concurrently — it is
  // not — but because a read that overlaps another task's erase of the same sector would return
  // half old and half 0xFF, and the layer above must be able to trust a read as a snapshot.
  rc = lock();
  if (rc != STORAGE_OK) {
    return rc;
  }
  memcpy(buf, xip_ptr(r->base + off), len);
  unlock();
  return STORAGE_OK;
}

storage_err_t storage_region_erase(const storage_region_t *r, uint32_t off, size_t len) {
  storage_err_t rc = bounds_check(r, off, len);
  if (rc != STORAGE_OK) {
    return rc;
  }
  if (!IS_SECTOR_ALIGNED(off) || !IS_SECTOR_ALIGNED(len)) {
    return STORAGE_ERR_ALIGN;
  }
  if (!g_armed) {
    return STORAGE_ERR_UNSAFE;
  }
  if (len == 0) {
    return STORAGE_OK;
  }

  rc = lock();
  if (rc != STORAGE_OK) {
    return rc;
  }

  // One sector per critical section (storage_api.md §6, rule 1). Interrupts come back between
  // sectors, so the tick catches up and the watchdog task can run. A multi-sector erase is
  // therefore several short blackouts rather than one long one.
  for (uint32_t done = 0; done < len && rc == STORAGE_OK; done += FLASH_SECTOR_SIZE) {
    erase_job_t job = { .device_offset = r->base + off + done };

    if (flash_safe_execute(do_erase_sector, &job, STORAGE_SAFE_EXECUTE_TIMEOUT_MS) != PICO_OK) {
      rc = STORAGE_ERR_UNSAFE;
      break;
    }

    // Verify through the uncached alias: an erased sector reads as all 0xFF.
    const uint8_t *p = xip_nocache_ptr(job.device_offset);
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; ++i) {
      if (p[i] != 0xFFu) {
        rc = STORAGE_ERR_VERIFY;
        break;
      }
    }
  }

  // The cached window may still hold the pre-erase contents.
  flash_flush_cache();
  unlock();
  return rc;
}

storage_err_t storage_region_write(const storage_region_t *r, uint32_t off, const void *buf, size_t len) {
  if (buf == NULL) {
    return STORAGE_ERR_PARAM;
  }
  storage_err_t rc = bounds_check(r, off, len);
  if (rc != STORAGE_OK) {
    return rc;
  }
  if ((off % FLASH_PAGE_SIZE) != 0u || (len % FLASH_PAGE_SIZE) != 0u) {
    return STORAGE_ERR_ALIGN;
  }
  if (!g_armed) {
    return STORAGE_ERR_UNSAFE;
  }
  if (len == 0) {
    return STORAGE_OK;
  }

  rc = lock();
  if (rc != STORAGE_OK) {
    return rc;
  }

  const uint8_t *src = (const uint8_t *) buf;

  // One page per critical section. A page program is on the order of a millisecond, so this is
  // cheap; the point of splitting is the same as for erase.
  for (uint32_t done = 0; done < len && rc == STORAGE_OK; done += FLASH_PAGE_SIZE) {
    program_job_t job = { .device_offset = r->base + off + done, .data = src + done };

    if (flash_safe_execute(do_program_page, &job, STORAGE_SAFE_EXECUTE_TIMEOUT_MS) != PICO_OK) {
      rc = STORAGE_ERR_UNSAFE;
      break;
    }

    // Verify through the uncached alias. This also catches a program into unerased space: the
    // device ANDs old and new bits, and the result does not match what was asked for.
    if (memcmp(xip_nocache_ptr(job.device_offset), job.data, FLASH_PAGE_SIZE) != 0) {
      rc = STORAGE_ERR_VERIFY;
      break;
    }
  }

  flash_flush_cache();
  unlock();
  return rc;
}
