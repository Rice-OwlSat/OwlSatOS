/**

  @file       can_controller.h
  @brief      Narrow interface onto the SPI-attached CAN controller that fronts the LTM-1.
  @details    The RP2350 has no CAN peripheral. The block diagram puts a controller on SPI0
              behind CAN_CS, bridging to the LTM's 125 kbit/s bus — and does not yet say which
              part. That is the whole reason this header exists as a boundary rather than as a
              driver: the register map of an MCP2515 and an MCP2518FD have nothing in common, so
              committing ltm1_link.cpp to either one now would be committing to a guess.

              Four calls is the entire surface the link layer needs. Everything above this file
              is written against extended identifiers and 8-byte payloads, which is common to
              every classic-CAN controller worth fitting.

              @par What replaces this
              src/can_controller_stub.cpp reports honest absence, exactly as hal_stub.cpp does
              for the sensor and storage branches. When the part is chosen, the stub is replaced
              by a real driver and nothing above it moves.

  @author     Viola Case
  @date       29.08.2026
  @copyright  (c) Viola Case, 2026. All rights reserved.

**/
#pragma once

#include <cstddef>
#include <cstdint>

#include <OwlSat/ltm1.h>

namespace OwlSat::Can {

  /**
   * @brief Brings up the controller: SPI0, CAN_CS, bit timing for OWLSAT_LTM_CAN_BITRATE.
   *
   * Assumes the radio power domain is already up — RADIO_PWR gates the transceiver as well as
   * the LTM, and a controller talking into an unpowered transceiver reports a healthy bus with
   * nothing on it.
   *
   * @return False if the controller did not answer. Every call below then fails too.
   */
  bool Init();

  /// @return True once Init() has succeeded and the controller is answering.
  bool Available();

  /**
   * @brief Transmit mailboxes currently free.
   *
   * The number the link layer reports upward as LinkStatus::frames_free. Zero is normal
   * backpressure, not a fault: the bus is 125 kbit/s and a full science frame is 29 messages.
   */
  uint16_t TxFree();

  /**
   * @brief Queues one message for transmission.
   *
   * Does not block waiting for arbitration. A message that will not fit is refused rather than
   * buffered here — the link layer already knows how to stop and try again, and a second queue
   * behind the controller's own would only hide how deep the backlog really is.
   *
   * @return False if no mailbox was free or the controller is unavailable.
   */
  bool Send(const Ltm1::CanMessage &msg);

  /**
   * @brief Takes one received message, if any is waiting.
   *
   * @param out Filled only when this returns true.
   * @return False when the receive queue is empty or the controller is unavailable. The caller
   *         cannot distinguish the two, and should not need to — both mean "nothing to act on".
   */
  bool Receive(Ltm1::CanMessage *out);

} // namespace OwlSat::Can
