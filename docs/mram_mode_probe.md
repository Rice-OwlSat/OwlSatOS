# MRAM interface-mode probe — test card

**Question:** once the AS3016204 is put into QPI (4-4-4) or DPI (2-2-2), does it stay there
(a) across an RP2350 reset with power held, and (b) across a full power cycle? And does the
mode-independent `escape` sequence reliably bring it back to SPI?

**Board:** modified WeAct RP2350A. AS3016204 (16 Mbit / 2 MiB STT-MRAM, 3.3 V) on QMI CS0, blinky
in the program partition, bottom 1 MiB block-protected (SR = 38h) and locked (CR1.MAPLK = 1).

## Setup

1. Build: `cmake --preset pico2-debug`, then `cmake --build build/pico2-debug --target MRAM_Probe`.
2. Load into SRAM and attach a terminal: `mram-probe/run_mram_probe.ps1` (or
   `picotool load -v -x build/pico2-debug/MRAM_Probe.uf2`, then any serial terminal).
   The probe is `no_flash`. It is never written to the MRAM.
3. The probe sends **nothing** to the MRAM until you type a command. The banner shows the
   direct-mode clock: clk_sys / 30, which is 5 MHz at 150 MHz.

| Command   | Sends                                    | Notes |
|-----------|------------------------------------------|-------|
| `rdid`    | 9Fh + 4 bytes, single-wire               | Expect `E6 01 04 01` in SPI |
| `regs`    | `rdid`, then 05h (1 byte), 46h (4 bytes) | Register bytes are meaningless unless RDID matches |
| `qpie`    | 38h single-wire, then `rdid`             | Garbage RDID is the expected result |
| `dpie`    | 37h single-wire, then `rdid`             | Garbage RDID is the expected result |
| `escape`  | FFh at quad, dual, single width, then `rdid` | Each FFh in its own CS# cycle, 10 µs apart |
| `wdreset` | nothing                                  | Watchdog reboot, normal boot path |
| `bootsel` | nothing                                  | Reboot straight into BOOTSEL |

**LEDs:**

| Pattern | Meaning |
|---|---|
| GP24 and GP25 blinking together, about 2 Hz | Probe running, no RDID yet |
| GP25 solid | Last RDID matched (SPI) |
| GP24 solid | Last RDID did not match |
| GP24/GP25 alternating, about 1 s each | Blinky is running from the MRAM |

## Read this before interpreting C and D

The RP2350 bootrom's `flash_exit_xip` deliberately sends a sequence to CS0 that returns a device
from XIP and/or **QPI** mode to SPI (see its doc comment in `pico/bootrom.h`). The bootrom runs it on
the normal flash-boot path. So:

- **Blinky booting after `wdreset` does not mean the mode was lost on reset.** The bootrom may have
  escaped it. The informative outcome is the opposite one: if blinky does *not* boot, the bootrom's
  own exit sequence failed to bring this chip back from that mode.
- It is not known whether the BOOTSEL/USB path, or picotool's load, also touches the QSPI bus.
  **C0** below checks this. Run it first: D only means something if C0 comes back garbage.

Also: after `qpie`/`dpie`, the probe's single-wire RDID can make the chip drive IO0 at the same time
as the RP2350. That contention is brief (5 MHz, a few bytes) and is inherent to the "garbage proves
it switched" check.

## Experiments

**A. Baseline**
1. `rdid`: expect `E6 01 04 01`, GP25 solid.
2. `regs`: expect SR = 38, CR1 MAPLK set, CR2 QPISL = DPISL = 0.

**B. Escape check**
1. `qpie`: expect garbage RDID, GP24 solid.
2. `escape`: expect `E6 01 04 01`, GP25 solid.
3. Repeat 1–2 with `dpie`.

**C0. Control: does the BOOTSEL path touch the chip?** *(added, run before C and D)*
1. `qpie`, confirm garbage.
2. `bootsel` (power stays on). Reload the probe with picotool.
3. `rdid` **before anything else**.
   Garbage: the BOOTSEL + load path leaves the chip alone, and D is meaningful.
   Match: something on that path restored SPI, so a match in D proves nothing about volatility.
4. `escape`, then `rdid`.

**C. Reset without power loss**
1. `qpie`, confirm garbage.
2. `wdreset`. Record whether blinky boots (alternating LEDs) or what else happens.
3. Without removing power, re-enter BOOTSEL (hold BOOTSEL, tap RESET), load the probe.
4. `rdid`, then `escape`.

**D. Full power cycle**
1. `qpie`, confirm garbage.
2. Unplug **all** power for at least 5 seconds. (The MRAM only re-initialises after VCC falls below
   about 1.6 V, and the capacitors take time to drain.)
3. Hold BOOTSEL while reconnecting. Load the probe.
4. `rdid` **before anything else**. Match: the mode is volatile (if C0 was garbage). Garbage: the
   mode persists across power loss.
5. `escape`, then `rdid`.

Repeat C0, C and D with `dpie`.

## Results

| Experiment | Mode entered | Reset type | rdid after | Blinky booted (y/n) | Notes |
|------------|--------------|------------|------------|---------------------|-------|
| A          | —            | —          |            | —                   |       |
| B          | QPI          | none       |            | —                   | after `escape`: |
| B          | DPI          | none       |            | —                   | after `escape`: |
| C0         | QPI          | `bootsel`, power held |  | —                   | after `escape`: |
| C          | QPI          | watchdog, power held  |  |                     | after `escape`: |
| D          | QPI          | power cycle ≥ 5 s     |  | —                   | after `escape`: |
| C0         | DPI          | `bootsel`, power held |  | —                   | after `escape`: |
| C          | DPI          | watchdog, power held  |  |                     | after `escape`: |
| D          | DPI          | power cycle ≥ 5 s     |  | —                   | after `escape`: |
