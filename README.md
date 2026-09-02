# The OwlSat Kernel

OwlSat flight software, written in C/C++ for the **RP2350** (Raspberry Pi Pico 2).

## Building
- Open the project in VS Code
- Make sure the **Raspberry Pi Pico** VS Code extension is installed (Pico SDK 2.2.0)
- Click the `Compile` button in the bottom status bar, or use a preset from `CMakePresets.json`
- Copy the `.uf2` from `build/` onto the Pico 2's boot drive

## Documentation
- This repo uses Doxygen as its documentation generator. From the project root, run
  `doxygen ./owlsatos.Doxyfile` and it generates HTML and LaTeX under `docs/` (both gitignored).
- **Tutorials** live in `docs/tutorials/` and appear under *Related Pages* in the Doxygen output.
  Each branch carries the tutorial for the interface it owns; see the table below.
- **Theme** lives in `docs/theme/`: `owlsat.css` (loaded through `HTML_EXTRA_STYLESHEET`, light and
  dark), `logo.png` in the banner and `favicon.png` in the tab. It only overrides doxygen's own CSS
  variables, so a doxygen upgrade will not break it.
- **Design documents** live in `docs/internal/`. `hardware_block_diagram.md` is the plain-text
  transcription of the block diagram and is on `master`; the others are on the branch they belong to.

| Tutorial | Branch | Covers |
|---|---|---|
| `docs/tutorials/flight_tasks.md` | `master` | boot order, console output, adding a task, landing a driver behind `hal.h` |
| `docs/tutorials/radio_link.md` | `radio` | the LTM-1 CAN link, the controller driver contract, science and health paths |
| `docs/tutorials/sxuv5_euv.md` | `sxuv5-interface` | taking an EUV reading, the flags, bench testing, calibration |
| `docs/tutorials/nonvolatile_storage.md` | `nonvolatile-data-storage` | config and latched flags against the designed API |

## Branch map
Each piece of hardware has its own branch. Work stays scoped to its branch until it is complete,
then it comes in by pull request with verbose documentation. The task layer on `master` talks to
every driver only through `include/OwlSat/hal.h`, so a driver branch merges by rewriting its section
of `src/hal_stub.cpp` and nothing above it moves.

| Branch | What is on it | State | Blocked on |
|---|---|---|---|
| `master` | FreeRTOS, the five-task flight set, the storage table, frame format, watchdog gating. Every `hal.h` call stubbed. | Boots and runs the full task graph against absent hardware | nothing |
| `flight-tasks` | The task layer above. | **Merged into `master`**; kept for history and the tutorial | — |
| `radio` | AMSAT LTM-1 link over CAN per ICD v2.3: identifier codec, science chunking, Table 8 health telemetry, `Hal::Radio*` implemented in `src/ltm1_link.cpp`. | Protocol layer complete and `static_assert`ed against the ICD; bus layer stubbed | **SPI CAN controller part selection** (MCP2515 vs MCP2518FD share no register map). Also: `RADIO_PWR` pin, a 5 V rail, `Umbilical Attached` line |
| `sxuv5-interface` | EUV science chain driver, four layers: transport (SENS_PWR, MUXSEL, GAINSEL, ADS7828 on I2C0), auto-ranged acquisition, conversion to irradiance with σ, and reconstruction of true irradiance from the five face projections. | Complete against placeholder constants; builds | Hardware lead confirming TIA feedback resistances, bias mode, mux channel map, settling times; harness pin numbers |
| `nonvolatile-data-storage` | Design for a K/V config store plus latched one-shot flags in the top 1024 KB of the QSPI device. **No code yet.** | Design reviewed; supersedes the FAT12 plan | **Hardware confirming the RP2350 boots from the MRAM on CS0.** Until then software assumes 2 MiB NOR-flash semantics |
| `storage` | FAT12 volume over USB mass storage (`msc_disk.c`, TinyUSB). | **Superseded** by `nonvolatile-data-storage`. Ground-test debug path at most; must not become the flight persistence path | — |
| `toolchain` | Early bring-up experiments with a custom `toolchain.cmake` and SDK overrides, June 2026. | Historical. `master` builds with the Pico extension's toolchain and `CMakePresets.json` instead | — |

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
report failure — the EUV chain is on `sxuv5-interface`, the store on `nonvolatile-data-storage`,
the radio on `radio`. Each stub carries a `MERGE:` comment naming the call that replaces it.
Nothing above `hal.h` should need to change when they land.

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

## Path to the assembled-board prototype
The board gets assembled once hardware has the MRAM booting. In the order the dependencies run,
what has to happen for the first assembled board to fly the full task graph:

1. **Hardware answers the MRAM question.** Does the RP2350 boot from it, and does `hardware_flash`
   drive it? Everything on `nonvolatile-data-storage` waits on this; the design is written so a
   "yes, it behaves like NOR" answer is a driver swap and a "no" changes the plan rather than
   the code.
2. **Merge `sxuv5-interface`.** Rewrite the EUV section of `hal_stub.cpp` per its `MERGE:` note,
   add `hardware_i2c` to `CMakeLists.txt`, and `static_assert` that `OWLSAT_UV_FACE_COUNT` equals
   `SXUV5_FACE_COUNT`. The constants marked TBC stay TBC until the hardware lead signs them off;
   the driver runs on placeholders and says so at compile time.
3. **Select the CAN controller part, then merge `radio`.** `src/can_controller_stub.cpp` becomes
   a real driver for the five calls in `can_controller.h`; nothing else on the branch changes.
   The `link` task already drains the bus and the `tx` task already sends.
4. **Implement `nonvolatile-data-storage`** against its design doc and wire `Hal::StorageInit()`.
   Config and the antenna-deployed latch come from it; bulk telemetry does not.
5. **Assign the pins nobody has assigned yet.** `WDT_WDI`, `RADIO_PWR`, and everything in
   `pin_assignment.h` marked placeholder. The watchdog task currently pulses the console.

## Tasks on the GANTT that still need to be done
Tasks are not ordered by priority. Once a task is complete, make a pull request with verbose
documentation.

### TASK: Nonvolatile storage (`nonvolatile-data-storage` branch)
The FAT12 filesystem plan is superseded — see `docs/internal/storage_api.md` §10 for the
reasoning, which should not be relitigated. What replaces it is a region layer over the top
1024 KB of the QSPI device, an A/B-banked key/value store for config, and a write-once latch
region for flags that must never revert (antenna deployed, first boot done). Open:
- Implement `storage.[ch]`, `latch.[ch]`, `nvm.[ch]` per the design; the tutorial has the order
- Confirm the fitted part's worst-case sector erase time and the external watchdog timeout —
  a commit runs with interrupts off and can reset the satellite through the task-driven watchdog
- Decide how `PICO_FLASH_SIZE_BYTES` is set for the flight board (custom board header or CMake
  define); today a build silently uses the dev board's 4 MB

### TASK: Bulk telemetry sink
The storage design deliberately does not cover the sensor/gyro log — a high-write-rate ring with
different constraints from a config store. It has **no owner and no branch**. Before it can be
designed, someone has to supply: sample rate per stream, how many streams, whether the record is
24 B or 32 B, how often ground drains the log, and whether losing the oldest data is acceptable
(`storage_api.md` §12.A). Until then `Hal::StorageAppend()` returns false and records survive only
as long as power does, in the `OWLSAT_TABLE_CAPACITY`-record ring on master.

### TASK: Parse and store sensor/gyro data
Depends on the bulk telemetry sink above. The `sensor` task and the in-RAM storage table are on
master; what is missing is the durable half. The IMU, magnetometer and power monitors share I2C0
with the EUV ADC, so whichever task reads them has to respect the same bus arbitration the EUV
driver defers to.

### TASK: Deal with requests for data
- Extend `consoleTask` with a real command parser instead of echo-only, and wire it back into
  `main()` once parsing is implemented
- Handle at minimum: request for a range of records, request for current sensor readings, and a
  status/health query (`StorageTable::GetStats()`, `Ltm1::GetStats()`, watchdog state)
- Over the flight link, commands arrive as opaque CAN messages passed through by the LTM.
  `Ltm1::PollInbound()` already recognises and counts them (`rx_opaque`); dispatch is this task's job
- Coordinate with the comms team on the command format before finalising

### TASK: Deal with update requests
- Accept an update command followed by a raw binary payload
- For config updates: `nvm_set()` the affected keys and `nvm_commit()` once, from a task that can
  absorb the interrupts-off window
- For firmware updates: stage the payload in the slack between `__flash_binary_end` and
  `STORAGE_BASE_OFFSET` (never in a storage region), verify integrity, then reboot into the new
  image via the RP2350 bootrom — the current image must remain valid until the new one passes
  verification. On the 2 MiB flight part this caps the running image at ~512 KB

### TASK: Serialize and transmit actual data (`radio` branch)
**The framing question is answered.** The AMSAT LTM ICD v2.3 makes OwlSat the *host platform*,
not the radio: the LTM owns the 665-byte frames, the CRC, the Reed-Solomon FEC and the 1200 bps
BPSK downlink. There is no AX.25 on this interface. Our job is CAN messages at 125 kbit/s with the
29-bit identifier layout from ICD Table 6. `OwlSatFrame` survives as OwlSat's own container,
chunked across opaque science messages and reassembled on the ground. Open:
- **Select the SPI CAN controller part** — blocks everything below the protocol layer
- Decide whether both transmit paths survive — opaque science (`OWLSAT_LTM_SEND_SCIENCE`) and
  ICD Table 8 health for FoxTelem (`OWLSAT_LTM_SEND_HEALTH`) are both compiled in
- Fill a `Ltm1::HealthSnapshot` from real sensors once the power/thermal branches merge; nothing
  calls `Ltm1::PublishHealth()` yet
- Bump `OWLSAT_FRAME_VERSION` on any layout change — the ground parser branches on it
- Hardware items the ICD raises and the block diagram does not answer: no 5 V rail for the LTM,
  `Umbilical Attached` must be held low in flight, LTM-as-I2C-master must not be wired to I2C0.
  All listed in §8 of `docs/internal/ltm1_link_design.md`

### TASK: EUV science chain (`sxuv5-interface` branch)
The driver is complete against placeholder constants. Open:
- Hardware lead confirms the items in `docs/internal/sxuv5.md` §6 — feedback resistances, bias
  mode, case lead, mux channel map, SENS_PWR settling — then define `SXUV5_CONSTANTS_CONFIRMED`
- Confirm a TIA compensation capacitor exists before writing any averaging logic (§7)
- Wire `SetTemperatureProvider()` to the thermistor nearest the TIA
- Feed the ADCS sun vector to `ScaleEUV(sample, sun_body)`; the unaided solver returns only a
  lower bound for half the sky

### TASK: Antenna deployment
- Fire the burn wires once, then `latch_set(LATCH_ANTENNA_DEPLOYED)` so they never fire again
- Gate `RADIO_PWR` on that latch and on the launch provider's post-release timer; the ICD makes
  this an interlock, not a power switch

### TASK: Battery management
- Poll battery
- Hibernate if needed; `Ltm1::RequestMode(EnterSafeMode, 1)` asks the radio to shed load
- Publish battery voltage and temperature as Table 8 health telemetry
<!--TODO Figure out if battery is handled by hardware or software-->
