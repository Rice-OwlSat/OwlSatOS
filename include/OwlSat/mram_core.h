/**

  @file       mram_core.h
  @brief      What every supported Avalanche serial MRAM has in common.
  @details    The opcodes, the Instruction type that describes one CS# cycle on the bus, the
              transport entry point Execute(), and the register fields that sit at the same address
              and bit position on every supported part.

              Part-specific facts live in mram_as3016204.h and mram_as3064204.h. Editable values
              live in mram_config.h. The driver API is in mram.h, which is the header to include.

              Datasheets:
              - AS3016204: *1Mb–16Mb Serial HP MRAM*, revision P
                (https://www.mouser.com/datasheet/3/4808/1/1Mb_16Mb_Serial_HP_MRAM.pdf)
              - AS3064204: *64Mbit–128Mbit Dual QSPI MRAM Memory, Space-Grade-E*, revision C.8

  @author     Viola Case
  @date       23.09.2026
  @copyright  © Viola Case, 2026. All rights reserved.

**/
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace OwlSat::MRAM {

  struct AS3016204;
  struct AS3064204;

  /*
   * ===========================================
   * Bus mode
   * ===========================================
   */

  /*!
   * Interface mode of the MRAM, in (command-address-data) lane counts.
   */
  enum struct Mode : uint8_t {
    M1_1_1, //!< Single SPI. The only mode Execute() drives
    M2_2_2, //!< DPI. AS3016204 only
    M4_4_4, //!< QPI
  };

  /*!
   * The mode the driver believes the chip is in. Execute() refuses to run unless this is
   * Mode::M1_1_1.
   */
  extern Mode BUS_MODE;

  /*!
   * @brief   Switches the chip and the driver to another interface mode. **Not implemented.**
   * @param   mode  The mode to switch to
   * @return  Whether the switch succeeded
   */
  bool SwitchMode(Mode mode);

  /*
   * ===========================================
   * Opcodes
   * ===========================================
   */

  /*!
   * Command bytes, from the AS3016204 datasheet (revision P, Table 28) and the AS3064204 datasheet
   * (revision C.8, Table 26). Each entry names the part(s) that support it. Where both parts have an
   * opcode, it means the same thing on both.
   *
   * An opcode being listed here does not make it usable: only the Instruction objects in this header
   * and in the part headers can be sent.
   */
  enum struct Opcode : uint8_t {
    /*
     * ===========================================
     * Control instructions
     * ===========================================
     */
    NOOP = 0x00, //!< No operation. Both parts
    WREN = 0x06, //!< Write enable. Both parts
    WRDI = 0x04, //!< Write disable. Both parts
    DPIE = 0x37, //!< Enable DPI. AS3016204 only
    QPIE = 0x38, //!< Enable QPI. Both parts
    SPIE = 0xFF, //!< Enable SPI. Both parts
    DPDE = 0xB9, //!< Enter deep power down. AS3016204 only
    HBNE = 0xBA, //!< Enter hibernate. AS3016204 only
    SRTE = 0x66, //!< Software reset enable. AS3016204 only
    SRST = 0x99, //!< Software reset. AS3016204 only
    DPDX = 0xAB, //!< Exit deep power down. AS3016204 only
    /*
     * ===========================================
     * Read register instructions
     * ===========================================
     */
    RDSR = 0x05, //!< Read status register. Both parts
    RDFS = 0x70, //!< Read flag status register. AS3064204 only
    RDC1 = 0x35, //!< Read configuration register 1. AS3016204 only
    RDC2 = 0x3F, //!< Read configuration register 2. AS3016204 only
    RDC3 = 0x44, //!< Read configuration register 3. AS3016204 only
    RDC4 = 0x45, //!< Read configuration register 4. AS3016204 only
    RDCX = 0x46, //!< Read configuration registers 1, 2, 3, 4. AS3016204 only
    RDID = 0x9F, //!< Read device ID. Both parts
    RUID = 0x4C, //!< Read unique ID. AS3016204 only
    RDSN = 0xC3, //!< Read serial number register. AS3016204 only
    RDAP = 0x14, //!< Read augmented array protection register. AS3016204 only
    RDAR = 0x65, //!< Read any register, address based. Both parts, with different latency rules
    /*
     * ===========================================
     * Write register instructions
     * ===========================================
     */
    WRSR = 0x01, //!< Write status register. Both parts
    WRCX = 0x87, //!< Write configuration registers 1, 2, 3, 4. AS3016204 only
    WRSN = 0xC2, //!< Write serial number register. AS3016204 only
    WRAP = 0x1A, //!< Write augmented array protection register. AS3016204 only
    WRAR = 0x71, //!< Write any register, address based. Both parts
    /*
     * ===========================================
     * Read memory array instructions
     * ===========================================
     */
    READ = 0x03, //!< Read memory array, SDR. Both parts
    RDFT = 0x0B, //!< Fast read memory array, SDR. Both parts
    DRFR = 0x0D, //!< Fast read memory array, DDR. Both parts
    RDDO = 0x3B, //!< Read dual output memory array, SDR. AS3016204 only
    RDQO = 0x6B, //!< Read quad output memory array, SDR. Both parts
    RDDI = 0xBB, //!< Read dual I/O memory array, SDR. AS3016204 only
    DRDI = 0xBD, //!< Read dual I/O memory array, DDR. AS3016204 only
    RDQI = 0xEB, //!< Read quad I/O memory array, SDR. Both parts
    DRQI = 0xED, //!< Read quad I/O memory array, DDR. Both parts
    /*
     * ===========================================
     * Write memory array instructions
     * ===========================================
     */
    WRTE = 0x02, //!< Write memory array, SDR. Both parts
    WRFT = 0xDA, //!< Fast write memory array, SDR. Both parts
    DRFW = 0xDE, //!< Fast write memory array, DDR. Both parts
    WDUI = 0xA2, //!< Write dual input memory array, SDR. AS3016204 only
    WQDI = 0x32, //!< Write quad input memory array, SDR. AS3016204 only
    DWQI = 0x31, //!< Write quad input memory array, DDR. AS3016204 only
    WDIO = 0xA1, //!< Write dual I/O memory array, SDR. AS3016204 only
    WQIO = 0xD2, //!< Write quad I/O memory array, SDR. Both parts
    DWQO = 0xD1, //!< Write quad I/O memory array, DDR. Both parts
    /*
     * ===========================================
     * Augmented storage array instructions
     * ===========================================
     */
    RDAS = 0x4B, //!< Read augmented storage array, SDR. AS3016204 only
    WRAS = 0x42  //!< Write augmented storage array, SDR. AS3016204 only
  };

  /*
   * ===========================================
   * Instructions
   * ===========================================
   */

  /*!
   * Where an instruction's latency cycles (between the address and the data) come from.
   */
  enum struct Latency : uint8_t {
    None,  //!< No latency cycles
    Fixed, //!< A fixed number of cycles, stored in the Instruction
    MLATS, //!< Whatever CR2.MLATS is set to. Execute() uses the driver's tracked copy of it
  };

  /*!
   * One instruction in single-lane SPI (1-0-0, 1-0-1 or 1-1-1 SDR), which is the only mode Execute()
   * drives. An instruction is one CS# low→high cycle: the opcode, then the address (MSB first), then
   * the latency cycles, then the data.
   *
   * The constructor is private. Only Instruction itself (the instructions every part shares) and the
   * part structs can create instructions, so the set the driver can send stays curated.
   */
  class Instruction {
  public:
    // Control instructions
    static const Instruction NoOperation;  //!< NOOP 00h
    static const Instruction WriteEnable;  //!< WREN 06h. Sets SR.WREN
    static const Instruction WriteDisable; //!< WRDI 04h. Clears SR.WREN

    // Read register instructions
    static const Instruction ReadStatusRegister; //!< RDSR 05h, 1 data byte
    static const Instruction ReadDeviceID;       //!< RDID 9Fh, 4 data bytes

    // Write register instructions
    static const Instruction WriteStatusRegister; //!< WRSR 01h, 1 data byte. Needs WREN
    static const Instruction WriteAnyRegister;    //!< WRAR 71h, 24-bit register address, 1 data byte. Needs WREN

    // Read memory array instructions
    static const Instruction ReadMemoryArray;     //!< READ 03h. No latency, 50 MHz max
    static const Instruction FastReadMemoryArray; //!< RDFT 0Bh. Latency from CR2.MLATS

    // Write memory array instructions
    static const Instruction WriteMemoryArray;     //!< WRTE 02h. WREN is sent first
    static const Instruction FastWriteMemoryArray; //!< WRFT DAh. WREN is sent first

    /*!
     * @brief   How many data bytes fit in one CS# cycle of this instruction, given the transfer
     *          buffer size in mram_config.h and the current latency.
     * @return  The maximum data length Execute() accepts for this instruction. 0 if the latency
     *          can't be driven in 1-1-1 at all.
     */
    [[nodiscard]] size_t MaxDataBytes() const;

  private:
    Opcode m_opcode;           //!< Command byte, always sent first
    uint8_t m_address_width;   //!< Address bytes after the opcode, MSB first. 0 = no address, 3 = 24-bit
    uint8_t m_latency_cycles;  //!< Clock cycles between the address and the data, when m_latency is Latency::Fixed
    Latency m_latency;         //!< Where the latency cycle count comes from
    bool m_needs_write_enable; //!< WREN (06h) goes out in its own CS# cycle first

    friend bool Execute(const Instruction &instr, uintptr_t addr, std::span<const uint8_t> tx_data, std::span<uint8_t> rx_data);
    friend struct AS3016204;
    friend struct AS3064204;

    /*!
     * @param opcode              Command byte
     * @param address_width       Address bytes after the opcode
     * @param latency             Where the latency cycle count comes from
     * @param latency_cycles      Cycle count for Latency::Fixed. Ignored otherwise
     * @param needs_write_enable  Whether WREN must be sent in its own CS# cycle first
     */
    constexpr Instruction(Opcode opcode, uint8_t address_width, Latency latency, uint8_t latency_cycles, bool needs_write_enable)
        : m_opcode(opcode), m_address_width(address_width), m_latency_cycles(latency_cycles), m_latency(latency), m_needs_write_enable(needs_write_enable) {}

    /// Opcode, address and latency bytes in 1-1-1, or 0 if the latency isn't a whole number of bytes
    [[nodiscard]] size_t HeaderBytes() const;
  };

#pragma region instructions
  // clang-format off
  // Control instructions
  inline constexpr Instruction Instruction::NoOperation          { Opcode::NOOP, 0, Latency::None,  0, false };
  inline constexpr Instruction Instruction::WriteEnable          { Opcode::WREN, 0, Latency::None,  0, false };
  inline constexpr Instruction Instruction::WriteDisable         { Opcode::WRDI, 0, Latency::None,  0, false };

  // Read register instructions
  inline constexpr Instruction Instruction::ReadStatusRegister   { Opcode::RDSR, 0, Latency::None,  0, false };
  inline constexpr Instruction Instruction::ReadDeviceID         { Opcode::RDID, 0, Latency::None,  0, false };

  // Write register instructions
  inline constexpr Instruction Instruction::WriteStatusRegister  { Opcode::WRSR, 0, Latency::None,  0, true  };
  inline constexpr Instruction Instruction::WriteAnyRegister     { Opcode::WRAR, 3, Latency::None,  0, true  };

  // Read memory array instructions
  inline constexpr Instruction Instruction::ReadMemoryArray      { Opcode::READ, 3, Latency::None,  0, false };
  inline constexpr Instruction Instruction::FastReadMemoryArray  { Opcode::RDFT, 3, Latency::MLATS, 0, false };

  // Write memory array instructions
  inline constexpr Instruction Instruction::WriteMemoryArray     { Opcode::WRTE, 3, Latency::None,  0, true  };
  inline constexpr Instruction Instruction::FastWriteMemoryArray { Opcode::WRFT, 3, Latency::None,  0, true  };
  // clang-format on
#pragma endregion

  /*!
   * @brief   Runs one instruction: one CS# low→high cycle, full duplex, in 1-1-1 SPI.
   * @details If the instruction needs WREN, WREN is sent first in its own CS# cycle, because the chip
   *          only latches it when CS# rises.
   * @param   instr    The instruction to send
   * @param   addr     Address sent after the opcode. Ignored if the instruction has no address
   * @param   tx_data  Bytes sent after the address and latency cycles
   * @param   rx_data  Bytes received after the address and latency cycles
   * @return  False, without touching the bus, if BUS_MODE isn't Mode::M1_1_1, the latency isn't a
   *          whole number of bytes, or the transfer doesn't fit in Config::MRAM_MAX_BELT_SIZE.
   */
  bool Execute(const Instruction &instr, uintptr_t addr, std::span<const uint8_t> tx_data, std::span<uint8_t> rx_data);

  namespace detail {
    /*!
     * @brief   The driver's copy of CR2.MLATS, used for Latency::MLATS instructions.
     * @return  Latency cycles
     */
    uint8_t MemoryLatency();

    /*!
     * @brief   Updates the driver's copy of CR2.MLATS. Called by the driver whenever it learns or
     *          writes CR2. Does not touch the chip.
     * @param   cycles  Latency cycles, 0–15
     */
    void SetMemoryLatency(uint8_t cycles);
  } // namespace detail

  /*
   * ===========================================
   * Common register fields
   * ===========================================
   * Fields that sit at the same address and bit on every supported part. Single-bit flags are plain
   * masks. Multi-bit fields get a _MASK, a _SHIFT, and pre-shifted values for their codes. ADDRESS is
   * for RDAR 65h / WRAR 71h.
   *
   * These are structs, not namespaces, so each part struct can extend them with its own bits.
   */

  /*!
   * Status Register. AS3016204 Table 12, AS3064204 Table 15.
   */
  struct SR {
    static constexpr uint32_t ADDRESS = 0x000000; //!< Register address

    static constexpr uint8_t WPEN = 1u << 7;  //!< WP# pin low write-protects SR and the CRs
    static constexpr uint8_t TBSEL = 1u << 5; //!< Block protect from the bottom (1) or top (0)
    static constexpr uint8_t WREN = 1u << 1;  //!< Write enable latch. Read-only, set by WREN 06h

    // BPSEL[2:0]: portion of the array that is write-protected. Codes 1–6 protect 1/64 up to 1/2.
    static constexpr uint8_t BPSEL_SHIFT = 2;                        //!< BPSEL[2:0] position
    static constexpr uint8_t BPSEL_MASK = 0b111u << BPSEL_SHIFT;     //!< BPSEL[2:0]
    static constexpr uint8_t BPSEL_NONE = 0b000u << BPSEL_SHIFT;     //!< Nothing protected
    static constexpr uint8_t BPSEL_HALF = 0b110u << BPSEL_SHIFT;     //!< Half the array protected
    static constexpr uint8_t BPSEL_ALL = 0b111u << BPSEL_SHIFT;      //!< The whole array protected
  };

  /*!
   * Configuration Register 1, the fields both parts share. AS3016204 Table 20, AS3064204 Table 21.
   */
  struct CR1 {
    static constexpr uint32_t ADDRESS = 0x000002; //!< Register address

    static constexpr uint8_t MAPLK = 1u << 2; //!< Locks SR.TBSEL and SR.BPSEL[2:0]
  };

  /*!
   * Configuration Register 2, the fields both parts share. AS3016204 Table 21, AS3064204 Table 22.
   */
  struct CR2 {
    static constexpr uint32_t ADDRESS = 0x000003; //!< Register address

    /*!
     * MLATS[3:0]: latency cycles (0–15) inserted before data on fast memory array reads. On the
     * AS3064204 it also sets the RDAR latency.
     */
    static constexpr uint8_t MLATS_SHIFT = 0;                   //!< MLATS[3:0] position
    static constexpr uint8_t MLATS_MASK = 0x0Fu << MLATS_SHIFT; //!< MLATS[3:0]

    /*!
     * @brief   Encodes a latency into the MLATS field.
     * @param   cycles  Latency cycles, 0–15
     * @return  The MLATS field, shifted into place
     */
    static constexpr uint8_t EncodeMLATS(uint8_t cycles) { return static_cast<uint8_t>((cycles << MLATS_SHIFT) & MLATS_MASK); }

    /*!
     * @brief   Decodes the MLATS field.
     * @param   cr2  A CR2 value
     * @return  Latency cycles, 0–15
     */
    static constexpr uint8_t DecodeMLATS(uint8_t cr2) { return static_cast<uint8_t>((cr2 & MLATS_MASK) >> MLATS_SHIFT); }
  };

  /*!
   * Device Identification Register. Same address on both parts. Read it with RDID 9Fh.
   */
  struct DIR {
    static constexpr uint32_t ADDRESS = 0x000030; //!< Register address
  };

  /*!
   * One register's expected value, for checking and applying configuration.
   */
  struct RegisterDefault {
    uint32_t address; //!< Register address, for RDAR / WRAR
    uint8_t value;    //!< Expected value
    uint8_t mask;     //!< Bits that count. Read-only and reserved bits are left out
  };

} // namespace OwlSat::MRAM
