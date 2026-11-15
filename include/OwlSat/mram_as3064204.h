/**

  @file       mram_as3064204.h
  @brief      Datasheet facts for the Avalanche AS3064204 (64 Mb / 8 MiB, single QSPI).
  @details    The prospective part. Not bench-tested yet: everything here comes from the
              *64Mbit–128Mbit Dual QSPI MRAM Memory, Space-Grade-E* datasheet, revision C.8, and the
              table numbers refer to it.

              Differences from the AS3016204 that matter to the driver:
              - No per-register read opcodes. Registers are read with RDAR 65h, and RDAR's latency
                comes from CR2.MLATS (Tables 22, 25), which is nonvolatile and so unknown at boot.
              - WRENS and ODSEL move into CR1. There is no CR3 or CR4.
              - A read-only Flag Status Register at 0x0A, read with RDFSR 70h.
              - No augmented storage array, serial number, unique ID, reset or power-down instructions.

              Only facts about the chip live here. Values someone might want to change, like the
              flight register configuration, are in mram_config.h.

  @author     Viola Case
  @date       15.11.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include <OwlSat/mram_core.h>

namespace OwlSat::MRAM {

  /*!
   * The AS3064204. Has the same member names as AS3016204, so the driver can use either one
   * through Config::MRAMPart.
   */
  struct AS3064204 {
    static constexpr const char *NAME = "AS3064204"; //!< Part number, for logs

    /// Memory array size in bytes (64 Mb). Address bit 23 must be 0 (Table 11)
    static constexpr size_t CAPACITY = 8u * 1024u * 1024u;

    /*!
     * RDID reply (Table 20): manufacturer E6h, interface 0010 (HP Dual QSPI), voltage 0001 (3 V),
     * density 0001 (64 Mb), frequency 01h (100 MHz). The temperature grade nibble is masked out by
     * ID_MASK.
     */
    static constexpr std::array<uint8_t, 4> EXPECTED_ID { 0xE6, 0x21, 0x01, 0x01 };

    /// Bits of the RDID reply that must match EXPECTED_ID. The temperature grade nibble is ignored
    static constexpr std::array<uint8_t, 4> ID_MASK { 0xFF, 0xFF, 0x0F, 0xFF };

    /// RDAR's latency is CR2.MLATS (Table 22 note 1, Table 25)
    static constexpr bool RDAR_USES_MLATS = true;

    /*
     * ===========================================
     * Part-only instructions (Table 26)
     * ===========================================
     */

    static constexpr Instruction ReadFlagStatusRegister { Opcode::RDFS, 0, Latency::None, 0, false }; //!< RDFSR 70h, 1 data byte
    static constexpr Instruction ReadAnyRegister { Opcode::RDAR, 3, Latency::MLATS, 0, false };        //!< RDAR 65h. Latency from CR2.MLATS

    /*
     * ===========================================
     * Registers (addresses from Table 12)
     * ===========================================
     */

    /*!
     * Status Register (Table 15). Bit 6 is reserved on this part.
     */
    struct SR : MRAM::SR {
      /// Bits that can be written and read back. WREN is read-only, bits 6 and 0 are reserved
      static constexpr uint8_t WRITABLE = WPEN | TBSEL | BPSEL_MASK;
    };

    /*!
     * Configuration Register 1: output driver strength, status register lock and WREN behaviour for
     * memory array writes (Table 21). Register writes always need WREN regardless.
     */
    struct CR1 : MRAM::CR1 {
      /*!
       * ODSEL[2:0]: output driver impedance in ohms, by VCCIO:
       *
       *     code  1.8V  2.5V  3.0V
       *     000   40    25    22
       *     001   40    25    22
       *     010   40    25    22
       *     011   40    25    22
       *     100   150   110   90
       *     101   80    50    45
       *     110   50    35    30     (factory default)
       *     111   40    25    22
       */
      static constexpr uint8_t ODSEL_SHIFT = 5;                    //!< ODSEL[2:0] position
      static constexpr uint8_t ODSEL_MASK = 0b111u << ODSEL_SHIFT; //!< ODSEL[2:0]

      // WRENS[1:0]. Code 11 is illegal.
      static constexpr uint8_t WRENS_SHIFT = 0;                           //!< WRENS[1:0] position
      static constexpr uint8_t WRENS_MASK = 0b11u << WRENS_SHIFT;         //!< WRENS[1:0]
      static constexpr uint8_t WRENS_NORMAL = 0b00u << WRENS_SHIFT;       //!< WREN before every array write; cleared when CS# goes high
      static constexpr uint8_t WRENS_SRAM = 0b01u << WRENS_SHIFT;         //!< WREN not needed (ignored) for array writes
      static constexpr uint8_t WRENS_BACK_TO_BACK = 0b10u << WRENS_SHIFT; //!< WREN before the first array write only; stays set until WRDI

      /*!
       * @brief   Encodes an ODSEL code.
       * @param   code  ODSEL code, 0–7 (see the table above)
       * @return  The ODSEL field, shifted into place
       */
      static constexpr uint8_t EncodeODSEL(uint8_t code) { return static_cast<uint8_t>((code << ODSEL_SHIFT) & ODSEL_MASK); }

      /// Bits that can be written and read back. Bits 4 and 3 are reserved
      static constexpr uint8_t WRITABLE = ODSEL_MASK | MAPLK | WRENS_MASK;
    };

    /*!
     * Configuration Register 2: XIP write and memory array / RDAR read latency (Table 22).
     */
    struct CR2 : MRAM::CR2 {
      static constexpr uint8_t XIPWR = 1u << 4; //!< 1 disables XIP writes, 0 enables them

      /// Bits that can be written and read back. Bits 7–5 are reserved
      static constexpr uint8_t WRITABLE = XIPWR | MLATS_MASK;
    };

    /*!
     * Flag Status Register, read-only (Table 19).
     */
    struct FSR {
      static constexpr uint32_t ADDRESS = 0x00000A; //!< Register address

      static constexpr uint8_t ST = 1u << 7; //!< 1 = ready, 0 = busy. Bits 6–0 are reserved and read 0
    };

    /*!
     * @brief   Bits that must always be written as 1 to a register.
     * @param   address  Register address
     * @return  Always 0. This part has no must-be-one bits
     */
    static constexpr uint8_t ForceOneBits(uint32_t address) { return (void)address, 0; }
  };

} // namespace OwlSat::MRAM
