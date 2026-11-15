/**

  @file       mram.h
  @brief      SPI MRAM chip driver — nonvolatile store.
  @details    Driver API for Avalanche AS3016204 (16MiB chip)

              The part works over SPI straight out of the box (bench-tested 23.09.2026).
              Datasheet (1Mb–16Mb Serial HP MRAM):
              https://www.mouser.com/datasheet/3/4808/1/1Mb_16Mb_Serial_HP_MRAM.pdf

  @author     Viola Case
  @date       23.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <OwlSat/utility_macros.h>


namespace OwlSat {

  namespace Config {
    constexpr uint8_t CR1_DEFAULT = 0x04;
    constexpr uint8_t CR2_DEFAULT = 0x00;
    constexpr uint8_t CR3_DEFAULT = 0x60;
    constexpr uint8_t CR4_DEFAULT = 0x04;



  } // namespace Config

  namespace MRAM {

    extern enum struct Mode : uint8_t {
      M1_1_1,
      M2_2_2,
      M4_4_4,
    } BUS_MODE;

    bool SwitchMode(Mode mode);

    /*!
     * Opcodes taken from Avalanche Technology's
     * *High Performance Serial Persistent SRAM Memory* datasheet, revision P
     */
    enum struct Opcode : uint8_t {
      /*
       * ===========================================
       * Control instructions
       * ===========================================
       */
      NOOP = 0x00, //!< No operation
      WREN = 0x06, //!< Write enable
      WRDI = 0x04, //!< Write disable
      DPIE = 0x37, //!< Enable DPI
      QPIE = 0x38, //!< Enable QPI
      SPIE = 0xFF, //!< Enable SPI
      DPDE = 0xB9, //!< Enter deep power down
      HBNE = 0xBA, //!< Enter hibernate
      SRTE = 0x66, //!< Software reset enable
      SRST = 0x99, //!< Software reset
      DPDX = 0xAB, //!< Exit deep power down
      /*
       * ===========================================
       * Read register instructions
       * ===========================================
       */
      RDSR = 0x05, //!< Read status register
      RDC1 = 0x35, //!< Read configuration register 1
      RDC2 = 0x3F, //!< Read configuration register 2
      RDC3 = 0x44, //!< Read configuration register 3
      RDC4 = 0x45, //!< Read configuration register 4
      RDCX = 0x46, //!< Read configuration register 1, 2, 3, 4
      RDID = 0x9F, //!< Read device ID
      RUID = 0x4C, //!< Read unique ID
      RDSN = 0xC3, //!< Read serial number register
      RDAP = 0x14, //!< Read augmented array protection register
      RDAR = 0x65, //!< Read any register - address based
      /*
       * ===========================================
       * Write register instructions
       * ===========================================
       */
      WRSR = 0x01, //!< Write status register
      WRCX = 0x87, //!< Write configuration register 1, 2, 3, 4
      WRSN = 0xC2, //!< Write serial number register
      WRAP = 0x1A, //!< Write augmented array protection register
      WRAR = 0x71, //!< Write any register - address based
      /*
       * ===========================================
       * Read memory array instructions
       * ===========================================
       */
      READ = 0x03, //!< Read Memory Array - SDR
      RDFT = 0x0B, //!< Fast Read Memory Array - SDR
      DRFR = 0x0D, //!< Fast Read Memory Array - DDR
      RDDO = 0x3B, //!< Read Dual Output Memory Array - SDR
      RDQO = 0x6B, //!< Read Quad Output Memory Array - SDR
      RDDI = 0xBB, //!< Read Dual I/O Memory Read - SDR
      DRDI = 0xBD, //!< Read Dual I/O Memory Read - DDR
      RDQI = 0xEB, //!< Read Quad I/O Memory Read - SDR
      DRQI = 0xED, //!< Read Quad I/O Memory Read - DDR
      /*
       * ===========================================
       * Write memory array instructions
       * ===========================================
       */
      WRTE = 0x02, //!< Write Memory Array - SDR
      WRFT = 0xDA, //!< Fast Write Memory Array - SDR
      DRFW = 0xDE, //!< Fast Write Memory Array - DDR
      WDUI = 0xA2, //!< Write Dual Input Memory Array - SDR
      WQDI = 0x32, //!< Write Quad Input Memory Array - SDR
      DWQI = 0x31, //!< Write Quad Input Memory Array - DDR
      WDIO = 0xA1, //!< Write Dual I/O Memory Array - SDR
      WQIO = 0xD2, //!< Write Quad I/O Memory Array - SDR
      DWQO = 0xD1, //!< Write Quad I/O Memory Array - DDR
      /*
       * ===========================================
       * Augmented storage array instructions
       * ===========================================
       */
      RDAS = 0x4B, //!< Read Augmented Storage Array - SDR
      WRAS = 0x42  //!< Write Augmented Storage Array - SDR
    };

    /*!
     * Datasheet Table 28, limited to the instructions that run in single-lane SPI (1-0-0, 1-0-1,
     * 1-1-1 SDR), since that's the only mode Execute() drives. Address width is in bytes. Latency
     * is in clock cycles, between the address and the data.
     *
     * RDFT and RDAS take their latency from CR2.MLATS, so CR2 must be set to at least the 8 cycles
     * stored here (Tables 22, 24). RDAR's 8-cycle latency is fixed (Table 25).
     */
    class Instruction {
    public:
      static constexpr size_t MAX_BELT_SIZE = 256;

      // Control instructions
      static const Instruction NoOperation;         //<!
      static const Instruction WriteEnable;         //<!
      static const Instruction WriteDisable;        //<!
      static const Instruction EnterDeepPowerDown;  //<!
      static const Instruction EnterHibernate;      //<!
      static const Instruction SoftwareResetEnable; //<!
      static const Instruction SoftwareReset;       //<!
      static const Instruction ExitDeepPowerDown;   //<!

      // Read register instructions
      static const Instruction ReadStatusRegister;                   //<!
      static const Instruction ReadConfigurationRegister1;           //<!
      static const Instruction ReadConfigurationRegister2;           //<!
      static const Instruction ReadConfigurationRegister3;           //<!
      static const Instruction ReadConfigurationRegister4;           //<!
      static const Instruction ReadConfigurationRegisters;           //<!
      static const Instruction ReadDeviceID;                         //<!
      static const Instruction ReadUniqueID;                         //<!
      static const Instruction ReadSerialNumberRegister;             //<!
      static const Instruction ReadAugmentedArrayProtectionRegister; //<!
      static const Instruction ReadAnyRegister;                      //<!

      // Write register instructions
      static const Instruction WriteStatusRegister;                   //<!
      static const Instruction WriteConfigurationRegisters;           //<!
      static const Instruction WriteSerialNumberRegister;             //<!
      static const Instruction WriteAugmentedArrayProtectionRegister; //<!
      static const Instruction WriteAnyRegister;                      //<!

      // Read memory array instructions
      static const Instruction ReadMemoryArray;     //<!
      static const Instruction FastReadMemoryArray; //<! Needs CR2.MLATS >= 8

      // Write memory array instructions
      static const Instruction WriteMemoryArray;     //<!
      static const Instruction FastWriteMemoryArray; //<!

      // Augmented storage array instructions
      static const Instruction ReadAugmentedStorageArray;  //<!
      static const Instruction WriteAugmentedStorageArray; //<!

    private:
      Opcode m_opcode;           //!< Command byte, always sent first
      size_t m_address_width;    //!< Address bytes after the opcode, MSB first. 0 = no address, 3 = 24-bit
      size_t m_latency_cycles;   //!< Clock cycles between the address and the data. 8 cycles = 1 dummy byte in 1-1-1
      bool m_needs_write_enable; //!< WREN (06h) must go out in its own CS# cycle first. Array writes depend on CR4.WRENS

      friend bool Execute(const Instruction &instr, uintptr_t addr, std::span<const uint8_t> tx_data, std::span<uint8_t> rx_data);


      constexpr Instruction(Opcode opcode, size_t addrwidth, size_t latency_cycles, bool needs_write_enable = false)
          : m_opcode(opcode), m_address_width(addrwidth), m_latency_cycles(latency_cycles), m_needs_write_enable(needs_write_enable) {}
    };

#pragma region instructions
    // clang-format off
    // Control instructions
    constexpr Instruction Instruction::NoOperation                                { Opcode::NOOP, 0, 0 };
    constexpr Instruction Instruction::WriteEnable                                { Opcode::WREN, 0, 0 };
    constexpr Instruction Instruction::WriteDisable                               { Opcode::WRDI, 0, 0 };
    constexpr Instruction Instruction::EnterDeepPowerDown                         { Opcode::DPDE, 0, 0 };
    constexpr Instruction Instruction::EnterHibernate                             { Opcode::HBNE, 0, 0 };
    constexpr Instruction Instruction::SoftwareResetEnable                        { Opcode::SRTE, 0, 0 };
    constexpr Instruction Instruction::SoftwareReset                              { Opcode::SRST, 0, 0 };
    constexpr Instruction Instruction::ExitDeepPowerDown                          { Opcode::DPDX, 0, 0 };

    // Read register instructions
    constexpr Instruction Instruction::ReadStatusRegister                         { Opcode::RDSR, 0, 0 };
    constexpr Instruction Instruction::ReadConfigurationRegister1                 { Opcode::RDC1, 0, 0 };
    constexpr Instruction Instruction::ReadConfigurationRegister2                 { Opcode::RDC2, 0, 0 };
    constexpr Instruction Instruction::ReadConfigurationRegister3                 { Opcode::RDC3, 0, 0 };
    constexpr Instruction Instruction::ReadConfigurationRegister4                 { Opcode::RDC4, 0, 0 };
    constexpr Instruction Instruction::ReadConfigurationRegisters                 { Opcode::RDCX, 0, 0 };
    constexpr Instruction Instruction::ReadDeviceID                               { Opcode::RDID, 0, 0 };
    constexpr Instruction Instruction::ReadUniqueID                               { Opcode::RUID, 0, 0 };
    constexpr Instruction Instruction::ReadSerialNumberRegister                   { Opcode::RDSN, 0, 0 };
    constexpr Instruction Instruction::ReadAugmentedArrayProtectionRegister       { Opcode::RDAP, 0, 0 };
    constexpr Instruction Instruction::ReadAnyRegister                            { Opcode::RDAR, 3, 8 }; //!< Fixed latency, not set by MLATS

    // Write register instructions
    constexpr Instruction Instruction::WriteStatusRegister                        { Opcode::WRSR, 0, 0, true };
    constexpr Instruction Instruction::WriteConfigurationRegisters                { Opcode::WRCX, 0, 0, true };
    constexpr Instruction Instruction::WriteSerialNumberRegister                  { Opcode::WRSN, 0, 0, true };
    constexpr Instruction Instruction::WriteAugmentedArrayProtectionRegister      { Opcode::WRAP, 0, 0, true };
    constexpr Instruction Instruction::WriteAnyRegister                           { Opcode::WRAR, 3, 0, true };

    // Read memory array instructions
    constexpr Instruction Instruction::ReadMemoryArray                            { Opcode::READ, 3, 0 };
    constexpr Instruction Instruction::FastReadMemoryArray                        { Opcode::RDFT, 3, 8 };

    // Write memory array instructions
    constexpr Instruction Instruction::WriteMemoryArray                           { Opcode::WRTE, 3, 0, true };
    constexpr Instruction Instruction::FastWriteMemoryArray                       { Opcode::WRFT, 3, 0, true };

    // Augmented storage array instructions
    constexpr Instruction Instruction::ReadAugmentedStorageArray                  { Opcode::RDAS, 3, 8 };
    constexpr Instruction Instruction::WriteAugmentedStorageArray                 { Opcode::WRAS, 3, 0, true };

    //clang-format on
#pragma endregion

    bool Execute(const Instruction &instr, uintptr_t addr, std::span<const uint8_t> tx_data, std::span<uint8_t> rx_data);

    /*
     * ===========================================
     * Register bit masks
     * ===========================================
     * Single-bit flags are plain masks. Multi-bit fields get a _MASK, a _SHIFT, and pre-shifted
     * values for their codes. ADDRESS is for RDAR 65h / WRAR 71h (Table 11).
     */

    /*!
     * Status Register. Datasheet Table 12.
     */
    namespace SR {
      constexpr uint32_t ADDRESS = 0x000000;

      constexpr uint8_t WPEN = 1u << 7;  //!< WP# pin low write-protects SR and the CRs (Table 15)
      constexpr uint8_t SNPEN = 1u << 6; //!< Write-protects the serial number register
      constexpr uint8_t TBSEL = 1u << 5; //!< Block protect from the bottom (1) or top (0)
      constexpr uint8_t WREN = 1u << 1;  //!< Write enable latch. Read-only, set by WREN 06h

      // BPSEL[2:0] — portion of the array that is write-protected. Codes 1–6 protect 1/64 up to 1/2.
      constexpr uint8_t BPSEL_SHIFT = 2;
      constexpr uint8_t BPSEL_MASK = 0b111u << BPSEL_SHIFT;
      constexpr uint8_t BPSEL_NONE = 0b000u << BPSEL_SHIFT;
      constexpr uint8_t BPSEL_ALL = 0b111u << BPSEL_SHIFT;
    } // namespace SR

    /*!
     * Configuration Register 1 — protection locks. Datasheet Table 20.
     */
    namespace CR1 {
      constexpr uint32_t ADDRESS = 0x000002;

      constexpr uint8_t ASPLK = 1u << 0; //!< Write protects the whole augmented storage array
      constexpr uint8_t MAPLK = 1u << 2; //!< Locks TBSEL and BPSEL[2:0] in the status register
    } // namespace CR1

    /*!
     * Configuration Register 2 — interface mode and memory array read latency. Datasheet Table 21.
     */
    namespace CR2 {
      constexpr uint32_t ADDRESS = 0x000003;

      /*!
       * MLATS[3:0] — latency cycles (0–15) inserted before data on memory array reads.
       * Plain READ (03h) needs none but tops out at 50MHz. Fast reads need 8–15 cycles for 1-1-1,
       * 1-1-2, 1-2-2 and 2-2-2, and 12–15 for 1-1-4, 1-4-4 and 4-4-4. Datasheet Tables 22–24.
       */
      constexpr uint8_t MLATS_SHIFT = 0;
      constexpr uint8_t MLATS_MASK = 0x0Fu << MLATS_SHIFT;

      // Read-only: the bus mode only changes through the DPIE / QPIE / SPIE instructions
      // (37h / 38h / FFh). Writing these bits does nothing.
      constexpr uint8_t DPISL = 1u << 4; //!< Dual SPI (DPI 2-2-2) is active
      constexpr uint8_t QPISL = 1u << 6; //!< Quad SPI (QPI 4-4-4) is active
    } // namespace CR2

    /*!
     * Configuration Register 3 — output driver strength and read wrap. Datasheet Table 26.
     */
    namespace CR3 {
      constexpr uint32_t ADDRESS = 0x000004;

      /*!
       * ODSEL[2:0] — output driver impedance. The same code gives a different impedance on 1.8V
       * and 3.0V parts:
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
      constexpr uint8_t ODSEL_SHIFT = 5;
      constexpr uint8_t ODSEL_MASK = 0b111u << ODSEL_SHIFT;

      constexpr uint8_t WRAPS = 1u << 4; //!< Enables read wrap within the boundary selected by WRPLS

      // WRPLS[2:0] — aligned boundary that reads wrap within. Codes 101–111 are reserved.
      constexpr uint8_t WRPLS_SHIFT = 0;
      constexpr uint8_t WRPLS_MASK = 0b111u << WRPLS_SHIFT;
      constexpr uint8_t WRPLS_16 = 0b000u << WRPLS_SHIFT;
      constexpr uint8_t WRPLS_32 = 0b001u << WRPLS_SHIFT;
      constexpr uint8_t WRPLS_64 = 0b010u << WRPLS_SHIFT;
      constexpr uint8_t WRPLS_128 = 0b011u << WRPLS_SHIFT;
      constexpr uint8_t WRPLS_256 = 0b100u << WRPLS_SHIFT;
    } // namespace CR3

    /*!
     * Configuration Register 4 — whether memory array and augmented storage writes need WREN
     * first. Register writes always need WREN regardless. Datasheet Table 27.
     */
    namespace CR4 {
      constexpr uint32_t ADDRESS = 0x000005;

      /// Reserved, but must always be written as 1. OR it into every value written to CR4.
      constexpr uint8_t MUST_BE_ONE = 1u << 2;

      // WRENS[1:0]. Code 11 is illegal.
      constexpr uint8_t WRENS_SHIFT = 0;
      constexpr uint8_t WRENS_MASK = 0b11u << WRENS_SHIFT;
      constexpr uint8_t WRENS_NORMAL = 0b00u << WRENS_SHIFT;       //!< WREN before every array write; cleared when CS# goes high
      constexpr uint8_t WRENS_SRAM = 0b01u << WRENS_SHIFT;         //!< WREN not needed (ignored) for array writes. Factory default
      constexpr uint8_t WRENS_BACK_TO_BACK = 0b10u << WRENS_SHIFT; //!< WREN before the first array write only; stays set until WRDI
    } // namespace CR4


    uint8_t ReadSR();
    uint8_t ReadCR1();
    uint8_t ReadCR2();
    uint8_t ReadCR3();
    uint8_t ReadCR4();



  } // namespace MRAM


} // namespace OwlSat
