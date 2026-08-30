# The OwlSat Kernel
---
OwlSat flight software, written in C/C++ for the **RP2350** (Raspberry Pi Pico 2).

## Building
- Open the project in VS Code
- Make sure the **Raspberry Pi Pico** VS Code extension is installed (Pico SDK 2.2.0)
- Click the `Compile` button in the bottom status bar
- Copy the `.uf2` from `build/` onto the Pico 2's boot drive

## Documentation
- This repo uses Doxygen as its documentation generator. From the project root, run `doxygen ./owlsatos.Doxyfile` and it should generate everything in both HTML and LaTeX form.

## What is currently working (master)
- FreeRTOS scheduler running on RP2350 Cortex-M33 @ 133 MHz
- USB stdio enabled; UART stdio disabled (see `pico_enable_stdio_*` in `CMakeLists.txt`)
- `blink` — 500 ms heartbeat on the onboard LED
- The barebones flight task set, described below

### The task layer
Five tasks, created in `main()` and paced against the scheduler:

| Task | Source | Priority | Cadence | Does |
|---|---|---|---|---|
| `sensor` | `src/sensor_task.cpp` | 1 | 5 s | Acquires the EUV array, appends to the storage table |
| `link` | `src/link_task.cpp` | 1 | 1 s | Asks the radio whether it will accept frames; publishes `EVT_LINK_READY` |
| `tx` | `src/transmit_task.cpp` | 2 | on demand | Packs pending records into frames, hands them to the radio |
| `wdt` | `src/watchdog_task.cpp` | 2 | 250 ms | Pulses `WDT_WDI` while the three above keep checking in |
| `blink` | `src/OwlSatOS.cpp` | 1 | 500 ms | Onboard-LED heartbeat |

Everything they can be tuned by lives in `include/OwlSat/config.h`.

**The hardware drivers are not on this branch.** `sensor`, `link` and `tx` talk to hardware only
through `include/OwlSat/hal.h`, and every function in it is stubbed in `src/hal_stub.cpp` to
report failure — the EUV chain is on `sxuv5-interface`, the store on `storage` /
`nonvolatile-data-storage`, the radio on `radio`. Each stub carries a `MERGE:` comment naming the
call that replaces it. Nothing above `hal.h` should need to change when they land.

So this build boots, runs the whole task graph, and honestly reports that it acquired nothing,
stored nothing durably and transmitted nothing. The stubs return failure rather than plausible
data on purpose: a stub that invented an irradiance would make the task layer look correct while
proving nothing about it.

### Two pieces worth knowing about before touching them

**The storage table** (`include/OwlSat/storage_table.h`) is a fixed ring, not a queue. With the
downlink stalled a queue would eventually block the sensor task and stop the science; the ring
drops the *oldest* record instead and counts the drop. Science acquisition never waits on the
radio. Records are marked downlinked only after the radio accepts the frame carrying them.

**The watchdog** (`include/OwlSat/watchdog.h`) does not simply kick on a timer. The block diagram
already notes that a task-driven kick proves more than a timer-driven one; this goes one step
further and gates the kick on check-ins, so `WDT_WDI` keeps pulsing only while *every* registered
task is still inside its deadline. A sensor task wedged forever on I2C stops the pulse train and
the external circuit resets the board. Deadlines are derived from the task periods in `config.h`,
not written independently, so retuning a period cannot arm a spurious reset.


## Tasks on the GANTT that still need to be done
Each task lives on its own branch. Keep all work scoped to its task until it is complete and merged. Tasks are not ordered by priority.
Once task is complete, make a pull request with verbose documentation.

### TASK: Implement the filesystem layer (`nonvolatile-data-storage` branch)
Need to rewrite this section

### TASK: Parse and store sensor/gyro data
Depends on the filesystem layer above being merged first.
The `sensor` task and the in-RAM storage table are already on master; what is missing is the
durable half — `Hal::StorageAppend()` currently returns false, so records survive only as long as
power does. The points below are all still open:
- Add a FreeRTOS task that reads from the satellite's sensors (gyroscope, temperature, etc.) on a fixed interval and writes records to the active day-file on the FAT12 volume
- Every 24 hours, close the current file and open a new one; use a FreeRTOS software timer (`xTimerCreate`) for the rollover — do not block the sensor task on a 24-hour delay
- FAT12 root directory holds at most 64 entries (hard limit set in the boot sector) — the naming/rotation scheme must stay within that limit

### TASK: Deal with requests for data
- Extend `consoleTask` with a real command parser instead of echo-only
- Wire it back into `main()` once parsing is implemented
- Handle at minimum: request for a specific data file, request for current sensor readings, and a status/health query
- Coordinate with the comms team on the exact wire format before finalizing — the ground station parser must agree on it

### TASK: Deal with update requests
- Accept an update command over UART that is followed by a raw binary payload
- For config updates: write to a reserved config file on the FAT12 volume and reload the in-memory config
- For firmware updates: stage the payload in flash, verify integrity (CRC or checksum), then reboot into the new image via the RP2350 bootrom — the current image must remain valid until the new one passes verification

### TASK: Serialize and transmit actual data (`radio` branch)
**The framing question is answered.** The AMSAT LTM ICD v2.3 makes OwlSat the *host platform*,
not the radio: the LTM owns the 665-byte frames, the CRC, the Reed-Solomon FEC and the 1200 bps
BPSK downlink. There is no AX.25 on this interface. Our job is CAN messages at 125 kbit/s with
the 29-bit identifier layout from ICD Table 6. See `docs/internal/ltm1_link_design.md`.

Done on the `radio` branch:
- Protocol layer (`include/OwlSat/ltm1.h`, `src/ltm1_can_id.cpp`) — identifier codec, science
  chunking, ICD Table 8 health telemetry, with the ICD's addressing rules enforced as
  `static_assert`s rather than comments
- `Hal::RadioInit/RadioQueryReady/RadioSendPacket` implemented in `src/ltm1_link.cpp`; the radio
  section of `hal_stub.cpp` is gone. Nothing above `hal.h` changed
- `OwlSatFrame` retained as OwlSat's own container, chunked across opaque science messages

Still open:
- **Select the SPI CAN controller part** — this blocks everything else. `MCP2515` vs `MCP2518FD`
  share no register map, so `src/can_controller_stub.cpp` reports honest absence until it is
  chosen, and the link still comes up `DOWN`
- Decide whether both transmit paths survive — opaque science (`OWLSAT_LTM_SEND_SCIENCE`) and
  ICD Table 8 health for FoxTelem (`OWLSAT_LTM_SEND_HEALTH`) are both compiled in for now
- Fill a `Ltm1::HealthSnapshot` from real sensors once the power/thermal branches merge; nothing
  calls `Ltm1::PublishHealth()` yet
- Bump `OWLSAT_FRAME_VERSION` on any layout change — the ground parser branches on it
- Hardware items the ICD raises and the block diagram does not answer — no 5 V rail for the
  LTM, `Umbilical Attached` must be held low in flight, LTM-as-I2C-master conflicts with I2C0.
  All listed in §8 of the design doc

### TASK: Battery management
- Poll battery
- Hibernate if needed
<!--TODO Figure out if battery is handled by hardware or software-->
