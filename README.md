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
- `BoardLEDTask` — 250 ms heartbeat blink on the onboard LED
- `ReadButtonTask` — debounce-free polling of a pushbutton on GPIO 16; prints press/release over UART
- UART stdio enabled; USB stdio disabled


## Tasks on the GANTT that still need to be done
Each task lives on its own branch. Keep all work scoped to its task until it is complete and merged. Tasks are not ordered by priority.
Once task is complete, make a pull request with verbose documentation.

### TASK: Implement the filesystem layer (`storage` branch)
The `storage` branch already has the raw FAT12 flash driver (`msc_disk.c`) and the USB MSC stack working. What is missing is the higher-level file API that the rest of the OS will use.
- `FILE_STRUCTURE` in `src/storage.cpp` is empty — fill it out with at minimum: sector LBA, byte offset, file size, dirty flag
- `openFile(path)` — locate or create the file's FAT12 directory entry, return a populated handle
- `closeFile` — flush any dirty data and update the directory entry size
- Add `readFile` and `writeFile` functions to the public API in `storage.h`
- Must reuse the existing `write_cache`/`cache_flush`/`cache_load` machinery from `msc_disk.c` — do not add a second cache on top of it

### TASK: Parse and store sensor/gyro data
Depends on the filesystem layer above being merged first.
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

### TASK: Serialize and transmit actual data
- Define a binary telemetry frame format (header, timestamp, sensor fields, checksum) — document it in `include/telemetry.h`
- Implement a serializer that packs sensor readings into frames
- Add a telemetry transmit task that pulls from a FreeRTOS queue and sends frames over UART or the RF interface
- Coordinate with the comms team on baud rate, framing, and packetization (COBS, SLIP, etc.) before writing the transmit path

### TASK: Battery management
- Poll battery
- Hibernate if needed
<!--TODO Figure out if battery is handled by hardware or software-->
