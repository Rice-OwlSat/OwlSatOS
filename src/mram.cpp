/**

    @file      mram.cpp
    @brief     MRAM driver: init, configuration, register and memory array access.
    @details   Built on Execute() (mram_transactions.cpp). Works with whichever part
               Config::MRAMPart selects. Part-specific steps are function templates with
               `if constexpr`, so a branch that names one part's registers is only compiled for that
               part.
    @author    Viola Case
    @date      15.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <algorithm>
#include <array>

#include <hardware/timer.h>

#include <OwlSat/mram.h>


namespace OwlSat::MRAM {

  namespace {

    using Part = Config::MRAMPart;
    using Settings = Config::MRAMActiveSettings;

    /*!
     * @brief   Runs a no-address, single-data-byte read instruction (RDSR, RDFSR, …).
     * @param   instr  The instruction
     * @return  The byte read, or nothing if Execute() refused
     */
    std::optional<uint8_t> ReadByte(const Instruction &instr) {
      std::array<uint8_t, 1> rx {};
      if (!Execute(instr, 0, {}, rx)) return std::nullopt;
      return rx[0];
    }

    /*!
     * @brief   Whether RDAR reads agree with the no-latency reads of the same registers. Only
     *          meaningful on parts where RDAR's latency is CR2.MLATS.
     * @details SR is compared with RDSR, and FSR with RDFSR. FSR reads 80h when the chip is ready,
     *          which a misaligned read is unlikely to reproduce even when SR happens to be 00h.
     * @tparam  P  The part. Must have a flag status register
     * @return  True if both pairs match
     */
    template <typename P>
    bool RegisterReadsAgree() {
      std::optional<uint8_t> sr_direct = ReadByte(Instruction::ReadStatusRegister);
      std::optional<uint8_t> sr_rdar = ReadRegister(P::SR::ADDRESS);
      std::optional<uint8_t> fsr_direct = ReadByte(P::ReadFlagStatusRegister);
      std::optional<uint8_t> fsr_rdar = ReadRegister(P::FSR::ADDRESS);
      if (!sr_direct || !sr_rdar || !fsr_direct || !fsr_rdar) return false;
      // SR.WREN can change between the two reads only if something else is talking to the chip
      return *sr_direct == *sr_rdar && *fsr_direct == *fsr_rdar;
    }

    /*!
     * @brief   Makes the driver's copy of CR2.MLATS match the chip (Init() step 3).
     * @tparam  P  The part
     * @return  Result::Ok, Result::LatencyUnknown or Result::NotSPI
     */
    template <typename P>
    Result EstablishLatency() {
      if constexpr (!P::RDAR_USES_MLATS) {
        // RDAR's latency is fixed, so CR2 can simply be read
        std::optional<uint8_t> cr2 = ReadRegister(P::CR2::ADDRESS);
        if (!cr2) return Result::NotSPI;
        detail::SetMemoryLatency(P::CR2::DecodeMLATS(*cr2));
        return Result::Ok;
      } else {
        if (RegisterReadsAgree<P>()) return Result::Ok;

        // The chip's MLATS isn't what the driver assumed. Writes have no latency, so set it.
        std::array<uint8_t, 1> cr2 { Config::MRAMSettings<P>::FLIGHT_CR2 };
        if (!Execute(Instruction::WriteAnyRegister, P::CR2::ADDRESS, cr2, {})) return Result::NotSPI;
        busy_wait_us_32(Config::MRAMSettings<P>::REGISTER_WRITE_DESELECT_US);
        detail::SetMemoryLatency(P::CR2::DecodeMLATS(cr2[0]));

        return RegisterReadsAgree<P>() ? Result::Ok : Result::LatencyUnknown;
      }
    }

    /// Whether [address, address + length) lies inside the memory array
    bool InRange(uint32_t address, size_t length) { return address <= Part::CAPACITY && length <= Part::CAPACITY - address; }

  } // namespace


  const char *ResultName(Result result) {
    switch (result) {
    case Result::Ok: return "Ok";
    case Result::NotSPI: return "NotSPI";
    case Result::WrongDevice: return "WrongDevice";
    case Result::LatencyUnknown: return "LatencyUnknown";
    case Result::ConfigMismatch: return "ConfigMismatch";
    case Result::WriteNotVerified: return "WriteNotVerified";
    case Result::OutOfRange: return "OutOfRange";
    }
    return "?";
  }

  Result Init() {
    while (time_us_64() < Settings::POWER_UP_US) {}

    std::array<uint8_t, 4> id {};
    if (!ReadID(id)) return Result::NotSPI;
    if (!IsExpectedID(id)) return Result::WrongDevice;

    if (Result latency = EstablishLatency<Part>(); latency != Result::Ok) return latency;

    return VerifyConfig() ? Result::Ok : Result::ConfigMismatch;
  }

  bool VerifyConfig() {
    for (const RegisterDefault &reg : Settings::FLIGHT_DEFAULTS) {
      std::optional<uint8_t> value = ReadRegister(reg.address);
      if (!value || ((*value ^ reg.value) & reg.mask) != 0) return false;
    }
    return true;
  }

  bool ApplyConfig() {
    bool ok = true;
    for (const RegisterDefault &reg : Settings::FLIGHT_DEFAULTS) ok &= WriteRegister(reg.address, reg.value, reg.mask) == Result::Ok;
    return ok;
  }

  bool ReadID(std::span<uint8_t, 4> id) { return Execute(Instruction::ReadDeviceID, 0, {}, id); }

  bool IsExpectedID(std::span<const uint8_t, 4> id) {
    for (size_t i = 0; i < id.size(); ++i)
      if ((id[i] & Part::ID_MASK[i]) != (Part::EXPECTED_ID[i] & Part::ID_MASK[i])) return false;
    return true;
  }

  std::optional<uint8_t> ReadRegister(uint32_t address) {
    std::array<uint8_t, 1> rx {};
    if (!Execute(Part::ReadAnyRegister, address, {}, rx)) return std::nullopt;
    return rx[0];
  }

  Result WriteRegister(uint32_t address, uint8_t value, uint8_t verify_mask) {
    std::array<uint8_t, 1> tx { static_cast<uint8_t>(value | Part::ForceOneBits(address)) };
    if (!Execute(Instruction::WriteAnyRegister, address, tx, {})) return Result::NotSPI;
    busy_wait_us_32(Settings::REGISTER_WRITE_DESELECT_US);

    // Where RDAR's latency is MLATS, the read-back itself already runs at the new latency
    const bool writing_cr2 = address == Part::CR2::ADDRESS;
    if (writing_cr2 && Part::RDAR_USES_MLATS) detail::SetMemoryLatency(Part::CR2::DecodeMLATS(tx[0]));

    std::optional<uint8_t> readback = ReadRegister(address);
    if (!readback) return Result::NotSPI;
    if (((*readback ^ tx[0]) & verify_mask) != 0) return Result::WriteNotVerified;

    if (writing_cr2) detail::SetMemoryLatency(Part::CR2::DecodeMLATS(*readback));
    return Result::Ok;
  }

  Result Read(uint32_t address, std::span<uint8_t> dest) {
    if (!InRange(address, dest.size())) return Result::OutOfRange;
    const size_t chunk = Instruction::ReadMemoryArray.MaxDataBytes();
    while (!dest.empty()) {
      size_t n = std::min(chunk, dest.size());
      if (!Execute(Instruction::ReadMemoryArray, address, {}, dest.first(n))) return Result::NotSPI;
      address += n;
      dest = dest.subspan(n);
    }
    return Result::Ok;
  }

  Result Write(uint32_t address, std::span<const uint8_t> src) {
    if (!InRange(address, src.size())) return Result::OutOfRange;
    const size_t chunk = Instruction::WriteMemoryArray.MaxDataBytes();
    while (!src.empty()) {
      size_t n = std::min(chunk, src.size());
      if (!Execute(Instruction::WriteMemoryArray, address, src.first(n), {})) return Result::NotSPI;
      busy_wait_us_32(Settings::ARRAY_WRITE_DESELECT_US);
      address += n;
      src = src.subspan(n);
    }
    return Result::Ok;
  }

} // namespace OwlSat::MRAM
