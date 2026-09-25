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
#include <OwlSat/utility_macros.h>

namespace OwlSat {

  namespace detail {
    using SR_base_t = uint8_t;

    using CR1_base_t = uint8_t;
    using CR2_base_t = uint8_t;
    using CR3_base_t = uint8_t;
    using CR4_base_t = uint8_t;

    /*!
     * Shared plumbing for the CRn_t structs. `Derived` is the register struct itself, so the
     * operators only take and return values of that one register — a CR3_t can't be OR'd into a
     * CR1_t, and a bare integer can't be passed where a CR1_t is expected.
     *
     * `MustBeOne` holds reserved bits the datasheet says must always be written as 1.
     */
    template <typename Derived, typename Base, Base MustBeOne = 0>
    class RegisterValue {
    public:
      using base_t = Base;

      /// Wraps a byte read back from the device.
      [[nodiscard]] static constexpr Derived FromRaw(Base raw) {
        Derived reg;
        reg.val = static_cast<Base>(raw | MustBeOne);
        return reg;
      }

      [[nodiscard]] constexpr Base Raw() const { return val; }
      explicit constexpr operator Base() const { return val; }

      /// Whether every bit in `flags` is set.
      [[nodiscard]] constexpr bool Has(Derived flags) const { return (val & flags.val) == flags.val; }

      friend constexpr Derived operator|(Derived a, Derived b) { return FromRaw(static_cast<Base>(a.val | b.val)); }
      friend constexpr bool operator==(Derived a, Derived b) { return a.val == b.val; }

      constexpr Derived& operator|=(Derived other) {
        val = static_cast<Base>(val | other.val);
        return static_cast<Derived&>(*this);
      }

    protected:
      Base val = MustBeOne;
    };
  } // namespace detail

  namespace Config {
    constexpr detail::CR1_base_t CR1_DEFAULT = 0x04;
    constexpr detail::CR2_base_t CR2_DEFAULT = 0x00;
    constexpr detail::CR3_base_t CR3_DEFAULT = 0x60;
    constexpr detail::CR4_base_t CR4_DEFAULT = 0x04;



  }

  namespace MRAM {

#pragma region CR1
    /*!
     * Configuration Register 1 — protection locks. Datasheet Table 20.
     */
    struct CR1_t : detail::RegisterValue<CR1_t, detail::CR1_base_t> {
      static constexpr uint32_t ADDRESS = 0x000002; //!< For RDAR 65h / WRAR 71h

      static const CR1_t ASPLK; //!< Write protects the whole augmented storage array
      static const CR1_t MAPLK; //!< Locks TBSEL and BPSEL[2:0] in the status register
    };
    inline constexpr CR1_t CR1_t::ASPLK = FromRaw(1u << 0);
    inline constexpr CR1_t CR1_t::MAPLK = FromRaw(1u << 2);
#pragma endregion
#pragma region CR2
    /*!
     * Configuration Register 2 — interface mode and memory array read latency. Datasheet Table 21.
     */
    struct CR2_t : detail::RegisterValue<CR2_t, detail::CR2_base_t> {
      static constexpr uint32_t ADDRESS = 0x000003; //!< For RDAR 65h / WRAR 71h

      /*!
       * MLATS[3:0] — latency cycles (0–15) inserted before data on memory array reads.
       * Plain READ (03h) needs none but tops out at 50MHz. Fast reads need 8–15 cycles for 1-1-1,
       * 1-1-2, 1-2-2 and 2-2-2, and 12–15 for 1-1-4, 1-4-4 and 4-4-4. Datasheet Tables 22–24.
       */
      static constexpr CR2_t MLATS(uint8_t cycles) { return FromRaw(cycles & 0x0Fu); }

      // Read-only: the bus mode only changes through the DPIE / QPIE / SPIE instructions
      // (37h / 38h / FFh). For testing a read-back value with Has(); writing them does nothing.
      static const CR2_t DPISL; //!< Dual SPI (DPI 2-2-2) is active
      static const CR2_t QPISL; //!< Quad SPI (QPI 4-4-4) is active
    };
    inline constexpr CR2_t CR2_t::DPISL = FromRaw(1u << 4);
    inline constexpr CR2_t CR2_t::QPISL = FromRaw(1u << 6);
#pragma endregion
#pragma region CR3
    /*!
     * Configuration Register 3 — output driver strength and read wrap. Datasheet Table 26.
     */
    struct CR3_t : detail::RegisterValue<CR3_t, detail::CR3_base_t> {
      static constexpr uint32_t ADDRESS = 0x000004; //!< For RDAR 65h / WRAR 71h

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
      static constexpr CR3_t ODSEL(uint8_t code) { return FromRaw(static_cast<uint8_t>((code & 0x07u) << 5)); }

      static const CR3_t WRAPS; //!< Enables read wrap within the boundary selected by WRPLS_*

      // WRPLS[2:0] — aligned boundary that reads wrap within. Codes 101–111 are reserved.
      static const CR3_t WRPLS_16;
      static const CR3_t WRPLS_32;
      static const CR3_t WRPLS_64;
      static const CR3_t WRPLS_128;
      static const CR3_t WRPLS_256;
    };
    inline constexpr CR3_t CR3_t::WRAPS     = FromRaw(1u << 4);
    inline constexpr CR3_t CR3_t::WRPLS_16  = FromRaw(0b000);
    inline constexpr CR3_t CR3_t::WRPLS_32  = FromRaw(0b001);
    inline constexpr CR3_t CR3_t::WRPLS_64  = FromRaw(0b010);
    inline constexpr CR3_t CR3_t::WRPLS_128 = FromRaw(0b011);
    inline constexpr CR3_t CR3_t::WRPLS_256 = FromRaw(0b100);
#pragma endregion
#pragma region CR4
    /*!
     * Configuration Register 4 — whether memory array and augmented storage writes need WREN
     * first. Register writes always need WREN regardless. Datasheet Table 27.
     *
     * CR4[2] is reserved but must be written as 1, so every CR4_t has it set.
     */
    struct CR4_t : detail::RegisterValue<CR4_t, detail::CR4_base_t, (1u << 2)> {
      static constexpr uint32_t ADDRESS = 0x000005; //!< For RDAR 65h / WRAR 71h

      // WRENS[1:0]. Code 11 is illegal.
      static const CR4_t WRENS_NORMAL;       //!< WREN before every array write; cleared when CS# goes high
      static const CR4_t WRENS_SRAM;         //!< WREN not needed (ignored) for array writes. Factory default
      static const CR4_t WRENS_BACK_TO_BACK; //!< WREN before the first array write only; stays set until WRDI
    };
    inline constexpr CR4_t CR4_t::WRENS_NORMAL       = FromRaw(0b00);
    inline constexpr CR4_t CR4_t::WRENS_SRAM         = FromRaw(0b01);
    inline constexpr CR4_t CR4_t::WRENS_BACK_TO_BACK = FromRaw(0b10);
#pragma endregion


    bool Read(uintptr_t dest, uintptr_t addr);

    bool Write(uintptr_t dest, uintptr_t src, size_t bytes);



    CR1_t ReadCR1();
    CR2_t ReadCR2();
    CR3_t ReadCR3();
    CR4_t ReadCR4();



  } // namespace MRAM


} // namespace OwlSat
