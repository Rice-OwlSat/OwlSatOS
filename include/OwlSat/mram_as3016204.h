/**

  @file       mram_as3016204.h
  @brief      Datasheet facts for the Avalanche AS3016204 (16 Mb / 2 MiB, 3.0 V).
  @details    The part fitted to the current test board. Bench-tested over SPI on 23.09.2026.
              Table numbers are from the *1Mb–16Mb Serial HP MRAM* datasheet, revision P.

              Only facts about the chip live here. Values someone might want to change, like the
              flight register configuration, are in mram_config.h.

  @author     Viola Case
  @date       23.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include <OwlSat/mram_core.h>

namespace OwlSat::MRAM {

  /*!
   * The AS3016204. Has the same member names as AS3064204, so the driver can use either one
   * through Config::MRAMPart.
   */
  struct AS3016204 {
    static constexpr const char *NAME = "AS3016204"; //!< Part number, for logs

    static constexpr size_t CAPACITY = 2u * 1024u * 1024u; //!< Memory array size in bytes (16 Mb)

    /*!
     * RDID reply for the AS3016204-0108X0I (Table 18 worked example): manufacturer E6h, HP QSPI,
     * 3 V, −40…85 °C, 16 Mb, 108 MHz.
     */
    static constexpr std::array<uint8_t, 4> EXPECTED_ID { 0xE6, 0x01, 0x04, 0x01 };

    /// Bits of the RDID reply that must match EXPECTED_ID. The temperature grade nibble is ignored
    static constexpr std::array<uint8_t, 4> ID_MASK { 0xFF, 0xFF, 0x0F, 0xFF };

    /// RDAR has a fixed 8-cycle latency in 1-1-1 (Table 25), independent of CR2.MLATS
    static constexpr bool RDAR_USES_MLATS = false;

    /*
     * ===========================================
     * Part-only instructions (Table 28)
     * ===========================================
     */

    // Control instructions
    static constexpr Instruction EnterDeepPowerDown { Opcode::DPDE, 0, Latency::None, 0, false };  //!< DPDE B9h
    static constexpr Instruction EnterHibernate { Opcode::HBNE, 0, Latency::None, 0, false };      //!< HBNE BAh
    static constexpr Instruction SoftwareResetEnable { Opcode::SRTE, 0, Latency::None, 0, false }; //!< SRTE 66h
    static constexpr Instruction SoftwareReset { Opcode::SRST, 0, Latency::None, 0, false };       //!< SRST 99h. Needs SRTE first
    static constexpr Instruction ExitDeepPowerDown { Opcode::DPDX, 0, Latency::None, 0, false };   //!< DPDX ABh

    // Read register instructions
    static constexpr Instruction ReadConfigurationRegister1 { Opcode::RDC1, 0, Latency::None, 0, false }; //!< RDC1 35h, 1 data byte
    static constexpr Instruction ReadConfigurationRegister2 { Opcode::RDC2, 0, Latency::None, 0, false }; //!< RDC2 3Fh, 1 data byte
    static constexpr Instruction ReadConfigurationRegister3 { Opcode::RDC3, 0, Latency::None, 0, false }; //!< RDC3 44h, 1 data byte
    static constexpr Instruction ReadConfigurationRegister4 { Opcode::RDC4, 0, Latency::None, 0, false }; //!< RDC4 45h, 1 data byte
    static constexpr Instruction ReadConfigurationRegisters { Opcode::RDCX, 0, Latency::None, 0, false }; //!< RDCX 46h, 4 data bytes: CR1..CR4
    static constexpr Instruction ReadUniqueID { Opcode::RUID, 0, Latency::None, 0, false };               //!< RUID 4Ch, 8 data bytes
    static constexpr Instruction ReadSerialNumberRegister { Opcode::RDSN, 0, Latency::None, 0, false };   //!< RDSN C3h, 8 data bytes
    static constexpr Instruction ReadAugmentedArrayProtectionRegister { Opcode::RDAP, 0, Latency::None, 0, false }; //!< RDAP 14h, 1 data byte
    static constexpr Instruction ReadAnyRegister { Opcode::RDAR, 3, Latency::Fixed, 8, false }; //!< RDAR 65h. Fixed 8-cycle latency (Table 25)

    // Write register instructions
    static constexpr Instruction WriteConfigurationRegisters { Opcode::WRCX, 0, Latency::None, 0, true };           //!< WRCX 87h, 4 data bytes: CR1..CR4
    static constexpr Instruction WriteSerialNumberRegister { Opcode::WRSN, 0, Latency::None, 0, true };             //!< WRSN C2h, 8 data bytes
    static constexpr Instruction WriteAugmentedArrayProtectionRegister { Opcode::WRAP, 0, Latency::None, 0, true }; //!< WRAP 1Ah, 1 data byte

    // Augmented storage array instructions. The ASA has its own 256-byte address space (Table 9)
    static constexpr Instruction ReadAugmentedStorageArray { Opcode::RDAS, 3, Latency::MLATS, 0, false }; //!< RDAS 4Bh. Needs CR2.MLATS >= 8
    static constexpr Instruction WriteAugmentedStorageArray { Opcode::WRAS, 3, Latency::None, 0, true };  //!< WRAS 42h

    /*
     * ===========================================
     * Registers
     * ===========================================
     */

    /*!
     * Status Register (Table 12). Adds SNPEN to the common fields.
     */
    struct SR : MRAM::SR {
      static constexpr uint8_t SNPEN = 1u << 6; //!< Write-protects the serial number register

      /// Bits that can be written and read back. WREN is read-only, bit 0 is reserved
      static constexpr uint8_t WRITABLE = WPEN | SNPEN | TBSEL | BPSEL_MASK;
    };

    /*!
     * Configuration Register 1: protection locks (Table 20).
     */
    struct CR1 : MRAM::CR1 {
      static constexpr uint8_t ASPLK = 1u << 0; //!< Write-protects the whole augmented storage array

      /// Bits that can be written and read back
      static constexpr uint8_t WRITABLE = ASPLK | MAPLK;
    };

    /*!
     * Configuration Register 2: interface mode and memory array read latency (Table 21).
     */
    struct CR2 : MRAM::CR2 {
      // Read-only: the bus mode only changes through DPIE / QPIE / SPIE. Writing these bits does nothing.
      static constexpr uint8_t DPISL = 1u << 4; //!< Dual SPI (DPI 2-2-2) is active
      static constexpr uint8_t QPISL = 1u << 6; //!< Quad SPI (QPI 4-4-4) is active

      /// Bits that can be written and read back. DPISL and QPISL are read-only
      static constexpr uint8_t WRITABLE = MLATS_MASK;
    };

    /*!
     * Configuration Register 3: output driver strength and read wrap (Table 26).
     */
    struct CR3 {
      static constexpr uint32_t ADDRESS = 0x000004; //!< Register address

      /*!
       * ODSEL[2:0]: output driver impedance. The same code gives a different impedance on 1.8V and
       * 3.0V parts:
       *
       *     code  1.8V   3.0V
       *     0     45Ω*   35Ω
       *     1     120Ω   75Ω
       *     2     90Ω    60Ω
       *     3     70Ω    45Ω*
       *     4     45Ω    35Ω
       *     5     60Ω    40Ω
       *     6     30Ω    20Ω
       *     7     20Ω    15Ω      (* = factory default)
       */
      static constexpr uint8_t ODSEL_SHIFT = 5;                    //!< ODSEL[2:0] position
      static constexpr uint8_t ODSEL_MASK = 0b111u << ODSEL_SHIFT; //!< ODSEL[2:0]

      static constexpr uint8_t WRAPS = 1u << 4; //!< Enables read wrap within the boundary selected by WRPLS

      // WRPLS[2:0]: aligned boundary that reads wrap within. Codes 101–111 are reserved.
      static constexpr uint8_t WRPLS_SHIFT = 0;                     //!< WRPLS[2:0] position
      static constexpr uint8_t WRPLS_MASK = 0b111u << WRPLS_SHIFT;  //!< WRPLS[2:0]
      static constexpr uint8_t WRPLS_16 = 0b000u << WRPLS_SHIFT;    //!< Wrap within 16 bytes
      static constexpr uint8_t WRPLS_32 = 0b001u << WRPLS_SHIFT;    //!< Wrap within 32 bytes
      static constexpr uint8_t WRPLS_64 = 0b010u << WRPLS_SHIFT;    //!< Wrap within 64 bytes
      static constexpr uint8_t WRPLS_128 = 0b011u << WRPLS_SHIFT;   //!< Wrap within 128 bytes
      static constexpr uint8_t WRPLS_256 = 0b100u << WRPLS_SHIFT;   //!< Wrap within 256 bytes

      /*!
       * @brief   Encodes an ODSEL code.
       * @param   code  ODSEL code, 0–7 (see the table above)
       * @return  The ODSEL field, shifted into place
       */
      static constexpr uint8_t EncodeODSEL(uint8_t code) { return static_cast<uint8_t>((code << ODSEL_SHIFT) & ODSEL_MASK); }

      /// Bits that can be written and read back. Bit 3 is reserved
      static constexpr uint8_t WRITABLE = ODSEL_MASK | WRAPS | WRPLS_MASK;
    };

    /*!
     * Configuration Register 4: whether memory array and augmented storage writes need WREN first.
     * Register writes always need WREN regardless (Table 27).
     */
    struct CR4 {
      static constexpr uint32_t ADDRESS = 0x000005; //!< Register address

      /// Reserved, but must always be written as 1. WriteRegister() ORs it in automatically
      static constexpr uint8_t MUST_BE_ONE = 1u << 2;

      // WRENS[1:0]. Code 11 is illegal.
      static constexpr uint8_t WRENS_SHIFT = 0;                       //!< WRENS[1:0] position
      static constexpr uint8_t WRENS_MASK = 0b11u << WRENS_SHIFT;     //!< WRENS[1:0]
      static constexpr uint8_t WRENS_NORMAL = 0b00u << WRENS_SHIFT;   //!< WREN before every array write; cleared when CS# goes high
      static constexpr uint8_t WRENS_SRAM = 0b01u << WRENS_SHIFT;     //!< WREN not needed (ignored) for array writes
      static constexpr uint8_t WRENS_BACK_TO_BACK = 0b10u << WRENS_SHIFT; //!< WREN before the first array write only; stays set until WRDI

      /// Bits that can be written and read back
      static constexpr uint8_t WRITABLE = MUST_BE_ONE | WRENS_MASK;
    };

    /*!
     * @brief   Bits that must always be written as 1 to a register.
     * @param   address  Register address
     * @return  CR4::MUST_BE_ONE for CR4, 0 for everything else
     */
    static constexpr uint8_t ForceOneBits(uint32_t address) { return address == CR4::ADDRESS ? CR4::MUST_BE_ONE : 0; }
  };

} // namespace OwlSat::MRAM
