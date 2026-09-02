# Tutorial: the AMSAT LTM-1 radio link {#tutorial_radio_link}

**Branch:** `radio`.
**Reader:** someone who has to write the CAN controller driver, send something to the radio that
is not a science frame, read the link state off the console, or check an encoding against the
ICD.

The design rationale is in `docs/internal/ltm1_link_design.md`; this page is the operating
manual. One fact from that document colours everything below: **OwlSat is the ICD's "host
platform" and the LTM is the radio.** The LTM owns RF framing, CRC, forward error correction, the
1200 bps downlink and its own operational mode. Our whole job is to put well-formed CAN messages on
a 125 kbit/s bus.

---

## 1. The layers

```
   link_task.cpp / transmit_task.cpp        task layer — unchanged from master
             │
             v
   include/OwlSat/hal.h                     RadioInit / RadioQueryReady / RadioSendPacket
   include/OwlSat/ltm1_link.h               PollInbound / Mode / RequestMode / PublishHealth / GetStats
             │
             v   src/ltm1_link.cpp          policy: what "ready" means, mode tracking, all-or-nothing frames
             │
   include/OwlSat/ltm1.h                    pure encoding: identifiers, chunking, Table 7/8 messages
             │   src/ltm1_can_id.cpp        + static_asserts against the ICD
             v
   include/OwlSat/can_controller.h          Init / Available / TxFree / Send / Receive
             │   src/can_controller_stub.cpp   <-- reports absence until the part is chosen
             v
   SPI0 + CAN_CS  ─>  CAN transceiver  ─>  LTM-1
```

Nothing above `hal.h` changed when this branch landed. `link_task.cpp` gained exactly one call,
`Ltm1::PollInbound()`, at the top of its loop.

---

## 2. What works today, and what does not

| Works on this branch | Waits on |
|---|---|
| Identifier pack/unpack, checked against ICD Table 6 at compile time | — |
| Chunking an `OwlSatFrame` into science CAN messages | — |
| Encoding a `HealthSnapshot` into Table 8 messages | — |
| Deriving `LinkState` from controller + mailboxes + LTM mode | — |
| Tracking the LTM's mode from inbound Table 7 messages | — |
| Anything reaching a bus | **CAN controller part selection** |
| A `HealthSnapshot` with real numbers in it | power / thermal / GPS branches |
| Powering the radio at all | `RADIO_PWR` pin assignment + deployment interlock |

So on this branch the console still says `[ltm] radio DOWN` and the link task reports
`LinkState::Down`. `Down`, not `Unknown`: `RadioInit()` ran and the controller did not answer,
which is an established fact and reported as one.

---

## 3. Reading the console

```
[can] Init: no CAN controller in this build (part not selected)
[ltm] radio DOWN; source 8 -> dest 3, 125000 bit/s, science type 10
[link] radio DOWN (will keep asking); polling readiness every 1000 ms
[link] ready -> down (frames_free=0, 0 records pending)
```

Once a controller driver exists and the LTM is on the bus, expect:

```
[ltm] radio up; source 8 -> dest 3, 125000 bit/s, science type 10
[link] ready -> not-ready (frames_free=3, 0 records pending)   <- mode still Unknown
[ltm] mode 0 -> 2 (reason 1)                                   <- LTM announced Health mode
[link] not-ready -> ready (frames_free=3, 7 records pending)
[tx] frame 1 sent: 4 records (seq 1..4), 205 bytes
```

The `[ltm] mode` line uses `Ltm1::LtmMode` numbers: 1 safe, 2 health, 3 science. Reason codes
follow ICD Table 7 (`0` commanded, `1` low power / power ok, `2` science timed out).

`[ltm] ERROR: ... frame did not chunk` should never appear; it means a frame larger than
`OWLSAT_FRAME_MAX_BYTES` reached the link layer, which the transmit task cannot produce.

---

## 4. Writing the CAN controller driver

This is the blocking item. Replace `src/can_controller_stub.cpp` with a real implementation of
the five calls in `include/OwlSat/can_controller.h`. Nothing else on the branch changes.

Contract per call:

| Call | Must | Must not |
|---|---|---|
| `Init()` | bring up SPI0, `CAN_CS`, reset the part, program 125 kbit/s with the sample point near 75 %, leave it in **normal** mode | be called before `RADIO_PWR` is asserted (the transceiver is inside that domain) |
| `Available()` | return true only after `Init()` succeeded and the part is still answering | guess |
| `TxFree()` | count empty TX mailboxes | block |
| `Send()` | load a **29-bit extended** identifier, DLC and data into a free mailbox and request transmission | block waiting for arbitration, or buffer the message anywhere but the mailbox |
| `Receive()` | pop one message if one is waiting, reassembling the 29-bit ID from the part's split registers | block when the queue is empty |

Notes that will save you a day:

- Every identifier on this bus is extended. Do not implement standard 11-bit frames.
- `Send()` refusing is **normal backpressure**, not a fault. A science frame is 26 to 29 messages
  and a typical controller has 3 mailboxes, so the link layer already handles refusal by leaving
  the frame pending and letting the transmit task retry.
- Read `OWLSAT_LTM_CAN_BITRATE` from `config.h`. Do not put 125000 in a bit-timing table.
- An acceptance filter on destination ≥ 8 is an optimisation only; `Ltm1::IsForHost()` already
  rejects everything else in software.
- The `MERGE:` comments in the stub say which registers each call touches on an MCP2515-class
  part. If the chosen part is an MCP2518FD they do not apply.

Bench-test the driver in loopback mode first, then flip to normal mode. `Ltm1::GetStats()` will
show `tx_refused` climbing if the bus is not acknowledging.

---

## 5. Sending science: you already are

If the CAN driver works, the transmit task on master sends science with no changes.
`Hal::RadioSendPacket()` does the chunking:

```
OwlSatFrame (≤ 232 B)  ──ChunkFrameToScience()──>  up to 29 CAN messages
                                                    id: priority 16, source 8, type 10, dest 3,
                                                        msg_id = chunk index 0, 1, 2, ...
                                                    data: 8 frame bytes each (last one shorter)
```

Properties you can rely on:

- **All or nothing.** `RadioSendPacket()` checks `TxFree() >= count` before queuing a single
  chunk. A frame is never half on the bus.
- Chunk index lives in the 12-bit message ID, so lower chunks have lower identifiers and win
  arbitration in order. The ground reassembles on index 0 plus the frame's `0x4F 0x57` sync.
- The frame's CRC-16 is now an end-to-end check across the CAN hop. Keep it.

The type field (`OWLSAT_LTM_SCIENCE_TYPE`, default 10) says *which LTM mode downlinks the data*
and whether it is also saved as Whole Orbit Data. 10 = health-mode realtime plus WOD, which is
what keeps science acquired outside a pass alive. If AMSAT reports a tight WOD budget, change it
to 2 (realtime only) and nothing else.

---

## 6. Sending health telemetry (path B)

The ICD's Table 8 messages are what FoxTelem renders as temperatures and voltages. Nothing on the
branch calls this yet because nothing has real numbers to put in it. When the power and thermal
branches merge, a housekeeping task does:

```cpp
#include <OwlSat/ltm1_link.h>

Ltm1::HealthSnapshot snap = {};       // every *_valid flag and mask starts false / 0

snap.volts[0] = battery_v;            // slot assignments are yours; keep them contiguous
snap.volts[1] = rail_3v3;
snap.volts_valid_mask = 0b11;

snap.temp_c[0] = battery_therm_c;
snap.temp_valid_mask = 0b1;

snap.state_bits  = (radio_powered ? Ltm1::STATE_RADIO_POWERED : 0)
                 | (link_ready    ? Ltm1::STATE_LINK_READY    : 0);
snap.state_valid = true;

size_t queued = Ltm1::PublishHealth(snap);   // 0 if the bus is down or nothing was valid
```

Rules:

- **Leave a group invalid rather than sending zeros.** A zero decodes on the ground as −20 °C or
  0 V and reads as a real, alarming measurement. Silence means "not fitted".
- Table 8 has no per-slot validity. The encoder trims the DLC to the highest valid slot, so a gap
  *below* a valid slot goes out as bottom-of-scale. Assign slots so fitted sensors are contiguous
  from 0.
- Values saturate at the scale ends and round to nearest. Do not pre-scale.
- Health is periodic and idempotent, so a refused message is dropped, not retried. Science is the
  opposite, and is handled the opposite way.
- Nothing serialises `PublishHealth()` against `RadioSendPacket()`; they use separate scratch
  buffers and the counters are deliberately unlocked. Call `PublishHealth()` from one task.

---

## 7. Mode: reading it and asking for it

The LTM does not answer questions about its mode; it announces changes. So:

- **Somebody must drain the receive queue**, or mode stays stale. The link task does this every
  second via `Ltm1::PollInbound()`. Do not remove that call.
- `Ltm1::Mode()` returns the last announced mode, `Unknown` before the LTM has said anything and
  after every boot. The LTM resets on its own every few days and re-enters its previous mode; mode
  must be re-learned from the bus, never cached across a reset.
- `Ltm1::RequestMode(StatusMsg::EnterSafeMode, 1)` asks for safe mode with reason "low power".
  It is a request. `Mode()` does not change until the LTM confirms over the bus.

Mode feeds readiness. `RadioQueryReady()` reports `NotReady` while the LTM is in safe mode or
before it has announced anything, so science stays in the storage ring instead of being handed to
a radio that is shedding load.

---

## 8. What "ready" means

The ICD gives no queue-depth query, so `Hal::RadioQueryReady()` derives it:

```
Ready = Can::Available() && Can::TxFree() > 0 && ModeCarriesScience(Ltm1::Mode())
```

`LinkStatus::frames_free` reports **CAN mailboxes, not frames**. It is a floor on what the bus
will take. The transmit task only compares it against zero, so this is harmless today; do not
start dividing by it.

---

## 9. Checking an encoding against the ICD

Everything in `ltm1.h` is a pure function, so it can be checked on a laptop or in a
`static_assert`. The bottom of `src/ltm1_can_id.cpp` is the test suite; add to it rather than
writing a comment.

```cpp
constexpr uint32_t id = Ltm1::HostToLtm(OWLSAT_LTM_PRIORITY_SCIENCE,
                                        Ltm1::MsgType::HealthRealtimeAndWod, 0);
static_assert(Ltm1::UnpackId(id).dest == Ltm1::DEST_LTM);
static_assert(Ltm1::UnpackId(id).source >= 8);           // ICD requires this
static_assert(!Ltm1::IsForHost(id));                     // our own traffic is not inbound
```

The values the ICD fixes (destination 3, LTM source 3, Table 7/8 message IDs, scale endpoints)
are constants in `ltm1.h`. The values it leaves to the host (source ID, priorities, science type,
burst sizes) are macros in `config.h`. If you find yourself editing a Table number in `config.h`,
it is in the wrong file.

---

## 10. Open hardware items you will hit

Full list in `docs/internal/ltm1_link_design.md` §8. The ones that block a bench test:

- **CAN controller part** — see §4.
- **`RADIO_PWR` pin** — `RadioInit()` must assert it before `Can::Init()`, and the ICD makes it an
  interlock: no LTM power until antennas are deployed and the post-release timer has expired.
- **5 V rail** — the LTM wants 5.0 V ±5 % at up to 2.9 W and the board has no identified source.
- **`Umbilical Attached` must be held low in flight** or the LTM will not transmit.
- **Do not connect the LTM's I2C.** It is always master and would fight the RP2350 on I2C0.
  Everything it would read there goes over CAN as Table 8 instead.

---

## 11. Where to go next

- `docs/tutorials/flight_tasks.md` — the task layer this plugs into.
- `docs/internal/ltm1_link_design.md` — sizes, budgets, and every ICD citation.
- `include/OwlSat/ltm1.h` — every field layout, with the Table it came from.
