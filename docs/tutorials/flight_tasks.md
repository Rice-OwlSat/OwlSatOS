# Tutorial: the flight task layer {#tutorial_flight_tasks}

**Branch:** `flight-tasks` (merged into `master`).
**Reader:** someone who has to add a task, tune a cadence, plug a driver into the task layer, or
read the console output and know whether the board is healthy.

This is a how-to. The *why* behind each design choice is in the header comments, which are
deliberately long; this page tells you what to call, in what order, and what you will see.

---

## 1. What is on the branch

Five FreeRTOS tasks, created in `main()` (`src/OwlSatOS.cpp`) and started under one scheduler:

| Task | Entry point | Priority | Cadence | Watchdog client |
|---|---|---|---|---|
| `sensor` | `OwlSat::SensorTask` | 1 | `OWLSAT_SENSOR_PERIOD_MS` (5 s) | yes |
| `link` | `OwlSat::LinkTask` | 1 | `OWLSAT_LINK_POLL_PERIOD_MS` (1 s) | yes |
| `tx` | `OwlSat::TransmitTask` | 2 | event-driven, `OWLSAT_TRANSMIT_WAIT_MS` timeout | yes |
| `wdt` | `OwlSat::Watchdog::WatchdogTask` | 2 | `OWLSAT_WATCHDOG_PERIOD_MS` (250 ms) | is the watchdog |
| `blink` | `blink` | 1 | `OWLSAT_BLINK_HALF_PERIOD_MS` | no, on purpose |

Three services sit under them, all in `include/OwlSat/`:

- **`storage_table.h`** — the in-RAM ring of `TelemetryRecord` the sensor task writes and the
  transmit task drains.
- **`telemetry.h`** — the record layout, the `OwlSatFrame` wire format and its serialiser.
- **`watchdog.h`** — the check-in registry that gates the `WDT_WDI` pulse train.

Everything that touches hardware goes through **`hal.h`**, and on this branch every function in it
is a stub in `src/hal_stub.cpp` that reports failure. That is intentional: the task layer is meant
to run, and be observed running, with no drivers present.

---

## 2. Data flow in one picture

```
   Hal::UvSample() ──> SensorTask ──> StorageTable::Append() ──┐
                                        (+ Hal::StorageAppend)   │  ring of TelemetryRecord
                                                                 │
   Hal::RadioQueryReady() ──> LinkTask ──> EVT_LINK_READY ──┐    │
                                                            v    v
                                        TransmitTask ── PeekPending() ── BuildTelemetryFrame()
                                             │                                   │
                                             └── MarkTransmitted() <── Hal::RadioSendPacket() ok
```

Two rules make the whole thing safe to leave running unattended:

1. **The sensor never waits on the radio.** The table is a ring, so a dead downlink costs the
   oldest records and never a missed sample.
2. **A record is downlinked only after the radio accepted the frame carrying it.** A rejected
   frame leaves its records pending.

---

## 3. Boot sequence, and what you must not reorder

`main()` does these in order. Each depends on the one before it.

```cpp
stdio_init_all();
OwlSat::StorageTable::Init();   // creates the table mutex — fatal if it fails
OwlSat::InitTaskEvents();       // creates the event group link/tx share — fatal if it fails
OwlSat::Watchdog::Init();       // zeroes the check-in registry, starts the boot grace period
(void) OwlSat::Hal::StorageInit();  // probes the nonvolatile store — NOT fatal
// ... xTaskCreate() x5 ...
vTaskStartScheduler();
```

- `StorageTable::Init()` and `InitTaskEvents()` **must** run before any task is created. Tasks use
  both without checking.
- `Watchdog::Init()` must run before `vTaskStartScheduler()`, because the boot grace period
  (`OWLSAT_WATCHDOG_BOOT_GRACE_MS`, 10 s) is timed from it.
- A fatal init failure calls `HaltForWatchdog()`: it prints, disables interrupts and spins. It
  never pulses `WDT_WDI`, so the external watchdog resets the board. That is the recovery path.

Per-subsystem hardware init (`Hal::UvInit()`, `Hal::RadioInit()`, `Hal::WatchdogInit()`) happens
**inside the owning task**, not in `main()`. A sensor that will not initialise costs the sensor
task and nothing else.

---

## 4. Reading the console

Connect to the USB CDC port. On this branch, with no drivers, a healthy boot looks like this:

```
=== OwlSatOS boot: main() reached, stdio up ===
[hal] StorageInit: no nonvolatile store in this build (branch storage)
all tasks created; starting scheduler...
[hal] UvInit: no EUV driver in this build (branch sxuv5-interface)
[sensor] EUV chain DOWN (sampling anyway, expect faults); sampling every 5000 ms
[hal] RadioInit: no radio driver in this build (branch radio)
[link] radio DOWN (will keep asking); polling readiness every 1000 ms
[tx] ready; 4 records per frame, 232 byte frames max
[hal] WatchdogInit: WDT_WDI pin not assigned; pulses go to the console only
[wdt] kicking every 250 ms once all clients check in (boot grace 10000 ms)
[link] ready -> unknown (frames_free=0, 0 records pending)
[sensor] acquisition failed (1 consecutive)
[tx] link not ready; 0 pending, 0 dropped, 0 downlinked
```

Then roughly one `[sensor] acquisition failed` per minute and one `[tx] link not ready` per
minute. **That is the expected steady state without hardware.** Nothing is fabricated; the board is
telling you honestly that it acquired nothing and sent nothing.

Lines that mean something is wrong:

| Line | Meaning | What to do |
|---|---|---|
| `FATAL: could not create task ...` | FreeRTOS heap exhausted at boot | lower stack depths in `config.h` or raise `configTOTAL_HEAP_SIZE` |
| `FATAL: stack overflow in task 'x'` | a task blew its stack | raise `OWLSAT_STACK_x`; `printf` is the usual culprit |
| `[wdt] STALL: sensor last checked in ...` | a task missed its deadline | that task is blocked somewhere — I2C wedge, mutex, infinite loop |
| `[sensor] ERROR: storage table rejected a record` | table not initialised | should be impossible after boot; look at `main()` |
| `[tx] frame N rejected` | radio refused a frame | normal on a marginal link; records stay pending |

With the drivers merged, the interesting lines become:

```
[sensor] seq 12  E=9.612e-04 W/m2  sigma=2.1e-05  flags=0x0000  faces=0x1F
[link] not-ready -> ready (frames_free=3, 12 records pending)
[tx] frame 1 sent: 4 records (seq 1..4), 205 bytes
```

---

## 5. Adding a task

Follow the existing four. The skeleton every flight task shares:

```cpp
void MyTask(void *params) {
  (void) params;

  // 1. Bring up whatever hardware this task owns. Not fatal if it fails.
  const bool up = Hal::MyInit();

  // 2. Fixed-reference pacing so cadence does not drift with work time.
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(OWLSAT_MY_PERIOD_MS);

  for (;;) {
    // 3. Check in FIRST, after the blocking call that paces you has returned.
    Watchdog::CheckIn(Watchdog::Client::My);

    // 4. Do the work. Log failures at an interval, not every loop.

    xTaskDelayUntil(&last_wake, period);
  }
}
```

Then, in this order:

1. **`config.h`** — add `OWLSAT_MY_PERIOD_MS`, `OWLSAT_PRIO_MY`, `OWLSAT_STACK_MY`, and
   `OWLSAT_WATCHDOG_DEADLINE_MY_MS` derived from the period with `OWLSAT_WATCHDOG_MARGIN`.
   Never write the deadline as an independent literal.
2. **`watchdog.h`** — add `Client::My` *before* `Count`. Then in `watchdog_task.cpp` add its
   deadline to `Init()` and its name to `ClientName()`.
3. **`tasks.h`** — declare the entry point.
4. **`OwlSatOS.cpp`** — one `Spawn()` line.

Priorities: 0 is idle, 3 is the FreeRTOS timer task, 4 is reserved for a future fault handler.
Use 1 for periodic work and 2 only for things that must preempt it. **Do not put a task above
the watchdog** unless you have read the priority comment in `config.h` and still want to.

If the task is a subsystem that gets deliberately shut down — a radio powered off to save budget —
call `Watchdog::Suspend(Client::My)` before it stops checking in and `Resume()` when it restarts.
A task that is *supposed* to be silent must not reset the spacecraft.

---

## 6. Plugging a driver into the task layer

The tasks do not know which branch a driver came from. To land one, rewrite the matching section
of `src/hal_stub.cpp` and nothing else. Each stub has a `MERGE:` comment naming the call that
replaces it.

| `hal.h` call | Owning task | Contract the task depends on |
|---|---|---|
| `UvInit()` / `UvSample()` | `sensor` | `UvSample` fills `out` only on success; a pass with faulted faces is still a success |
| `StorageInit()` / `StorageAppend()` / `StorageAvailable()` | table, `main()` | `StorageAppend` returns false if the record is not durable; table keeps working |
| `RadioInit()` / `RadioQueryReady()` / `RadioSendPacket()` | `link`, `tx` | `RadioQueryReady` fills `out` on *every* call; `RadioSendPacket` returns false if the frame was not accepted |
| `WatchdogInit()` / `WatchdogPulse()` | `wdt` | `WatchdogPulse` is edge-driven and must be cheap; no `printf` in it |

Two things a driver must **not** do from inside these calls:

- Block for longer than the owning task's watchdog deadline. Sensor gets 15 s, link 3 s, transmit
  6 s. Bound every bus transfer with a timeout.
- Return plausible data on failure. The tasks are written to treat `false` as "nothing happened",
  and a stub that lies makes the whole pipeline look correct while proving nothing.

The `radio` branch is the worked example: `src/ltm1_link.cpp` implements the three radio calls and
the radio section of `hal_stub.cpp` is deleted. `link_task.cpp` gained one line.

---

## 7. Using the storage table from new code

```cpp
#include <OwlSat/storage_table.h>

// Producer side (sensor task does this):
uint32_t seq;
StorageTable::Append(sample, &seq);

// Consumer side (transmit task does this):
TelemetryRecord batch[OWLSAT_RECORDS_PER_FRAME];
size_t n = StorageTable::PeekPending(batch, OWLSAT_RECORDS_PER_FRAME);
// ... send them ...
StorageTable::MarkTransmitted(batch[n - 1].seq);  // ONLY after the radio accepted the frame

// Anyone (console, health telemetry):
StorageTable::Stats s = StorageTable::GetStats();
printf("%lu pending, %lu dropped\n", s.pending, s.dropped);
```

Rules:

- `PeekPending()` **copies**. Never hold a pointer into the ring; the sensor task overwrites it.
- Every call takes the table mutex. Safe from any task, **not from an ISR**.
- Capacity is `OWLSAT_TABLE_CAPACITY` (256 records, about 21 minutes at the default cadence).
  When full, the oldest record is overwritten and `Stats::dropped` increments. If you see drops
  climbing on the console, the downlink is not keeping up; that is information, not a bug.

---

## 8. Changing the frame format

`telemetry.h` owns the wire layout. If you touch `UvSample`, `TelemetryRecord` or the frame
header:

1. Update `SerializeRecord()` in `src/telemetry.cpp` — the layout is written field by field,
   little-endian, so struct padding never reaches the wire.
2. Update `OWLSAT_RECORD_WIRE_BYTES` to match. The `static_assert` on `OWLSAT_RECORDS_PER_FRAME`
   will catch a payload that no longer fits a record.
3. **Bump `OWLSAT_FRAME_VERSION`.** The ground parser branches on it.

The CRC is CRC-16/CCITT-FALSE over everything before it. On the `radio` branch this became an
end-to-end check across the CAN hop, so keep it even if the link layer adds its own.

---

## 9. Tuning cadences safely

All the knobs are in `include/OwlSat/config.h`. When you change one:

- **Periods** feed the watchdog deadlines automatically (`period × OWLSAT_WATCHDOG_MARGIN`). You
  cannot arm a spurious reset by shortening a period, but you *can* by making a task's real work
  take longer than three periods. Check the `[wdt] STALL` line after any change.
- **`OWLSAT_WATCHDOG_PERIOD_MS`** must stay well under the external part's timeout, which is a
  board-level number not yet fixed. 250 ms is a placeholder.
- **Stack depths** are in words. They are generous because the tasks `printf`. Measure with
  `uxTaskGetStackHighWaterMark()` before trimming.
- **`OWLSAT_PACKET_MAX_PAYLOAD`** changes `OWLSAT_RECORDS_PER_FRAME` and the size of the transmit
  task's static buffers. It also changes how many CAN chunks a frame needs on the radio branch.

---

## 10. Where to go next

- `docs/tutorials/sxuv5_euv.md` (branch `sxuv5-interface`) — the driver behind `UvSample()`.
- `docs/tutorials/radio_link.md` (branch `radio`) — the driver behind `RadioSendPacket()`.
- `docs/tutorials/nonvolatile_storage.md` (branch `nonvolatile-data-storage`) — the design behind
  `StorageAppend()`, not yet implemented.
- `docs/internal/hardware_block_diagram.md` — what the board actually has.
