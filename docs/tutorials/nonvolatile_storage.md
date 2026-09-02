# Tutorial: nonvolatile storage {#tutorial_nonvolatile_storage}

**Branch:** `nonvolatile-data-storage`.
**Status: design only. There is no storage code on this branch yet.** The API below is the one
specified in `docs/internal/storage_api.md`; this page is written against it so that the
implementation, when it lands, has a usage contract to be checked against. Where the tutorial
says "will", that is a statement about the design, not the code.

**Reader:** someone who has to save a config value, latch a one-shot flag, implement the layer,
or work out why a commit reset the satellite.

---

## 1. What this is, and is not

A key/value store and a set of latched flags, held in the top 1024 KB of the QSPI device that
also holds the program. **There is no filesystem.** The earlier FAT12 + USB mass-storage plan is
superseded; see `storage_api.md` §10 for why.

```
   clients:  deployment logic, config readers, console, telemetry
                 |                          |
        +------------------+   +-------------------------+
        |  NVM key/value   |   |  latched one-shot flags  |   layer 2   nvm_*, latch_*
        |  (A/B banked)    |   |  (write-once pages)      |
        +------------------+   +-------------------------+
                 +------------+-------------+
                  +------------------------+
                  |  region layer          |                  layer 1   storage_region_*
                  |  named, bounds-checked |
                  +------------------------+
                  hardware_flash / pico_flash (Pico SDK)
```

What it holds: a few hundred bytes of configuration and a handful of flags. What it does **not**
hold: bulk telemetry. The sensor/gyro log has no sink on this design and needs its own
(`storage_api.md` §12.A). Do not let it quietly become `nvm_set()`.

---

## 2. The one assumption to know before anything else

The flight part is MRAM on CS0, sharing the device with the program. **Hardware has not yet
confirmed that the RP2350 boots from it.** Until it does, software assumes the part behaves
exactly like a 2 MiB Pico-style NOR flash: erase before write, 4 KB erase sectors, 256 B program
pages, bits only go 1→0, finite erase endurance.

Flash semantics are the stricter set, so code written to them runs correctly on MRAM. The reverse
is not true. **Build to the flash contract.** When the MRAM is confirmed, the erase calls and the
wear discussion are deleted and everything else stays (`storage_api.md` §9).

---

## 3. Saving and reading configuration

Keys are a `uint16_t` enum, not strings. Values are fixed-size blobs the caller owns the layout of.

```c
#include "storage.h"   // region layer, storage_init()
#include "nvm.h"       // nvm_get / nvm_set / nvm_commit

// At boot, before the scheduler, after stdio:
storage_err_t rc = storage_init();
if (rc != STORAGE_OK) {
  // Refuse to arm writes, latch a fault, shout on the console. Do NOT configASSERT.
  // A satellite that cannot save config is still a satellite that should fly.
}

// Reading, from any task at any priority — RAM cache only, cannot block:
uint32_t period_ms;
if (nvm_get(NVM_KEY_SENSOR_PERIOD_MS, &period_ms, sizeof period_ms) == STORAGE_ERR_NOT_FOUND) {
  period_ms = OWLSAT_SENSOR_PERIOD_MS;   // compile-time default
}

// Writing, from a task — RAM cache only, sets a dirty flag:
nvm_set(NVM_KEY_SENSOR_PERIOD_MS, &period_ms, sizeof period_ms);
nvm_set(NVM_KEY_TX_BURST_LIMIT,   &burst,     sizeof burst);

// Committing — the ONLY call that touches the device. See §5 before adding one.
rc = nvm_commit();
```

Why `set` and `commit` are separate: a commit erases and rewrites a 16 KB bank with interrupts
off. Five `nvm_set()` calls that each committed would stall the system five times. Batch your
changes and commit once, at a moment you chose.

Power-loss safety is by construction: the commit writes the *inactive* bank in full, header last,
with a higher sequence number. Cut power at any point and the next boot picks the intact bank.
Do not "optimise" the header ordering.

---

## 4. Latched one-shot flags

For facts that must never revert: "the burn wires have fired", "first boot is done".

```c
#include "latch.h"

if (!latch_is_set(LATCH_ANTENNA_DEPLOYED)) {
  FireBurnWires();
  latch_set(LATCH_ANTENNA_DEPLOYED);     // programs one 256 B page, never erases
}
```

Why these are **not** K/V keys: the K/V store's safety property is that a bad commit falls back to
the previous bank. For config that is right. For "have we already deployed", falling back means
reverting to *not deployed* and firing the burn wires a second time into an already-deployed
antenna. The K/V store's best feature is this flag's worst failure.

Properties:

- Setting a flag is idempotent and costs a single page program, ~1 ms, no erase.
- **There is no `latch_clear()`.** Ground can clear the region only through an explicit
  maintenance command that erases all of it.
- A torn write reads as *set*. Not deploying an already-deployed antenna is a no-op; the other
  direction is not.
- Sixteen flags fit the region, one per page. Two are assigned. Add new ones to the enum and the
  table in `storage_api.md` §7.

---

## 5. Read before adding `nvm_commit()` anywhere

During an erase or program, **XIP is off and interrupts are disabled** for the whole operation.
Code cannot be fetched from QSPI, so SysTick and PendSV cannot run. A 4 KB sector erase is ~45 ms
typical and up to **400 ms** worst case on typical NOR [flash-assumption].

The external watchdog on this board is kicked by a *task*, and it is gated on check-ins from the
sensor, link and transmit tasks (`docs/tutorials/flight_tasks.md`). A long interrupts-off window
is indistinguishable, from the watchdog's point of view, from the CPU wedge it exists to catch.
**A careless commit can reset the satellite.**

Rules:

1. Erase one sector per critical section. Re-enable interrupts between sectors.
2. Budget against the external watchdog timeout, which is not yet known (`storage_api.md`
   §12.C). Until it is, no write path is signed off as safe.
3. Never write from an ISR, a timer callback, or `xTimerPendFunctionCall`.
4. All writes go through one task, or at least one mutex with a finite timeout that returns
   `STORAGE_ERR_BUSY`.
5. Prefer program-only updates. The latch region never erases; that is why it exists.
6. Commit from a task whose watchdog deadline can absorb the stall, or `Watchdog::Suspend()` the
   affected client around the commit and `Resume()` after.

After any write, `flash_flush_cache()` before reading the range through `XIP_BASE`, or read via
`XIP_NOCACHE_NOALLOC_BASE`. Stale cache looks exactly like a write that silently did nothing.

---

## 6. The region layer, for implementers

Clients of layer 2 never see this. Whoever writes layer 2 does.

```c
storage_region_t r;
storage_region_open(REGION_CONFIG_B, &r);          // resolves base/size; never a raw pointer

storage_region_erase(&r, 0, FLASH_SECTOR_SIZE);    // off, len: sector multiples
storage_region_write(&r, 0, buf, FLASH_PAGE_SIZE); // off, len: page multiples; does NOT erase
storage_region_read (&r, 0, buf, 37);              // any offset, any length
```

- Offsets are **region-relative**. The SDK wants offsets relative to `XIP_BASE`, reads want
  `XIP_BASE + offset` as a pointer; the region layer does that translation so no client ever
  holds a raw flash offset. Mixing those two up is the easiest way to erase the program.
- `storage_region_write()` deliberately does not erase. A write that silently erases the
  surrounding sector destroys whatever shared it. Read-modify-write is explicit, one layer up.
- Every call bounds-checks `off + len` against the region, with the addition checked for
  overflow first.
- Route through `flash_safe_execute()` with `PICO_FLASH_ASSUME_CORE1_SAFE=1` even though core 1
  is never started, so the day someone starts it this code does not become the bug. Add
  `pico_flash` and `hardware_flash` to `target_link_libraries`.

Region map, offsets from `STORAGE_BASE_OFFSET = PICO_FLASH_SIZE_BYTES - 1024 KB`:

| Region | Offset | Size | Holds |
|---|---|---|---|
| `REGION_CONFIG_A` | `0x00000` | 16 KB | K/V bank A |
| `REGION_CONFIG_B` | `0x04000` | 16 KB | K/V bank B |
| `REGION_LATCH` | `0x08000` | 4 KB | 16 write-once flags |
| `REGION_SCRATCH` | `0x09000` | 4 KB | firmware-update metadata, never the image |
| reserved | `0x0A000` | 984 KB | unallocated; update the table before using |

The base is derived, not written down, because the dev board is 4 MB and the flight part is
2 MiB. `static_assert` that the device is larger than the reservation, or the subtraction wraps
and lands storage on the program.

---

## 7. What survives a reflash

| Path | Storage regions |
|---|---|
| UF2 drag-and-drop, `picotool load` | survive |
| `picotool erase`, flash nuke | destroyed; reprovision config |
| Debugger flash algorithm | usually destroyed; check the probe config before a persistence test |

The in-flight firmware update stages its image in the slack between `__flash_binary_end` and
`STORAGE_BASE_OFFSET`, never in a storage region. On the 2 MiB flight part that caps the running
image at about 512 KB; the current build is around 25 KB.

---

## 8. Implementation checklist

In the order the dependencies run:

1. `storage.h` / `storage.c`: region table, the compile-time checks from `storage_api.md` §4.2,
   `storage_init()` with the runtime `STORAGE_BASE_OFFSET >= __flash_binary_end` check, the four
   region calls.
2. `latch.h` / `latch.c`: page-per-flag, RAM cache at init, CRC-fail-reads-as-set.
3. `nvm.h` / `nvm.c`: bank header, RAM mirror, `nvm_get`/`nvm_set`/`nvm_commit`, header-last
   commit ordering.
4. Wire `Hal::StorageInit()` / `StorageAvailable()` on master to `storage_init()`.
   `Hal::StorageAppend()` stays false: bulk records are not this store's job (§1).
5. Measure the fitted part's worst-case sector erase and the external watchdog timeout, and write
   the margin into `storage_api.md` §6.

Naming is Pico SDK `snake_case`, deliberately, because this layer sits directly on SDK calls.

---

## 9. Where to go next

- `docs/internal/storage_api.md` — the full design, with every SDK constant it rests on cited.
- `docs/internal/hardware_block_diagram.md` §2 — the MRAM as drawn, and the size correction.
- `docs/tutorials/flight_tasks.md` (branch `flight-tasks` / `master`) — the watchdog a commit
  has to stay inside.
