/**

    @file      mram_transactions.cpp
    @brief     Raw SPI transactions with the MRAM.
    @details   ~
    @author    Viola Case
    @date      24.09.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <algorithm>
#include <array>

#include <OwlSat/mram.h>

#if PICO_NO_FLASH
#include <hardware/address_mapped.h>
#include <hardware/structs/qmi.h>
#else
#include <hardware/flash.h>
#endif


namespace OwlSat::MRAM {


  Mode BUS_MODE = Mode::M1_1_1;

  namespace {

    /*!
     * One CS# low->high cycle, full duplex: rx[i] is the byte clocked in while tx[i] went out.
     * tx and rx may be the same buffer, since rx[i] can't arrive before tx[i] has been sent.
     */
#if PICO_NO_FLASH
    // Mirrors the SDK's flash_do_cmd, which is compiled out of no_flash builds. Nothing executes
    // from the MRAM here, so there's no XIP to exit or re-enter around the transaction.
    void Transfer(const uint8_t *tx, uint8_t *rx, size_t count) {
      size_t tx_left = count, rx_left = count;
      hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS); // CS low
      hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);          // direct mode on
      while (tx_left || rx_left) {
        uint32_t csr = qmi_hw->direct_csr;
        if (tx_left && !(csr & QMI_DIRECT_CSR_TXFULL_BITS)) {
          qmi_hw->direct_tx = *tx++;
          --tx_left;
        }
        if (rx_left && !(csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) {
          *rx++ = (uint8_t)qmi_hw->direct_rx;
          --rx_left;
        }
      }
      hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);          // direct mode off
      hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS); // CS high
    }
#else
    // Exits XIP, runs the transaction and re-enters XIP. Nothing may touch XIP while it runs.
    void Transfer(const uint8_t *tx, uint8_t *rx, size_t count) { flash_do_cmd(tx, rx, count); }
#endif

  } // namespace

  bool Execute(const Instruction &instr, uintptr_t addr, std::span<const uint8_t> tx_data, std::span<uint8_t> rx_data) {
    // Only single-lane SPI is driven from here
    if (BUS_MODE != Mode::M1_1_1) return false;

    // Opcode, address MSB first, then 8 latency cycles per dummy byte (1-1-1)
    size_t headerLen = sizeof(Opcode) + instr.m_address_width + instr.m_latency_cycles / 8;
    size_t dataLen = std::max(tx_data.size(), rx_data.size());
    size_t beltLen = headerLen + dataLen;
    if (instr.m_address_width > sizeof(addr) || beltLen > Instruction::MAX_BELT_SIZE) return false;

    // Sent and received in place: the reply overwrites the belt as it goes out
    std::array<uint8_t, Instruction::MAX_BELT_SIZE> belt {};
    belt[0] = static_cast<uint8_t>(instr.m_opcode);
    for (size_t i = 0; i < instr.m_address_width; ++i)
      belt[sizeof(Opcode) + i] = static_cast<uint8_t>(addr >> (8 * (instr.m_address_width - 1 - i)));
    std::copy(tx_data.begin(), tx_data.end(), belt.begin() + headerLen);

    // WREN is only latched when CS# rises after it, so it needs its own transaction
    if (instr.m_needs_write_enable) {
      uint8_t wren = static_cast<uint8_t>(Opcode::WREN);
      Transfer(&wren, &wren, 1);
    }

    Transfer(belt.data(), belt.data(), beltLen);
    std::copy_n(belt.begin() + headerLen, rx_data.size(), rx_data.begin());
    return true;
  }

} // namespace OwlSat::MRAM
