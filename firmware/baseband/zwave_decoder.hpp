/*
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __ZWAVE_DECODER_H__
#define __ZWAVE_DECODER_H__

// ITU-T G.9959 (Z-Wave) packet decoder from FM-demodulated samples:
// - R1 (9.6 kbps): Manchester coded (19.2 kchips/s), preamble 0x55, SOF 0xF0, 8-bit XOR FCS
// - R2 (40 kbps):  NRZ (40 kbaud), preamble 0x55, SOF 0xF0, 8-bit XOR FCS
// - R3 (100 kbps): NRZ (100 kbaud), preamble 0x55, SOF 0xF0, 16-bit CRC-CCITT (0x1D0F) FCS

#include <array>
#include <cstddef>
#include <cstdint>

namespace zwave {

enum class Rate : uint8_t {
    R1_9k6 = 0,
    R2_40k = 1,
    R3_100k = 2,
};

constexpr size_t MIN_MPDU_R1_R2 = 10;
constexpr size_t MIN_MPDU_R3 = 11;
constexpr size_t MAX_MPDU_R1_R2 = 64;  // G.9959 limit for R1/R2, tighter limit = fewer noise false-locks
constexpr size_t MAX_MPDU = 170;       // R3

uint8_t fcs_xor8(const uint8_t* data, size_t length);
uint16_t fcs_crc16(const uint8_t* data, size_t length);

class ChannelDecoder {
   public:
    using PacketHandler = void (*)(void* context, Rate rate, const uint8_t* mpdu, size_t length);

    void configure(Rate rate, float sampling_rate, PacketHandler handler, void* context);
    void execute(float sample);

    uint32_t packets() const { return packets_; }

   private:
    enum class State : uint8_t {
        Search,
        Data,
    };

    Rate rate_{Rate::R2_40k};
    PacketHandler handler_{nullptr};
    void* context_{nullptr};

    float dc_{0.0f};
    float dc_alpha_{0.01f};
    float symbol_phase_{0.0f};
    float symbol_step_{0.1f};
    bool last_level_{false};

    State state_{State::Search};
    bool inverted_{false};
    uint32_t shift_{0};
    uint8_t chip_count_{0};
    uint8_t bit_count_{0};
    uint8_t byte_{0};
    size_t expected_length_{0};
    size_t received_{0};
    std::array<uint8_t, MAX_MPDU> buffer_{};
    uint32_t packets_{0};

    void reset_state();
    void symbol(uint8_t s);
    void push_bit(uint8_t b);
};

class Decoder {
   public:
    using PacketHandler = ChannelDecoder::PacketHandler;

    Decoder(PacketHandler handler, void* context)
        : handler_{handler}, context_{context} {}

    void configure(float sampling_rate);
    void execute(float sample);

   private:
    PacketHandler handler_;
    void* context_;
    ChannelDecoder r1_{};
    ChannelDecoder r2_{};
    ChannelDecoder r3_{};
};

}  // namespace zwave

#endif /*__ZWAVE_DECODER_H__*/
