# LTM-1 Link Design — OwlSat as ICD Host Platform

**Source:** AMSAT Linear Transponder Module (LTM) Generic ICD, revision 2.3, 21 September 2024.
**Status:** design for the `radio` branch. The CAN controller part is not selected, so everything
below the protocol layer is stubbed.

> The ICD is marked AMSAT proprietary and "for official use only". This file carries only the
> encodings and constraints firmware needs, cited by section, and does not reproduce its text.
> The uplink command channel frequency is marked sensitive in the ICD and is deliberately absent
> here and from the code.

---

## 1. The thing the ICD settles

**OwlSat is the ICD's "host platform". The LTM is the radio.** This inverts an assumption that
had been sitting in `telemetry.h` since the task layer was written, so it is worth stating flatly:

| Concern | Owner |
|---|---|
| 665-byte telemetry frames | LTM |
| 4-byte CRC, Reed-Solomon (255,223) FEC | LTM |
| 1200 bps BPSK modulation, UHF downlink | LTM |
| Whole Orbit Data buffering and replay | LTM |
| Operational mode (safe / health / science) | LTM |
| Producing well-formed CAN messages | **OwlSat** |
| Deciding what is worth sending, and when | **OwlSat** |
| Powering the LTM only when it is safe to transmit | **OwlSat** |

The README's open question — "coordinate with the comms team on baud rate, framing, and
packetization (COBS, SLIP, etc.); AX.25 appears to be the standard" — is answered, and the answer
is that none of it is ours. There is no AX.25 on this interface. There is a 125 kbit/s CAN bus
(ICD 2.3.1) and the LTM does the rest.

`OwlSatFrame` therefore is **not** what reaches the radio. It survives only as an OwlSat-level
container chunked across science CAN messages, opaque to the LTM end to end (§4).

---

## 2. Bus and addressing

- **CAN, 125 kbit/s, 29-bit extended identifiers** (ICD 2.3.1). Classic CAN, not FD.
- CANH/CANL reach the LTM on PC-104 pins H2/51 and H2/52.
- The RP2350 has no CAN peripheral; the block diagram bridges from SPI0 via `CAN_CS`.

### Identifier layout (ICD Table 6)

| Bits | Field | What OwlSat puts there |
|---|---|---|
| 28:24 | Priority (5) | `OWLSAT_LTM_PRIORITY_*` — 1 status, 8 health, 16 science |
| 23:20 | Source (4) | `OWLSAT_LTM_SOURCE_ID` = 8. ICD **requires** ≥ 8 when addressing the LTM |
| 19:16 | Type (4) | `MsgType` — which LTM mode the data is downlinked in |
| 15:12 | Destination (4) | 3. The LTM accepts **only** 3 or 0 |
| 11:0 | Message ID (12) | Table 7 for status, Table 8 for health, **chunk index** for science |

Both sides must accept any priority and ignore it when interpreting a message, so the priority
field only orders our traffic against itself. It is still worth setting: CAN arbitration is
lowest-ID-wins, so a safe-mode request beats a science backlog to the bus.

**Inbound:** the LTM stamps destination 8 on messages for us and source 3 on its own. The host
must ignore anything with destination below 8 — `Ltm1::IsForHost()`, applied before any inbound
message is interpreted.

These rules are enforced as `static_assert`s at the bottom of `src/ltm1_can_id.cpp`, not as
comments. An out-of-range source produces an identifier the LTM silently drops, which is close to
undiagnosable on a bus.

---

## 3. Two paths out, both compiled in

Selectable via `OWLSAT_LTM_SEND_SCIENCE` / `OWLSAT_LTM_SEND_HEALTH` in `config.h`. One will
likely be dropped once the ground segment settles; until then neither choice is foreclosed.

| | Path A — science | Path B — health |
|---|---|---|
| Carries | `OwlSatFrame` bytes | OwlSat state as ICD Table 8 fields |
| Type | `OWLSAT_LTM_SCIENCE_TYPE` (10) | 5 |
| LTM understands it? | No — opaque | Yes |
| Ground display | Custom parser | FoxTelem, natively |
| Entry point | `Hal::RadioSendPacket()` | `Ltm1::PublishHealth()` |

**Why health is not routed through `RadioSendPacket()`.** A `HealthSnapshot` is not a function of
a frame — it comes from the power monitors, thermistors and GPS, none of which have merged.
Deriving one inside a call that takes frame bytes would mean inventing a data source to satisfy
an interface, which is precisely what `hal_stub.cpp` exists to avoid. Path B gets its own entry
point in `ltm1_link.h`, no caller on this branch yet, and returns an honest zero.

*(This is a deliberate departure from the approved plan, which put both paths behind
`RadioSendPacket()`.)*

---

## 4. Science chunking (path A)

Per ICD 2.6.5, science reaching the LTM over CAN is assembled into **83-byte Science payloads**,
where each contributing CAN message costs **12 identifier bits + a 4-bit length + its data
bytes** — i.e. **2 bytes of payload budget per message**, whatever the DLC. That is why the ICD
quotes 8 to 40 messages per payload:

```
8-byte messages: 8 + 2 = 10 B budget each  ->  8 messages fill 83 B, carrying 64 data bytes
0-byte messages: 0 + 2 =  2 B budget each  -> 41 messages fill 83 B, carrying 0 data bytes
```

Full 8-byte messages are the only efficient size, and the only one `ChunkFrameToScience()` emits
(bar the last).

**The chunk counter lives in the 12-bit message ID**, not in the data. Two properties follow:

- Every data byte is frame payload. A chunk costs nothing beyond the LTM's fixed 2 bytes.
- Lower chunk index is a numerically lower identifier, and CAN arbitration favours lower
  identifiers, so chunks of one frame naturally win the bus in order.

The ground reassembles using the chunk index together with the frame's own `0x4F 0x57` sync bytes:
index 0 starts a frame. `OwlSatFrame`'s CRC-16 becomes an **end-to-end** check underneath the
LTM's own CRC and FEC — it covers the CAN hop and the reassembly, which the LTM's checks do not.

### Sizes, as configured today

| Quantity | Value |
|---|---|
| `OWLSAT_RECORD_WIRE_BYTES` | 48 B |
| `OWLSAT_RECORDS_PER_FRAME` | 4 |
| Typical frame (10 + 193 + 2) | 205 B → **26 CAN messages** |
| Max frame `OWLSAT_FRAME_MAX_BYTES` | 232 B → **29 CAN messages** (`SCIENCE_MAX_MESSAGES`) |
| One frame on the bus | ≈ 30 ms at 125 kbit/s |

A frame spans roughly **four** of the LTM's 83-byte science payloads. That is the LTM's business,
not ours; chunk indices stay continuous across the boundary.

**A frame is queued whole or not at all.** `RadioSendPacket()` checks `TxFree() >= count` before
sending any chunk. A half-queued frame reaches the ground as a CRC failure and costs the records
in it anyway, so a mailbox that fills mid-frame is reported as a rejection — and the transmit task
already knows how to leave those records pending and retry.

### The real bottleneck is the downlink, not the bus

The LTM's downlink is 1200 bps and a frame takes ~6.5 s, so the RF side moves on the order of
**100 bytes/second**. The CAN bus moves ~15 kB/s. Science at the current 5 s cadence produces
~10 B/s of records. So the bus has roughly **three orders of magnitude** more capacity than the
downlink, and every question about what to send is a question about the downlink budget and the
LTM's WOD buffer — never about CAN throughput.

### Why science defaults to type 10

`OWLSAT_LTM_SCIENCE_TYPE` = 10 is *health-mode realtime **plus** Whole Orbit Data*. The
alternative is 2, realtime only.

Realtime alone means science acquired outside a ground station pass is downlinked into an empty
sky and lost — exactly the failure the storage ring upstream exists to prevent. Carrying it into
the LTM's WOD buffer keeps it alive across passes; the ICD notes WOD payloads are replayed
repeatedly before being overwritten. The cost is LTM buffer depth, which the ICD marks TBS. **If
AMSAT comes back with a tight WOD budget, this macro is the one that changes.**

Note also that type 3 ("science mode") is *not* the right default despite the name: per ICD 2.6.5
ordinary Science payloads are downlinked in **health** mode, which is the LTM's normal mode. Type
3 would mean our science only leaves the spacecraft during a commanded, timed science window.

---

## 5. Health telemetry (path B)

`BuildHealthMessages()` maps a `HealthSnapshot` onto ICD Table 8 message IDs:

| ID | Content | OwlSat source |
|---|---|---|
| 8 | Solar panel temps, +X −X +Y −Y +Z −Z | *not fitted — no per-panel temp sensors on the diagram* |
| 9 | Temps 0–7 | Battery `THERM`, RP2350 core, radio domain monitor |
| 16 | Solar panel voltages, same face order | LTC4121 panel ADCs |
| 17 | Voltages 0–7 | Battery ADM1176, 3V3 rail, `RFvdd` |
| 24 | 32 binary state bits | `Ltm1::StateBit` — deployment, power gates, link, storage |
| 32 | UTC year/month/day/hour/min/sec | GPS |

Scales are fixed by Table 8 and encoded in `EncodeTempC()` / `EncodeVolts()`:

- **Temperature:** byte 0–255 spans −20 °C to +107.5 °C — exactly 0.5 °C per code.
- **Voltage:** byte 0–255 spans 0 V to 10 V.

Both saturate rather than wrap, and round to nearest rather than truncating. Truncation would
bias every temperature in the downlink half a step cold: small, systematic, and the kind of error
that survives review because each individual reading looks plausible.

**An invalid group is not emitted at all.** Zero decodes on the ground as −20 °C or 0 V, which
reads as a real and alarming measurement. Silence is the honest encoding of "not fitted yet".

### Known limitation of Table 8

Table 8 gives no per-slot validity within a message. `BuildHealthMessages()` trims the DLC to the
highest valid slot, which reports fewer values rather than wrong ones — but a *gap below* that
slot (say sensors 0 and 3 fitted, 1 and 2 not) is inexpressible and goes out at the bottom of
scale. Worth raising with AMSAT if OwlSat ends up with a sparse sensor set; the alternative is to
assign slots so the fitted ones are contiguous.

---

## 6. What "ready" means

**The ICD gives the host no way to query the LTM's transmit queue depth.** The bus carries
telemetry toward the radio and status back; there is no "may I send" round trip. So
`Hal::RadioQueryReady()` *derives* readiness from the three things we can actually know:

```
Ready  =  controller reachable  AND  a TX mailbox is free  AND  the LTM's mode carries science
```

- **`Unknown`** — `RadioInit()` has not run.
- **`Down`** — it has, and the controller is silent. An established fact, not an assumption.
- **`NotReady`** — answering, but not now. Includes **safe mode**: the LTM is shedding load and
  carries health data only, so science handed over then is buffered at best. Holding it in the
  storage ring keeps it recoverable.
- **`Ready`** — send.

⚠️ **`LinkStatus::frames_free` reports CAN mailboxes, not frames.** One frame is ~26 messages, so
it is a floor on what the bus will take rather than a count of frames. The transmit task only
compares it against zero, so this is currently harmless — recorded here so it stays that way.

---

## 7. Mode tracking

The LTM announces its mode rather than answering questions about it, via Table 7 status messages
(type 4). `Ltm1::PollInbound()` drains the receive queue and updates `Ltm1::Mode()`; the link task
is the natural caller, on its 1 s poll.

| ID | Meaning | Data 0 = reason |
|---|---|---|
| 1 | Enter Safe Mode | 0 commanded, 1 low power |
| 2 | Enter Health Mode | 0 commanded, 1 power ok, 2 science timed out |
| 3 | Enter Science Mode | 0 commanded, 1 scheduled; data 1 = timeout [min] |

The host may **ask** for a mode — `Ltm1::RequestMode()` — most usefully safe mode on a low
battery, a condition the LTM cannot see. It is a request; `Mode()` does not change until the LTM
confirms. Opaque uplinked commands passed through by the LTM are counted (`rx_opaque`) but not
dispatched: command handling is its own GANTT task.

The LTM also resets on its own occasionally — the ICD notes on-orbit resets every few days, often
near the South Atlantic Anomaly — and re-enters its previous mode from non-volatile memory. Mode
must therefore be re-learned from the bus after any silence, never cached across a reset.

---

## 8. Open items this design depends on

Firmware assumptions should not quietly outrun these.

### Blocking
- **CAN controller part is not selected.** The block diagram says "CAN Transceiver (SPI ↔ CAN)"
  on SPI0 behind `CAN_CS`. MCP2515 (classic) and MCP2518FD (FD) share no register map, so
  `src/can_controller_stub.cpp` cannot be written until this is fixed. 125 kbit/s and 29-bit IDs
  are all this design needs from it; classic CAN is sufficient.

### Hardware / electrical
- **The LTM needs 5.0 V ±5 % at up to 2.9 W** (ICD 2.4, Table 5), on two `Vsys` PC-104 pins. The
  OwlSat power tree is 3.3 V-centric and `RFvdd` comes from an LDO the drawing already questions
  the capacity of. **There is no identified 5 V rail for the LTM.** This needs a hardware answer.
- **`Umbilical Attached` (H2/13) must be held LOW in flight.** The ICD flags this twice as
  CRITICAL. High inhibits transmission and diverts the LTM into its flash loader on boot. It is
  not on the block diagram at all, and needs a host-driven line plus a defined power-on state.
- **PC-104 interface lines are absent from the block diagram**: `Vsys` ×2, GND ×2, CANH/CANL
  (H2/51-52), `Umbilical Attached` (H2/13), GPIO1-4 (H2/14-17), analog `tlm1-4` (H1/20-23),
  I2C SCK/SDA (H1/43, H1/41), `i2c_reset` (H1/29), USB± (H1/52, H1/51). The LTM must sit at one
  **end** of the stack — it does not pass PC-104 bus lines through.
- **Unused analog `tlm1-4` lines should be tied to ground**, per ICD 2.3.2 — floating lines
  downlink random values.

### Interface conflicts
- **The LTM is always I2C master and cannot be a slave** (ICD 2.5.2). It expects to read host
  ADS7828s at 0x48–0x4B and MAX31725/6 at 0x4C–0x54 *directly*. OwlSat's EUV ADC is an ADS7828
  on **I2C0, where the RP2350 is master**. Wiring the LTM to I2C0 puts two masters on a bus that
  also carries the IMU, magnetometer and every power monitor. **Recommendation: do not connect
  the LTM's I2C at all.** Everything it would read there, we can send over CAN as Table 8 health
  telemetry — which path B already does.

### Sequencing
- **Antennas must be deployed and the launch provider's post-release timer expired before the LTM
  receives power** (ICD 2.4). `RADIO_PWR` is therefore an interlock, not just a power switch, and
  gating it belongs to whatever owns deployment. The block diagram's note that initial deployment
  must set an NVM key is the other half of this.
- The host must keep the LTM **unpowered** through integration, test and launch, with the single
  exception of an attached umbilical.

---

## 9. Where this lives

| File | Role |
|---|---|
| `include/OwlSat/ltm1.h` | Protocol layer: ID codec, Tables 6/7/8, chunking, health encoding |
| `src/ltm1_can_id.cpp` | Its implementation, plus the ICD `static_assert`s |
| `include/OwlSat/ltm1_link.h` | Radio-branch calls beside the facade: `PollInbound`, `PublishHealth`, `RequestMode`, `GetStats` |
| `src/ltm1_link.cpp` | Implements the `hal.h` radio facade over the protocol layer |
| `include/OwlSat/can_controller.h` | Four-call boundary onto the SPI CAN controller |
| `src/can_controller_stub.cpp` | Honest absence, pending part selection |

Nothing above `hal.h` changed. `link_task.cpp` and `transmit_task.cpp` are untouched — which was
the point of the facade.
