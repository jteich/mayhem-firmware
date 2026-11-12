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

#include "zwave_decoder.hpp"

namespace zwave {

namespace {

constexpr float PLL_GAIN = 0.25f;

// R1: 1 byte preamble (0x55) + 1 byte SOF (0xF0) Manchester-encoded (0 -> 10, 1 -> 01)
constexpr uint32_t R1_SYNC_NORM = 0x999955AA;
constexpr uint32_t R1_SYNC_INV = 0x6666AA55;

// R2 & R3: 2 bytes preamble (0x5555) + 1 byte SOF (0xF0) NRZ
constexpr uint32_t NRZ_SYNC_NORM = 0x5555F0;
constexpr uint32_t NRZ_SYNC_INV = 0xAAAA0F;

}  // namespace

uint8_t fcs_xor8(const uint8_t* data, const size_t length) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; i++)
        crc ^= data[i];
    return crc;
}

uint16_t fcs_crc16(const uint8_t* data, const size_t length) {
    uint16_t crc = 0x1D0F;
    for (size_t i = 0; i < length; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (size_t b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

void ChannelDecoder::configure(const Rate rate, const float sampling_rate, PacketHandler handler, void* context) {
    rate_ = rate;
    handler_ = handler;
    context_ = context;

    const float symbol_rate = (rate == Rate::R1_9k6)   ? 19200.0f
                              : (rate == Rate::R2_40k) ? 40000.0f
                                                       : 100000.0f;
    symbol_step_ = symbol_rate / sampling_rate;
    dc_alpha_ = 1.0f / (4.0f * sampling_rate / symbol_rate);
    symbol_phase_ = 0.0f;
    dc_ = 0.0f;
    last_level_ = false;
    shift_ = 0;
    packets_ = 0;
    reset_state();
}

void ChannelDecoder::reset_state() {
    state_ = State::Search;
    inverted_ = false;
    chip_count_ = 0;
    bit_count_ = 0;
    byte_ = 0;
    expected_length_ = 0;
    received_ = 0;
}

void ChannelDecoder::execute(const float sample) {
    if (state_ == State::Search)
        dc_ += (sample - dc_) * dc_alpha_;
    const bool level = sample > dc_;

    if (level != last_level_) {
        symbol_phase_ -= (symbol_phase_ - 0.5f) * PLL_GAIN;
        last_level_ = level;
    }
    symbol_phase_ += symbol_step_;
    if (symbol_phase_ >= 1.0f) {
        symbol_phase_ -= 1.0f;
        symbol(level ? 1 : 0);
    }
}

void ChannelDecoder::symbol(const uint8_t s) {
    shift_ = (shift_ << 1) | s;

    if (state_ == State::Search) {
        if (rate_ == Rate::R1_9k6) {
            if (shift_ == R1_SYNC_NORM || shift_ == R1_SYNC_INV) {
                reset_state();
                state_ = State::Data;
                inverted_ = (shift_ == R1_SYNC_INV);
            }
        } else {
            const uint32_t window = shift_ & 0xFFFFFF;
            if (window == NRZ_SYNC_NORM || window == NRZ_SYNC_INV) {
                reset_state();
                state_ = State::Data;
                inverted_ = (window == NRZ_SYNC_INV);
            }
        }
        return;
    }

    if (rate_ == Rate::R1_9k6) {
        if (++chip_count_ < 2)
            return;
        chip_count_ = 0;
        const uint8_t pair = (shift_ ^ (inverted_ ? 0x03 : 0x00)) & 0x03;
        if (pair == 0b10)
            push_bit(0);
        else if (pair == 0b01)
            push_bit(1);
        else
            reset_state();  // Manchester violation or EOF
    } else {
        push_bit(s ^ (inverted_ ? 1 : 0));
    }
}

void ChannelDecoder::push_bit(const uint8_t b) {
    byte_ = (byte_ << 1) | b;
    if (++bit_count_ < 8)
        return;
    bit_count_ = 0;

    buffer_[received_++] = byte_;
    if (received_ == 8) {
        expected_length_ = byte_;
        const size_t min_len = (rate_ == Rate::R3_100k) ? MIN_MPDU_R3 : MIN_MPDU_R1_R2;
        const size_t max_len = (rate_ == Rate::R3_100k) ? MAX_MPDU : MAX_MPDU_R1_R2;
        if (expected_length_ < min_len || expected_length_ > max_len) {
            reset_state();
            return;
        }
    }

    if (received_ >= 8 && received_ == expected_length_) {
        bool valid = false;
        if (rate_ == Rate::R3_100k) {
            const uint16_t rx_crc = (static_cast<uint16_t>(buffer_[expected_length_ - 2]) << 8) |
                                    buffer_[expected_length_ - 1];
            valid = (fcs_crc16(buffer_.data(), expected_length_ - 2) == rx_crc);
        } else {
            valid = (fcs_xor8(buffer_.data(), expected_length_ - 1) == buffer_[expected_length_ - 1]);
        }

        if (valid) {
            packets_++;
            if (handler_)
                handler_(context_, rate_, buffer_.data(), expected_length_);
        }
        reset_state();
    }
}

void Decoder::configure(const float sampling_rate) {
    r1_.configure(Rate::R1_9k6, sampling_rate, handler_, context_);
    r2_.configure(Rate::R2_40k, sampling_rate, handler_, context_);
    r3_.configure(Rate::R3_100k, sampling_rate, handler_, context_);
}

void Decoder::execute(const float sample) {
    r1_.execute(sample);
    r2_.execute(sample);
    r3_.execute(sample);
}

}  // namespace zwave
