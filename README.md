# The OwlSat Kernel

OwlSat flight software, written in C/C++ for the **RP2350** (Raspberry Pi Pico 2).

## Building
`CMakePresets.json` is the single source of build configuration: CLion, VS Code and the command
line all configure from the same file. Every preset generates with Ninja into `build/<preset>/`,
sets `PICO_BOARD=pico2`, exports `compile_commands.json`, and points `PICO_SDK_PATH` and
`PICO_GCC_PATH` at the SDK 2.2.0 and `14_2_Rel1` toolchain that the Raspberry Pi Pico extension
installs under `~/.pico-sdk`. Both are cache variables, so building with a different toolchain is
`-DPICO_GCC_PATH=/path/to/arm-none-eabi-gcc` and not an edit to a tracked file.

**On a fresh clone**, run `initialize.ps1`, `initialize.bat` or `initialize.sh` — or
`git submodule update --init --recursive` directly. The FreeRTOS kernel is a submodule in
`freertos/` and nothing configures without it.

| Configure preset       | Host        | Build type                         |
|------------------------|-------------|------------------------------------|
| `pico2-release`        | Windows     | Release                            |
| `pico2-debug`          | Windows     | Debug, for SWD/debugprobe sessions |
| `pico2-relwithdebinfo` | Windows     | Optimised, keeps symbols           |
| `pico2-release-unix`   | Linux/macOS | Release                            |
| `pico2-debug-unix`     | Linux/macOS | Debug                              |

The Windows/Unix split is a preset `condition` on `hostSystemName`, so only the presets that apply
to the machine you are on are offered. Each configure preset has a matching build preset that
builds the `OwlSatOS` target.

### Command line
```
cmake --preset pico2-release
cmake --build --preset pico2-release
```
leaves `OwlSatOS.uf2` (plus `.elf`, `.bin`, `.hex`, `.dis` and the map) in `build/pico2-release/`.
This is what both editors below do underneath.

### JetBrains (CLion)
CLion reads `CMakePresets.json` natively — no Pico plugin is involved and there is nothing to
configure by hand.
- Open the repository folder. The presets show up as CMake profiles under
  **Settings → Build, Execution, Deployment → CMake**, marked as coming from the preset file.
  Enable the ones you want and reload the CMake project.
- Select a profile in the toolbar and build the `OwlSatOS` target; artifacts land in
  `build/<profile>/`, the same place the command line puts them.
- A CMake Application run configuration cannot *run* an RP2350 image on the host — it builds and
  then fails to launch. Flash by copying the `.uf2`, or attach a debugger with an
  **Embedded GDB Server** configuration against `pico2-debug`. The repo ships no CLion debug
  configuration, and `.idea/` is gitignored, so which profiles are enabled is per-developer;
  `CMakePresets.json` is the part that is shared.

### VS Code
- Install the **Raspberry Pi Pico** extension (SDK 2.2.0). `.vscode/extensions.json` recommends
  it along with cortex-debug and the serial monitor.
- Either pick a preset through CMake Tools, or click the extension's `Compile` button — the
  latter configures into plain `build/` rather than `build/<preset>/`. `.gitignore` covers both
  with `build*/`.
- `.vscode/launch.json` carries a cortex-debug configuration for SWD.

### Flashing and console
Hold BOOTSEL, plug the Pico 2 in, and copy `build/<preset>/OwlSatOS.uf2` onto the drive that
appears. Console output is USB CDC — `pico_enable_stdio_usb` is on and `pico_enable_stdio_uart` is
off — so the board enumerates as a serial port and the boot banner arrives there, not on UART0.

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

| Tutorial                                | Branch                     | Covers                                                                       |
|-----------------------------------------|----------------------------|------------------------------------------------------------------------------|
| `docs/tutorials/flight_tasks.md`        | `master`                   | boot order, console output, adding a task, landing a driver behind `hal.h`   |
| `docs/tutorials/radio_link.md`          | `radio`                    | the LTM-1 CAN link, the controller driver contract, science and health paths |
| `docs/tutorials/sxuv5_euv.md`           | `sxuv5-interface`          | taking an EUV reading, the flags, bench testing, calibration                 |

## Branch map
Each piece of hardware has its own branch. Work stays scoped to its branch until it is complete,
then it comes in by pull request with verbose documentation — **into `dev`, not into `master`**.
`dev` is where branches meet and where a merge is allowed to be wrong; `master` is what has been
confirmed stable, so a change reaches it only after it has been integrated on `dev` and checked
out there. The task layer talks to every driver only through `include/OwlSat/hal.h`, so a driver
branch merges by rewriting its section of `src/hal_stub.cpp` and nothing above it moves.

| Branch                        | What is on it                                                                                                                                                                                                                                                                                                                                                                  | State                                                                                                                                                               | Blocked on                                                                                                                             |
|-------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| `master`                      | The confirmed-stable line. FreeRTOS, the five-task flight set, the storage table, frame format, watchdog gating, `pin_assignment.h`. Every `hal.h` call stubbed.                                                                                                                                                                                                               | Configures and builds clean from the presets; boots and runs the full task graph against absent hardware                                                            | nothing                                                                                                                                |
| `dev`                         | The integration branch: every feature branch merges here first, and `master` is only moved once what is on `dev` is confirmed stable.                                                                                                                                                                                                                                          | Level with `master` — nothing is mid-integration right now                                                                                                          | —                                                                                                                                      |
| `radio`                       | AMSAT LTM-1 link over CAN: `Hal::Radio*` implemented in `src/ltm1_link.cpp`.                                                                                                                                                                                                                        | Protocol layer complete and `static_assert`ed against the vendor spec; bus layer stubbed in `src/can_controller_stub.cpp`                                                   | **SPI CAN controller part selection** (MCP2515 vs MCP2518FD share no register map). Also: outstanding vendor-spec integration items |
| `sxuv5-interface`             | EUV science chain driver, four layers: transport (SENS_PWR, MUXSEL, GAINSEL, ADS7828 on I2C0), auto-ranged acquisition, conversion to irradiance with σ, and reconstruction of true irradiance from the five face projections.                                                                                                                                                 | Complete against placeholder constants; builds. Its `CMakeLists.txt` already adds `hardware_i2c`                                                                    | Hardware lead confirming TIA feedback resistances, bias mode, mux channel map, settling times; harness pin numbers                     |
| `mram-interface`              | MRAM driver for the Avalanche AS3016204 (fitted, 2 MiB) and AS3064204 (prospective, 8 MiB), chosen at compile time: `include/OwlSat/mram.h` (API), `mram_core.h`, one header per part, `mram_config.h` (every editable value), `src/mram.cpp`, `src/mram_transactions.cpp`. XIP-safe transactions for an image that runs from the MRAM. Bench tools: `MRAM_Probe` (`docs/mram_mode_probe.md`) and the `MRAM_Test_*` targets. | API complete. Passes on the AS3016204 board from SRAM and from an image executing in place under FreeRTOS. The AS3064204 side builds but has never seen hardware | An AS3064204 to bench. Not yet behind `hal.h`: no storage layer uses it                                                               |
| `imu`                         | Nothing yet — created from `master` and currently identical to it. Intended home for the IMU and magnetometer on I2C0.                                                                                                                                                                                                                                                         | Placeholder; unowned                                                                                                                                                | —                                                                                                                                      |

`flight-tasks` is gone: it merged at `3d51aae` and the branch was deleted. Its tutorial survives
as `docs/tutorials/flight_tasks.md`. The `DEPRECATED-` branches are not meant to be checked out.
`imu` carries no work, and `dev` is level with `master` because nothing is part-way through
integration.

## What is currently working (master)
- The tree configures and builds from `CMakePresets.json`: `cmake --build --preset pico2-release`
  produces `build/pico2-release/OwlSatOS.uf2`. No source in `src/` or `include/` warns. The only
  warnings in a clean build are `-Wvolatile` on `vPortRecursiveLock()` in the FreeRTOS port's
  `portmacro.h`, repeated once per C++ translation unit that includes `FreeRTOS.h` — the port is
  C, the increment it does on a `volatile` is deprecated in C++20, and it is submodule code
- FreeRTOS scheduler running on RP2350 Cortex-M33 @ 133 MHz
- USB stdio enabled; UART stdio disabled (see `pico_enable_stdio_*` in `CMakeLists.txt`)
- `blink` — 500 ms heartbeat on the onboard LED
- The barebones flight task set, described below
- `include/OwlSat/pin_assignment.h` — one project-wide header for every GPIO, bus instance and
  device address, included by every branch rather than copied. Read the "Pin budget" note at the
  bottom of it before adding a signal: an RP2350A has 30 GPIO, the block diagram wants about
  thirty-five, and the shortfall is why `MAG_STAT`, both `LTC4121_*_FAULT` lines, `BURN_1` and
  `USER_BUTTON` are `Pins::UNASSIGNED` rather than numbered

`CMakeLists.txt` globs `src/*.c` and `src/*.cpp` with `CONFIGURE_DEPENDS`, so a new source file is
picked up without a manual re-configure. Master links `pico_stdlib`, `hardware_pwm` and the
FreeRTOS kernel and heap, and nothing else — each driver branch adds what it needs.

### The task layer
Five tasks, created in `main()` and paced against the scheduler:

| Task     | Source                  | Priority | Cadence   | Does                                                                     |
|----------|-------------------------|----------|-----------|--------------------------------------------------------------------------|
| `sensor` | `src/sensor_task.cpp`   | 1        | 5 s       | Acquires the EUV array, appends to the storage table                     |
| `link`   | `src/link_task.cpp`     | 1        | 1 s       | Asks the radio whether it will accept frames; publishes `EVT_LINK_READY` |
| `tx`     | `src/transmit_task.cpp` | 2        | on demand | Packs pending records into frames, hands them to the radio               |
| `wdt`    | `src/watchdog_task.cpp` | 2        | 250 ms    | Pulses `WDT_WDI` while the three above keep checking in                  |
| `blink`  | `src/OwlSatOS.cpp`      | 1        | 500 ms    | Onboard-LED heartbeat                                                    |

Everything they can be tuned by lives in `include/OwlSat/config.h`.

**The hardware drivers are not on this branch.** `sensor`, `link` and `tx` talk to hardware only
through `include/OwlSat/hal.h`, and every function in it is stubbed in `src/hal_stub.cpp` to
report failure — the EUV chain is on `sxuv5-interface`, the MRAM driver on `mram-interface`,
the radio on `radio`. Nothing yet turns the MRAM driver into the store `hal.h` asks for. Each stub carries a `MERGE:` comment naming the call that replaces it.
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
The MRAM question is answered on the test board: the RP2350 boots from the AS3016204 on CS0, and
`mram-interface` drives it, including from an image that executes in place. In the order the
dependencies run, what has to happen for the first assembled board to fly the full task graph:

1. **Merge `mram-interface` into `dev`.** It adds `hardware_flash`, `pico_flash` and
   `PICO_FLASH_ASSUME_CORE1_SAFE` to the build. Nothing above `hal.h` changes.
2. **Merge `sxuv5-interface` into `dev`.** Rewrite the EUV section of `hal_stub.cpp` per its
   `MERGE:` note and `static_assert` that `Config::UV_FACE_COUNT` equals `SXUV5_FACE_COUNT`. The branch's
   `CMakeLists.txt` already adds `hardware_i2c`, so the merge carries it. The constants marked
   TBC stay TBC until the hardware lead signs them off; the driver runs on placeholders and says
   so at compile time.
3. **Select the CAN controller part, then merge `radio` into `dev`.**
   `src/can_controller_stub.cpp` becomes a real driver for the five calls in `can_controller.h`;
   nothing else on the branch changes.
   The `link` task already drains the bus and the `tx` task already sends.
4. **Build the storage layer on the MRAM driver.** `Hal::StorageInit()` is still a stub, and the
   config store and latches behind it are not written. Config and the antenna-deployed latch come from this; bulk
   telemetry does not.
5. **Confirm the pin numbers.** Every number in `pin_assignment.h` is a placeholder until the
   harness drawing is released, and five signals have no pin at all. `WDT_WDI` and `RADIO_PWR` do
   now have (placeholder) numbers, and the watchdog task really pulses GPIO rather than the
   console — but nothing in that header is flight-valid until the hardware lead signs it off.

## Tasks on the GANTT that still need to be done
Tasks are not ordered by priority. Once a task is complete, open a pull request into `dev` with
verbose documentation; `master` moves separately, once the result has been confirmed stable.

### TASK: MRAM driver (`mram-interface` branch)
`include/OwlSat/mram.h` is the API: `Init`, `VerifyConfig`, `ApplyConfig`, `ReadID`,
`ReadRegister`, `WriteRegister`, `Read` and `Write`, all in single-lane SPI. The part is picked
by `Config::MRAMPart` in `mram_config.h`, which also holds every register value, timing and bus
setting. With `Config::MRAM_EXECUTE_IN_PLACE` (the default) each transaction runs from SRAM inside
`flash_safe_execute()` and suspends XIP around itself. Bench-tested on the AS3016204 over UART:
`MRAM_Test_Read`, `MRAM_Test_Write` (both from SRAM) and `MRAM_Test_XIP` (from the MRAM, under
FreeRTOS) all pass. Open:
- Bench the AS3064204 once one is in hand. Its RDID and its register reads at MLATS = 8 come from
  the datasheet only
- Test array writes with CR4.WRENS = Normal, the flight setting. The test board is still at the
  factory SRAM setting, where array writes don't need WREN
- Decide whether `ApplyConfig()` runs on the flight board: it sets CR1.MAPLK, which locks block
  protection, and CR4.WRENS = Normal
- Decide between an XIP design and `copy_to_ram`. `Config::MRAM_EXECUTE_IN_PLACE` switches the
  driver; the build checks it against the binary type
- A mutex around the driver for when more than one task uses it, and a guard that stops writes
  landing in the running image
- `MRAMWatchdogTask` is empty. `VerifyConfig()` is the check it is meant to run

### TASK: Nonvolatile storage
Built on the MRAM driver; no branch yet. The FAT12 filesystem plan stays superseded. The planned
replacement is a key/value store for config and a write-once latch region for flags that must
never revert (antenna deployed, first boot done), over the top half of the MRAM. The MRAM has no
erase and writes bytes in place, so NOR-flash constraints from the earlier design (sector erase
time, erase-before-write banking) no longer apply. A driver write holds interrupts off for one
transaction of at most 256 bytes. Open:
- Implement the config store and the latches, and `Hal::StorageInit()` behind them
- Decide how `PICO_FLASH_SIZE_BYTES` is set for the flight board (custom board header or CMake
  define); today a build silently uses the dev board's 4 MB

### TASK: Bulk telemetry sink
The storage design deliberately does not cover the sensor/gyro log — a high-write-rate ring with
different constraints from a config store. It has **no owner and no branch**. Before it can be
designed, someone has to supply: sample rate per stream, how many streams, whether the record is
24 B or 32 B, how often ground drains the log, and whether losing the oldest data is acceptable.
Until then `Hal::StorageAppend()` returns false on every branch, and records survive only as long
as power does, in the `Config::TABLE_CAPACITY`-record ring (256 records) on master.

### TASK: Parse and store sensor/gyro data
Depends on the bulk telemetry sink above. The `sensor` task and the in-RAM storage table are on
master; what is missing is the durable half. The IMU, magnetometer and power monitors share I2C0
with the EUV ADC, so whichever task reads them has to respect the same bus arbitration the EUV
driver defers to.

### TASK: Deal with requests for data `(TODO: I NEED TO REWRITE THIS SECTION BECAUSE A CONSOLE TASK IN FLIGHT IS A STUPID IDEA)`
- There is no console task to extend any more: `src/console.cpp` and `include/OwlSat/console.h`
  were deleted from `master` in `645215e` and survive only in history. Whatever answers commands
  has to be written fresh, and the TODO above is the reason it has not been
- Handle at minimum: request for a range of records, request for current sensor readings, and a
  status/health query (`StorageTable::GetStats()`, `Ltm1::GetStats()`, watchdog state)
- Over the flight link, commands arrive as opaque CAN messages passed through by the LTM.
  `Ltm1::PollInbound()` already recognises and counts them (`rx_opaque`); dispatch is this task's job
- Coordinate with the comms team on the command format before finalising

### TASK: Deal with update requests
- Accept an update command followed by a raw binary payload
- For config updates: write the affected keys through the config store, from a task that can
  absorb the interrupts-off window of each MRAM write
- For firmware updates: stage the payload between the end of the running image and the storage
  region (never in a storage region), verify integrity, then reboot into the new image via the
  RP2350 bootrom — the current image must remain valid until the new one passes verification.
  With storage in the top half of the 2 MiB AS3016204, this caps the running image at ~512 KB

### TASK: Serialize and transmit actual data (`radio` branch)
**The framing question is answered.** The AMSAT LTM interface specification makes OwlSat the *host platform*,
not the radio: the LTM owns the frame format, the error coding and the
downlink. There is no AX.25 on this interface. Our job is CAN messages at the vendor-specified
bit rate and identifier layout. `OwlSatFrame` survives as OwlSat's own container,
chunked across opaque science messages and reassembled on the ground. Open:
- **Select the SPI CAN controller part** — blocks everything below the protocol layer
- Decide whether both transmit paths survive — opaque science (`OWLSAT_LTM_SEND_SCIENCE`) and
  vendor health telemetry for FoxTelem (`OWLSAT_LTM_SEND_HEALTH`) are both compiled in
- Fill a `Ltm1::HealthSnapshot` from real sensors once the power/thermal branches merge; nothing
  calls `Ltm1::PublishHealth()` yet
- Bump `OWLSAT_FRAME_VERSION` on any layout change — the ground parser branches on it
- Hardware integration items the vendor spec raises and the block diagram does not answer.
  Details live in the vendor interface specification, not in this repo.
  Our own summary is in `docs/internal/ltm1_link_design.md` (section 8).

### TASK: EUV science chain (`sxuv5-interface` branch)
The driver is complete against placeholder constants. Open:
- Hardware lead confirms the items in `docs/internal/sxuv5.md` §6 — feedback resistances, bias
  mode, case lead, mux channel map, SENS_PWR settling — then define `SXUV5_CONSTANTS_CONFIRMED`
- Confirm a TIA compensation capacitor exists before writing any averaging logic (§7)
- Wire `SetTemperatureProvider()` to the thermistor nearest the TIA
- Feed the ADCS sun vector to `ScaleEUV(sample, sun_body)`; the unaided solver returns only a
  lower bound for half the sky

### TASK: Antenna deployment
- Fire the burn wires once, then set the antenna-deployed latch so they never fire again
- Gate `RADIO_PWR` on that latch and on the launch provider's post-release timer; the vendor spec makes
  this an interlock, not a power switch

### TASK: Battery management
- Poll battery
- Hibernate if needed; `Ltm1::RequestMode(EnterSafeMode, 1)` asks the radio to shed load
- Publish battery voltage and temperature as vendor health telemetry
<!--TODO Figure out if battery is handled by hardware or software-->
