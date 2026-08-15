# OwlSat Storage API — Internal Design

**Status:** Design. No code on this branch yet (`nonvolatile-data-storage`).
**Scope:** The non-volatile storage API the rest of OwlSatOS uses to keep small amounts of state
across power cycles — configuration and latched one-shot flags — held in the QSPI memory that
also holds the program, without ever writing over the program.

**This document supersedes the FAT12 + USB MSC plan** described under
*TASK: Implement the filesystem layer* in `README.md`. See [§10](#10-what-this-replaces).

---

## 0. The one assumption that colours everything

The flight design calls for **MRAM** on QSPI. As of the current hardware plan, the program and the
data live on **one MRAM part, wired to CS0** — not the two-part split the block diagram draws
(see [§2.2](#22-not-the-plan--a-separate-data-device-on-cs1)). **We have not confirmed that the
MRAM works, that it boots, or how to drive it.**

The working assumption, agreed with hardware and stated here so it is one line rather than folklore:

> **Software treats the part as behaving identically to a 2 MiB NOR flash of the kind fitted to a
> Pico.** Making that true at boot — stage 2, the read command set, XIP configuration — is
> hardware's problem to solve, and is tracked as such in [§12](#12-answers-needed).

So: **this API is specified against NOR flash semantics.** Erase-before-write, 4 KB erase
granularity, 256-byte program pages, bits only go 1→0 until erased, finite erase endurance.

Flash semantics are the *stricter* set. Anything written to satisfy them will also run correctly
on MRAM, which is byte-writable with no erase step. The reverse is not true — an MRAM-native
design (write anywhere, any size, any time) would silently corrupt data on flash. Building to the
strict contract now means the MRAM bring-up is a driver swap and a set of deletions, not a
redesign. [§9](#9-what-changes-when-the-mram-is-confirmed) lists exactly what falls away.

Where a number below comes from the flash assumption rather than from a measurement, it is marked
**[flash-assumption]**.

---

## 1. Layers

Two layers, and a deliberate refusal to build a third.

```
   clients:  deployment logic, config readers, console, telemetry
                 |                          |
                 v                          v
        +------------------+   +-------------------------+
        |  NVM key/value   |   |  latched one-shot flags  |   <- layer 2
        |  (A/B banked)    |   |  (write-once pages)      |
        +------------------+   +-------------------------+
                 |                          |
                 +------------+-------------+
                              v
                  +------------------------+
                  |  region layer          |                  <- layer 1
                  |  named, bounds-checked |
                  |  spans of QSPI         |
                  +------------------------+
                              |
                              v
                  hardware_flash / pico_flash (Pico SDK)
```

There is no filesystem. Nothing here allocates. Nothing here has a path parser, a directory, or a
free-list. Every region is fixed at compile time, and the total RAM cost is known at link time.
That is the point: this is the layer that has to work when everything else is broken, and a
storage stack whose failure modes you cannot enumerate on one page is not that layer.

---

## 2. Address model

### 2.1 One QSPI device on CS0, shared with the program

This is the layout on the bench **and** the layout in flight. It is not a stopgap.

The program and the storage regions share one device. The program is at the bottom, growing up;
storage is pinned to the **top**, growing nowhere.

```
 XIP_BASE = 0x10000000
   +--------------------------------------------------+ 0x000000
   |  program image (XIP, read-only at runtime)        |
   |                                                   |
   +--------------------------------------------------+ __flash_binary_end
   |                                                   |
   |  slack — firmware-update staging (§8)             |
   |                                                   |
   +--------------------------------------------------+ STORAGE_BASE_OFFSET
   |  storage regions (1024 KB)                        |
   +--------------------------------------------------+ PICO_FLASH_SIZE_BYTES
```

The reservation is a fixed **1024 KB**. The offset it begins at is not fixed — it follows the
device, because the dev board and the flight part are not the same size:

```c
#define STORAGE_RESERVE_BYTES  (1024u * 1024u)
#define STORAGE_BASE_OFFSET    (PICO_FLASH_SIZE_BYTES - STORAGE_RESERVE_BYTES)
```

| Device | `PICO_FLASH_SIZE_BYTES` | `STORAGE_BASE_OFFSET` | Program + staging slack |
|---|---|---|---|
| Pico 2 dev board | 4 MB | `0x300000` | 3072 KB |
| Flight part (2 MiB) | 2 MiB | `0x100000` | 1024 KB |

Deriving the base rather than writing it down is the point: a hard-coded `0x3C0000` is correct on
exactly one device and silently wrong on every other, and "silently wrong" here means storage
overlapping the program. The one number that must be written down is the reservation size, and it
is written down once.

Anchoring to the *top* rather than to `__flash_binary_end` is the whole safety argument. The
binary end moves every single build. If storage started there, every firmware update would
relocate the data underneath itself and every stored value would be lost — or worse, read back at
the wrong offset and interpreted as valid. A fixed top-anchored base is stable across builds, so
data survives a firmware update, which is a hard requirement for a satellite we cannot physically
reach.

Verified SDK facts this rests on:

| Constant | Value | Source |
|---|---|---|
| `XIP_BASE` | `0x10000000` | `hardware/regs/addressmap.h:24` |
| `PICO_FLASH_SIZE_BYTES` | `4 * 1024 * 1024` | `boards/pico2.h:80` — **dev board only**; flight overrides this |
| `FLASH_SECTOR_SIZE` | `4096` (erase unit) | `hardware/flash.h:46` |
| `FLASH_PAGE_SIZE` | `256` (program unit) | `hardware/flash.h:45` |
| `__flash_binary_end` | linker symbol | `rp2350/memmap_default.ld:272` (`PROVIDE`d) |
| `XIP_NOCACHE_NOALLOC_BASE` | `0x14000000` | `hardware/regs/addressmap.h:27` |

**Offsets passed to the SDK are relative to `XIP_BASE`, not absolute pointers.**
`flash_range_erase(0x300000, …)` — not `0x10300000`. Reads go the other way: through
`XIP_BASE + offset` as a normal `const` pointer. Mixing these up is the single easiest way to
erase the program, so the region layer never lets a client see a raw offset at all
([§4](#4-region-layer)).

### 2.2 Not the plan — a separate data device on CS1

Recorded so it is not proposed again as though it were new, and so the cost of *not* doing it is
written down somewhere.

The block diagram draws two MRAM parts, code and data. Earlier revisions of this document treated
a two-chip-select layout as the configuration to migrate toward:

```
  CS0   0x10000000  +------------------+   code MRAM — XIP, program only
  CS1   0x11000000  +------------------+   data MRAM — storage regions only
```

The RP2350 can do this. Its QMI has **two chip selects and two independently-configured address
windows** (`qmi_hw_t.m[2]`, `hardware/structs/qmi.h:111`), with eight 4 MiB address-translation
registers covering a 32 MiB virtual XIP space (`atrans[8]`, `qmi.h:118`), and the SDK's flash API
takes a chip select index throughout — `flash_devinfo_get_cs_size(uint cs)`, where "0 is QMI chip
select 0 (QSPI CS pin), 1 is QMI chip select 1" (`hardware/flash.h:197`).

**Hardware's current plan is one MRAM part on CS0 carrying both program and data.** So this does
not happen, and §2.1 is the layout, permanently.

#### What that costs us

The two-device layout was attractive for one reason, and it is worth being blunt about losing it:
it made the central safety property **structural rather than arithmetic**. With storage on a
different chip select, the write path *physically cannot address the program* — no bug, no
miscomputed offset, no corrupted region table can reach it.

On one shared device, "don't overwrite the program" goes back to being an offset comparison that
code has to get right, every time, forever. That comparison is
[§4.2](#42-the-checks) — the `STORAGE_BASE_OFFSET >= __flash_binary_end` runtime check and the
bounds check on every call. Those were written as the *interim* guard for a bench setup. They are
now the **only** thing standing between a storage bug and an unbootable satellite, for the life of
the mission.

Two consequences follow, and neither is optional:

- §4.2's checks get reviewed as flight-critical code, not as scaffolding.
- The `cs` field stays in `storage_region_t`, still hard-coded to `0`. It costs a byte and it
  keeps this section cheap to reverse if hardware revisits the split.

> **If the split is ever revived**, these were the open items, and they were never settled: the
> CS1 window base of `0x11000000` is per the RP2350 datasheet and unconfirmed in this repo; and it
> is unknown whether `flash_range_erase`/`flash_range_program` drive CS1 correctly, since the
> bootrom bounds-checks against the `FLASH_DEVINFO` OTP entry and only issues an XIP exit to CS1
> when its recorded size is non-zero — which may require `flash_devinfo_set_cs_size(1, …)` at boot.

---

## 3. Region map

Sizes are generous relative to what we store. Space at the top of the device is not the scarce
resource; erase cycles and the ability to change our minds later are.

| Region | Offset (from `STORAGE_BASE_OFFSET`) | Size | Purpose |
|---|---|---|---|
| `REGION_CONFIG_A` | `0x00000` | 16 KB | K/V bank A |
| `REGION_CONFIG_B` | `0x04000` | 16 KB | K/V bank B |
| `REGION_LATCH` | `0x08000` | 4 KB | Write-once latched flags |
| `REGION_SCRATCH` | `0x09000` | 4 KB | Firmware-update staging metadata |
| — reserved — | `0x0A000` | 984 KB | Unallocated. Do not use without updating this table. |

Note what growing the reservation to 1024 KB did and did not do. It did **not** give the K/V store
more room: one bank is live at a time, so config capacity is still one 16 KB bank minus a 32-byte
header — about **16 KB of payload** — and the latch region still holds exactly **16 flags**, one
per 256 B page. All 768 KB of the increase landed in the unallocated reserve.

That is deliberate. The regions are sized by what they hold, not by what is available, and the
reserve is the part that is *supposed* to be big: it is where the bulk-telemetry sink that §10
flags as a real gap would go, and sizing that region now — before its write rate and ring-buffer
semantics are designed — would be guessing. Keeping it unallocated keeps it honest.

Every region is sector-aligned and a whole number of sectors, because 4 KB is the erase unit and a
region that shares a sector with its neighbour cannot be erased without destroying the neighbour.
This is a static assertion, not a convention ([§4.2](#42-the-checks)).

---

## 4. Region layer

### 4.1 Interface

```c
/**
 * @file storage.h
 * @brief Bounds-checked named spans of the QSPI device.
 */

/** @brief Compile-time identity of a reserved span. */
typedef enum {
  REGION_CONFIG_A,
  REGION_CONFIG_B,
  REGION_LATCH,
  REGION_SCRATCH,
  REGION_COUNT
} storage_region_id_t;

typedef enum {
  STORAGE_OK = 0,
  STORAGE_ERR_BOUNDS,      ///< Access fell outside the region.
  STORAGE_ERR_ALIGN,       ///< Offset or length violated page/sector alignment.
  STORAGE_ERR_BUSY,        ///< Could not take the storage mutex before timeout.
  STORAGE_ERR_VERIFY,      ///< Read-back after program did not match.
  STORAGE_ERR_CORRUPT,     ///< Header magic or CRC rejected.
  STORAGE_ERR_NOT_FOUND,   ///< No such key.
  STORAGE_ERR_NO_SPACE,    ///< Value would not fit in the bank.
  STORAGE_ERR_UNSAFE,      ///< flash_safe_execute could not guarantee safety.
} storage_err_t;

/** @brief Opaque handle. Holds resolved base/size/cs; never a raw pointer. */
typedef struct { uint32_t base; uint32_t size; uint8_t cs; } storage_region_t;

storage_err_t storage_init(void);
storage_err_t storage_region_open(storage_region_id_t id, storage_region_t *out);

/** @brief Read from a region. Offsets are region-relative. No alignment requirement. */
storage_err_t storage_region_read(const storage_region_t *r, uint32_t off,
                                  void *buf, size_t len);

/** @brief Erase whole sectors. @p off and @p len must be FLASH_SECTOR_SIZE multiples. */
storage_err_t storage_region_erase(const storage_region_t *r, uint32_t off, size_t len);

/**
 * @brief Program into already-erased space. @p off and @p len must be
 *        FLASH_PAGE_SIZE multiples. Does NOT erase first — flash bits only go 1->0.
 * @warning Blocks with interrupts disabled. See §6 before calling.
 */
storage_err_t storage_region_write(const storage_region_t *r, uint32_t off,
                                   const void *buf, size_t len);
```

`storage_region_write` deliberately does not erase. A `write()` that silently erases the
surrounding sector is how you lose the three other values that shared it. Callers that want
read-modify-write ask for it explicitly, at the layer above.

### 4.2 The checks

Enforced at **compile time**, in `static_assert`:

- Every region is `FLASH_SECTOR_SIZE`-aligned in both base and size.
- No two regions overlap.
- The last region ends at or before `PICO_FLASH_SIZE_BYTES`.
- `STORAGE_BASE_OFFSET` is sector-aligned.
- **`PICO_FLASH_SIZE_BYTES > STORAGE_RESERVE_BYTES`**, with margin. Now that the base offset is
  derived by subtraction ([§2.1](#21-one-qspi-device-on-cs0-shared-with-the-program)), a
  device smaller than the reservation does not produce an error — it produces an *unsigned
  wraparound*, and `STORAGE_BASE_OFFSET` lands somewhere near the top of the address space or, at
  exactly equal sizes, at `0`, directly on top of the program. Neither is caught by any of the
  checks above, because both are internally consistent. This assertion is what makes the
  subtraction safe to write.

Enforced at **`storage_init()`**, once, at boot, before the scheduler starts:

- `STORAGE_BASE_OFFSET >= (uintptr_t)&__flash_binary_end - XIP_BASE`, plus a slack margin.
  This is the "don't overwrite the program" check, and it is the only one that has to be a runtime
  check, because the binary size is not known to the preprocessor. Declare the symbol as
  `extern char __flash_binary_end;` and take its address.
- On failure: refuse to arm any write path, latch a fault, and shout on the console. Do **not**
  `configASSERT` — that spins forever with interrupts off (`FreeRTOSConfig.h:45`), and a satellite
  that cannot save config is still a satellite that should fly. A satellite in a hard spin is not.

Enforced on **every call**: `off + len` within `r->size`, with the addition checked for overflow
before the comparison.

---

## 5. Layer 2a — the key/value store

### 5.1 Shape

Two banks, A and B, of 16 KB each. Exactly one is live at a time.

```
bank header (32 B)                    entries, packed, 4-byte aligned
+-------------------------------+     +------+------+---------+ +------+---
| magic | ver | seq | count     |     | key  | len  | value.. | | key  | ...
| len   | crc32 | reserved      |     | u16  | u16  |         | | u16  |
+-------------------------------+     +------+------+---------+ +------+---
```

- `magic` — rejects an erased or foreign bank immediately.
- `seq` — `uint32_t`, incremented per commit. Compared as a **signed difference**
  (`(int32_t)(a - b) > 0`) so wraparound is a non-event rather than a mission-ending regression.
- `crc32` — over header-minus-crc plus the entry payload. Covers the torn-write case.

Keys are a fixed `uint16_t` enum, not strings. No parser, no allocation, no length limit to get
wrong, and a typo is a compile error rather than a value that silently reads back as absent.

### 5.2 Interface

```c
storage_err_t nvm_get(nvm_key_t key, void *out, size_t len);
storage_err_t nvm_set(nvm_key_t key, const void *val, size_t len);
storage_err_t nvm_commit(void);      ///< The only call that touches the device.
```

### 5.3 Why get/set are RAM-only and commit is explicit

At `storage_init()`, both bank headers are read, validated, and the live bank's entries are
mirrored into a small statically-allocated RAM cache.

- `nvm_get` reads the RAM cache. No QSPI access. It cannot block, cannot fail transiently, and
  costs a memcpy — so any task, at any priority, can read config on its hot path.
- `nvm_set` updates the RAM cache and sets a dirty flag. Still no QSPI access.
- `nvm_commit` writes the **inactive** bank, in full, then that bank becomes live by virtue of
  having the higher `seq`.

Splitting `set` from `commit` is not ceremony. Writing to flash means erasing a 16 KB bank with
interrupts disabled for a long time ([§6](#6-the-real-cost-of-a-write)). If every `nvm_set` did
that, a task updating five config values would stall the system five times over. Batching makes
the expensive, dangerous operation happen once, at a moment the caller chose.

### 5.4 Power-loss atomicity

The commit sequence is:

1. Erase the inactive bank.
2. Program entries, then the header **last**, with `seq = live_seq + 1`.
3. Flush the XIP cache.

Cut power at any point and the result is still correct. The header is written last, so a
half-written bank has no valid magic/CRC and loses to the intact one on the next boot. There is no
window in which both banks are invalid, and no window in which the new bank wins before it is
complete. This is why the header goes last, and it is the only ordering constraint in the design
that must not be relaxed for performance.

### 5.5 Wear

**[flash-assumption]** ~100 k erase cycles per sector. One commit erases one bank. At a
deliberately pessimistic 10 commits/day, a single bank sees ~7300 cycles in two years — under 10 %
of budget, and the two banks alternate, halving it again. Wear levelling is therefore **not**
implemented, and should not be added speculatively. If the commit rate ever rises to where it
matters, that is a signal that the caller is misusing config storage for telemetry.

---

## 6. The real cost of a write

This is the section to read before adding a `nvm_commit()` anywhere.

**During any erase or program, XIP is disabled.** Code cannot be fetched from QSPI. The SDK's
flash routines are RAM-resident, but every FreeRTOS ISR — SysTick, PendSV — and every callback
they reach lives in flash. If one fires mid-operation, the CPU fetches from a device that is busy
answering an erase command, and the firmware dies. So **interrupts are disabled for the whole
operation**, via `save_and_disable_interrupts()` / `restore_interrupts()`.

**[flash-assumption]** A 4 KB sector erase on typical QSPI NOR is ~45 ms typical, **up to 400 ms
worst case**. Confirm against the datasheet for the part actually fitted.

Now put that next to two facts already established about this system:

- `configTICK_RATE_HZ` is **1000** (`FreeRTOSConfig.h:20`). A 400 ms blackout drops ~400 ticks.
  Every `vTaskDelay` in the system finishes late by that much.
- The external watchdog is kicked by a **task**, not a timer — a deliberate choice recorded in
  `hardware_block_diagram.md` §2, precisely so that a wedged CPU fails to kick. A long
  interrupts-off window is indistinguishable, from the watchdog's point of view, from the wedge it
  exists to catch. **A careless commit can reset the satellite.**

The rules that follow:

1. **Erase one sector per critical section.** Never hold interrupts off across a multi-sector
   erase. Re-enable between sectors, let the tick catch up, give the watchdog task a chance to run.
2. **Budget against the watchdog timeout**, not against a feel for "fast enough". Worst-case single
   sector erase must sit comfortably inside it, with the margin written down here once measured.
3. **Never write from an ISR.** Not from a timer callback, not from `xTimerPendFunctionCall`.
4. **All writes go through one task**, or at minimum one FreeRTOS mutex
   (`xSemaphoreCreateMutex`) taken with a finite timeout returning `STORAGE_ERR_BUSY`. Two
   concurrent erases on one device corrupt both.
5. **Prefer program-only updates.** Programming a page is ~1 ms rather than tens; the latched-flag
   region ([§7](#7-layer-2b--latched-one-shot-flags)) never erases at all, by design.

### 6.1 `flash_safe_execute` and this port

The SDK ships `flash_safe_execute()` (`pico_flash`) to make the *other* core safe during flash
operations. This project runs the **single-core, non-SMP** `RP2350_ARM_NTZ` FreeRTOS port —
`FreeRTOSConfig.h` sets no `configNUMBER_OF_CORES`, and core 1 is never started.

So the correct configuration is `PICO_FLASH_ASSUME_CORE1_SAFE=1`, and `pico_flash` must be added
to `target_link_libraries` in `CMakeLists.txt` alongside `pico_stdlib` and `hardware_flash`.
Routing through `flash_safe_execute` anyway — rather than calling the flash functions bare — costs
nothing and means the day someone starts core 1 for the science chain, this code does not quietly
become the bug.

### 6.2 Cache coherency

After any erase or program, call `flash_flush_cache()` (`hardware/flash.h:148`) before reading the
affected range through `XIP_BASE`. The XIP cache will otherwise serve stale data — a failure that
looks exactly like a write that silently did nothing, and wastes an afternoon. Alternatively read
through `XIP_NOCACHE_NOALLOC_BASE` (`0x14000000`), which is the right choice inside verify-after-
write, where trusting the cache defeats the purpose of verifying.

---

## 7. Layer 2b — latched one-shot flags

The block diagram is explicit (`hardware_block_diagram.md` §6): initial antenna deployment
*"must set a key in NVM recording that the antennae have already been deployed, so the burn wires
are never triggered a second time."*

**This must not live in the K/V store**, and the reason is worth stating plainly. The K/V store's
safety property is that a bad commit *falls back to the previous bank*. For config, that is
exactly right. For "have we already fired the burn wires", falling back to the previous state
means reverting to `not deployed` — and firing burn wires a second time on an already-deployed
satellite, draining the battery into a resistor for nothing. The K/V store's best feature is this
flag's worst failure.

So latched flags get a region with the opposite property: **write-once, never erased in flight.**

```
REGION_LATCH — 4 KB = 16 pages of 256 B, one page per flag

  page 0  LATCH_ANTENNA_DEPLOYED
  page 1  LATCH_FIRST_BOOT_DONE
  page 2..15  reserved

  page content:  0xFF.. (erased)  => not set
                 magic + key + crc => set
```

- Setting a flag programs one page. **No erase.** Bits go 1→0, which is the direction flash gives
  for free.
- Clearing is not in the API. There is no `latch_clear()`. Ground can clear one only via an
  explicit maintenance command that erases the whole region, and that command should be hard to
  issue by accident.
- A torn write leaves a page failing its CRC. Treat that as **set** — the conservative reading.
  Not deploying an already-deployed antenna is a no-op; deploying an already-deployed one is not.

```c
bool          latch_is_set(latch_id_t id);   ///< Cheap. Cached in RAM at init.
storage_err_t latch_set(latch_id_t id);      ///< Programs one page. Idempotent.
```

---

## 8. Interaction with firmware update and reflashing

| Path | Effect on storage regions |
|---|---|
| UF2 drag-and-drop | **Survives.** A UF2 rewrites only the sectors it contains; the program image does not extend to `STORAGE_BASE_OFFSET`. |
| `picotool load` | **Survives**, same reason. |
| `picotool erase` / flash nuke | **Destroyed.** Whole-device erase. Expect to reprovision config afterwards. |
| Debugger flash algorithm | **Usually destroyed** — most default to full-chip erase. Check the probe config before assuming a bench test proves persistence. |

The in-flight firmware update path (`README.md`, *TASK: Deal with update requests*) stages an image
and must **not** stage it into the storage regions. `REGION_SCRATCH` is for update *metadata* —
sequence, length, CRC, state — not for the image payload. The image goes in the slack between
`__flash_binary_end` and `STORAGE_BASE_OFFSET`, which is where the top-anchored reservation earns
its keep: staging cannot reach the config, and config cannot reach the staged image.

### 8.1 The image-size budget this imposes

On the 2 MiB flight part, a 1024 KB reservation splits the device exactly in half, and the running
program and the staged image must both fit in the lower half:

```
  program image  +  staged image  <=  STORAGE_BASE_OFFSET  =  1024 KB
```

So the practical ceiling on firmware size is **~512 KB**, and it is a ceiling on the *running*
image, not on the update — the two are the same size in a like-for-like update. There is a lot of
room against it today: the current release build is 25,480 bytes, about 5 % of budget.

This is the number to watch, and it is the reason the reservation is worth revisiting rather than
treating as settled. Storage is not consuming 1024 KB — it is allocating 40 KB and holding 984 KB
in reserve ([§3](#3-region-map)) — so if the image ever approaches the ceiling, the cheapest fix is
to shrink the reservation, not to fight the linker. Trading reserve for image headroom is a
one-constant change *only if* nothing has quietly started using the reserve, which is the real
reason [§3](#3-region-map) insists the reserve stay unallocated.

---

## 9. What changes when the MRAM is confirmed

Kept deliberately short, because it is the checklist for that bring-up.

| Concern | Flash (assumed now) | MRAM (expected) |
|---|---|---|
| Erase | Required, 4 KB granularity | None — byte-writable in place |
| Program granularity | 256 B pages | Arbitrary |
| Write latency | ~45 ms/sector erase, up to 400 ms | Sub-microsecond, bus-limited |
| Endurance | ~100 k cycles | Effectively unlimited |
| Interrupts-off window | The dominant design constraint (§6) | Largely evaporates |
| A/B banking | Needed for atomicity **and** wear | Still needed for **atomicity** |

The critical line is the last one. It is tempting to conclude that MRAM makes the A/B scheme
unnecessary. It does not. MRAM removes the *wear* argument and the *latency* argument; it does not
make a multi-byte update atomic against power loss mid-write. Power can still be cut between byte
3 and byte 4 of a value. Keep the banking, keep the header-last ordering, delete the erase calls
and the wear discussion.

Bring-up questions to answer on hardware, in order. Note that (1) is new and now dominates: with
program and data on the same part ([§2.1](#21-one-qspi-device-on-cs0-shared-with-the-program)),
the device has to be bootable, not merely readable and writable. A part that stores data
beautifully but cannot be executed from is not a usable part under this plan.

1. **Does the RP2350 boot from it?** Does the bootrom's XIP setup and boot stage 2 bring the part
   up as executable memory on CS0 — and if not, what does stage 2 need to become? Everything else
   on this list is moot until this is yes.
2. Does the part respond to standard `0x9F` JEDEC ID over QMI at all?
3. Does it accept `0x02` page program and `0x03`/`0xEB` read like NOR, or does it need a
   different command set — i.e. can `hardware_flash` drive it, or does this need a QMI-level driver?
4. Does erase (`0x20`) exist as a no-op, an error, or an unsupported command?

Until (1)–(4) are answered, nothing in this document changes.

---

## 10. What this replaces

`README.md` describes *TASK: Implement the filesystem layer (`storage` branch)* — a FAT12 volume
with a USB MSC stack in `msc_disk.c`, `openFile`/`closeFile`/`readFile`/`writeFile`, and a
`FILE_STRUCTURE` to fill in. **That approach is superseded by this document.** Note also that no
`storage.cpp`/`storage.h` exists on this branch; hits for those names in `.cache/clangd` are a
stale index, not code.

The reasoning, so it is not relitigated later:

- **FAT12 is a ground-convenience format, not a flight format.** Its value is that a laptop can
  mount it over USB MSC. In flight there is no laptop and no USB host, so the entire benefit is
  unavailable exactly when the storage matters.
- **It has no power-loss story.** A FAT update is several non-atomic writes across the FAT, the
  directory entry and the data area. Power loss between them yields a cross-linked or
  half-extended file. The A/B bank scheme here is atomic by construction (§5.4).
- **It carries no wear or latency model.** Nothing in the FAT12 design accounts for a 400 ms
  interrupts-off erase against a task-driven watchdog (§6) — the constraint that actually governs
  this system.
- **It is far more machinery than the requirement needs.** The real requirement is a few hundred
  bytes of config and a handful of latched flags. That is a K/V store, not a filesystem.

Migration notes:

- Nothing to migrate. No flight data exists in FAT12 form.
- The USB MSC stack (`msc_disk.c`, TinyUSB config) can be kept as a **ground-test-only** debug
  path if someone wants it, but it must not be the flight persistence path and must not share
  regions with the storage layer. Simplest is to leave it on the `storage` branch, unmerged.
- The **sensor/gyro logging task** (`README.md`, *TASK: Parse and store sensor/gyro data*) listed
  FAT12 day-files as its sink and now has none. That is a real gap, and out of scope here:
  bulk telemetry logging is a different problem with different constraints (high write rate,
  ring-buffer semantics, tolerance for losing the oldest data) and deserves its own design rather
  than being bolted onto a config store. Flag it; do not let it quietly become `nvm_set`.
- `README.md`'s task list should be updated to match this document.

**Naming:** this document uses Pico SDK `snake_case` (`storage_region_open`) rather than the
`camelCase` (`openFile`) of the superseded FAT12 sketch, on the grounds that this layer sits
directly on SDK calls and should read continuously with them. It is a deliberate break from
`consoleTask` and worth a decision, not a drift.

---

## 11. Open questions

Carried explicitly, in the style of the hardware document, so firmware assumptions do not outrun
them. These are the *design's own* uncertainties. For the inputs this document cannot supply
itself — things someone has to go and decide or measure — see [§12](#12-answers-needed).

- **What is the fitted flash part's worst-case sector erase time**, and what is the external
  watchdog timeout? Until both numbers exist, the §6 latency budget is an assumption, not a design.
- ~~Is the CS1 window really at `0x11000000`, and can `hardware_flash` drive CS1?~~ Moot under the
  one-device plan; retained in [§2.2](#22-not-the-plan--a-separate-data-device-on-cs1) in case the
  split is revived.
- **The drawing still says the wrong device size, and needs redrawing.** This document is sized
  against a **2 MiB** part; `OwlSat Hardware block diagram.png` shows **2 Mb** — 2 mega*bit*,
  256 KiB, an eightfold understatement. The transcription in `hardware_block_diagram.md` §2
  carries a correction note rather than a silent edit, because that file's job is to say what the
  image says. Until the image is fixed, anyone reading it alone will size firmware against a
  device eight times too small. Not a design question — a task.
- Is 1024 KB the right reservation? It is biased toward "we will want room later", and §3 is
  candid that 984 KB of it is unallocated. The two things most likely to move it are the
  firmware-update staging budget (§8.1) and the bulk-telemetry sink that §10 leaves unowned.
- Should `REGION_SCRATCH` be readable by ground for post-mortem after a failed update?
- Does the RTC (`hardware_block_diagram.md` §2) need a region for drift calibration, or does that
  fit as K/V keys? Probably keys.
- RP2350 supports **partition tables** in the boot image (picobin/picotool). Should the storage
  reservation be declared there rather than as a bare offset constant, so `picotool` itself knows
  not to touch it? This would upgrade the §4.2 runtime check into something the tooling enforces.

---

## 12. Answers needed

Distinct from [§11](#11-open-questions): those are design choices still to be made. These are
**inputs from outside the document** — numbers to measure, decisions to take — that specific
pieces of work are waiting on. Each says what it blocks, so it is clear what unsticks when it is
answered.

### A. The telemetry sink

[§10](#10-what-this-replaces) removed the sensor/gyro logging task's storage and deliberately did
not replace it. Writing that design needs five numbers, and none of them are firmware's to invent:

1. **What is the sample rate, per stream, in Hz?** The single most load-bearing number here. It
   decides append-only vs. a true ring buffer, how much of the §3 reserve the region takes, the
   erase-scheduling strategy, and whether wear is a non-issue or a budget.
2. **How many streams, and are they one record or several?** The 24 B figure discussed so far
   looks like the 6-DOF IMU. Magnetometer, EUV ADC and the power monitors are separate sources at
   plausibly different rates — one interleaved log or several regions is a structural choice, not
   a detail.
3. **Is the 24 B record fixed, or may it become 32 B?** 24 B does not divide the 4 KB erase unit;
   32 B gives 128 records per sector exactly and leaves 8 B for a timestamp delta, sequence and
   CRC. Trading ~25 % of capacity for self-describing, individually verifiable records is a good
   deal, but it is a deal someone has to agree to.
4. **How often does ground drain the log?** With (1), this sizes the region: the log must hold at
   least one downlink interval, plus margin for missed passes.
5. **When the log is full, is losing the oldest data acceptable?** Overwrite-oldest (ring) and
   stop-and-preserve (fill-and-stop) are different designs and cannot both be built. For attitude
   telemetry the answer is usually ring; for anything feeding an anomaly investigation it usually
   is not.

### B. Booting from the MRAM — hardware's to answer

Now that program and data share one part ([§2.1](#21-one-qspi-device-on-cs0-shared-with-the-program)),
the device is on the boot path, and this outranks everything else on this page.

6. **Can the RP2350 boot from the fitted MRAM on CS0, and what does it take?** Stage 2, the read
   command set, XIP configuration. Software's standing assumption is that the part behaves like a
   2 MiB Pico-style NOR flash ([§0](#0-the-one-assumption-that-colours-everything)); this question
   is the request to make that assumption true, or to tell us precisely how it is false. If the
   answer is "it does not boot", the plan changes rather than the code — so an early *no* is worth
   far more than a late *yes*.

### C. The latency budget

7. **What is the external watchdog timeout, in milliseconds?** [§6](#6-the-real-cost-of-a-write)
   instructs the reader to budget erase time against it and then cannot supply it. Until this
   number exists, "erase one sector per critical section" is a rule with no margin attached, and
   *no* write path — config included, not just telemetry — can be signed off as safe.
8. **Which flash/MRAM part is fitted, and what is its datasheet worst-case sector erase?** Also
   carried in §11. Repeated here because it gates the same sign-off as (7): the ~45 ms typical /
   400 ms worst case in §6 is marked **[flash-assumption]** and has never been checked against a
   real part.

### D. Build plumbing

9. **How does `PICO_FLASH_SIZE_BYTES` get set for the flight board — a custom board header, or a
   CMake define?** [§2.1](#21-one-qspi-device-on-cs0-shared-with-the-program) derives
   `STORAGE_BASE_OFFSET` from it, and nothing overrides it today, so a build right now silently
   uses the dev board's 4 MB and places storage at `0x300000`. Harmless on the bench, wrong on
   flight hardware, and invisible either way.
