/**
 * @file can_controller_stub.cpp
 * @brief No-op implementation of the SPI-attached CAN controller interface.
 *
 * Placeholder for the driver that cannot be written yet: the block diagram puts a CAN controller
 * on SPI0 behind CAN_CS and does not say which part, and the MCP2515 and MCP2518FD register maps
 * share nothing worth abstracting over. Selecting the part is the item this file waits on.
 *
 * @par Why these do not pretend to work
 * The same reason hal_stub.cpp gives. A controller stub that accepted messages and reported free
 * mailboxes would make the whole link layer above it look exercised — chunking, arbitration
 * ordering, mode gating — while proving only that the code runs. Reporting absence keeps the
 * console honest and keeps `[link] down` on screen where the missing hardware belongs.
 */

#include <cstdio>

#include <OwlSat/can_controller.h>

namespace OwlSat::Can {

  bool Init() {
    // MERGE: choose the controller, then bring up OWLSAT_SPI0 (OWLSAT_SPI0_SCK / _MOSI / _MISO)
    // at the part's maximum sane clock, assert CAN_CS, reset, and program bit timing for
    // OWLSAT_LTM_CAN_BITRATE (125 kbit/s) with the sample point around 75%. Leave the part in
    // normal mode, not loopback. All four pins are in pin_assignment.h; CAN_CS is driven as
    // plain GPIO because the SPI block's hardware CS holds for a transfer, not a transaction.
    //
    // CAN_INT is assigned but provisional — the part is not chosen, and a polled driver need not
    // use it. Prefer polling until the part is known.
    printf("[can] Init: no CAN controller in this build (part not selected)\n");
    return false;
  }

  bool Available() { return false; }

  uint16_t TxFree() {
    // MERGE: read the transmit buffer control registers and count the empty mailboxes.
    return 0;
  }

  bool Send(const Ltm1::CanMessage &msg) {
    // MERGE: load the extended identifier, DLC and data into a free TX mailbox and set the
    // request-to-send bit. Extended frames only — every identifier above is 29-bit.
    (void) msg;
    return false;
  }

  bool Receive(Ltm1::CanMessage *out) {
    // MERGE: check the receive interrupt flags, read the buffer, reassemble the 29-bit
    // identifier from the part's split ID registers, and clear the flag.
    //
    // Set an acceptance filter for destination >= 8 here if the part supports masking on the
    // identifier's middle bits. Ltm1::IsForHost() already rejects the rest in software, so the
    // filter is an optimisation and not a correctness requirement.
    (void) out;
    return false;
  }

} // namespace OwlSat::Can
