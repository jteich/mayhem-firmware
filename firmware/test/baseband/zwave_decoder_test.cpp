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
#include "doctest.h"

#include <cstring>
#include <vector>

namespace {

constexpr float kSampleRate = 400000.0f;

struct CapturedPacket {
    zwave::Rate rate{};
    std::vector<uint8_t> data{};
};

void on_packet(void* ctx, zwave::Rate rate, const uint8_t* data, size_t len) {
    auto* list = static_cast<std::vector<CapturedPacket>*>(ctx);
    list->push_back({rate, std::vector<uint8_t>(data, data + len)});
}

void feed_symbols(zwave::Decoder& dec, const std::vector<uint8_t>& chips, float symbol_rate, bool inverted) {
    const float samples_per_chip = kSampleRate / symbol_rate;
    float accum = 0.0f;
    for (uint8_t chip : chips) {
        const bool high = (chip != 0) ^ inverted;
        const float sample = high ? 1000.0f : -1000.0f;
        accum += samples_per_chip;
        while (accum >= 1.0f) {
            dec.execute(sample);
            accum -= 1.0f;
        }
    }
}

void append_manchester_byte(std::vector<uint8_t>& chips, uint8_t b) {
    for (int i = 7; i >= 0; --i) {
        if ((b >> i) & 1) {
            chips.push_back(0);
            chips.push_back(1);
        } else {
            chips.push_back(1);
            chips.push_back(0);
        }
    }
}

void append_nrz_byte(std::vector<uint8_t>& chips, uint8_t b) {
    for (int i = 7; i >= 0; --i) {
        chips.push_back((b >> i) & 1);
    }
}

}  // namespace

TEST_CASE("zwave FCS XOR8 and CRC16-CCITT") {
    const uint8_t sample[] = {0xD4, 0x98, 0x05, 0x96, 0x01, 0x41, 0x08, 0x0C, 0x02, 0x20, 0x01, 0xFF};
    const uint8_t xor8 = zwave::fcs_xor8(sample, sizeof(sample));
    std::vector<uint8_t> with_xor(sample, sample + sizeof(sample));
    with_xor.push_back(xor8);
    CHECK(zwave::fcs_xor8(with_xor.data(), with_xor.size() - 1) == with_xor.back());

    const uint16_t crc16 = zwave::fcs_crc16(sample, sizeof(sample));
    CHECK(crc16 != 0);
}

TEST_CASE("zwave R1 9.6 kbps Manchester decode normal and inverted") {
    for (bool inverted : {false, true}) {
        std::vector<CapturedPacket> captured;
        zwave::Decoder dec{&on_packet, &captured};
        dec.configure(kSampleRate);

        std::vector<uint8_t> mpdu = {
            0xD4, 0x98, 0x05, 0x96,  // Home ID
            0x01,                    // Src
            0x41, 0x03,              // Frame ctrl + seq
            0x0D,                    // Length (13 bytes total including XOR8 FCS)
            0x02,                    // Dst
            0x20, 0x01, 0xFF         // BASIC_SET(0xFF)
        };
        mpdu.push_back(zwave::fcs_xor8(mpdu.data(), mpdu.size()));

        std::vector<uint8_t> chips;
        for (int i = 0; i < 10; ++i)
            append_manchester_byte(chips, 0x55);
        append_manchester_byte(chips, 0xF0);  // SOF
        for (uint8_t b : mpdu)
            append_manchester_byte(chips, b);
        append_manchester_byte(chips, 0x00);

        feed_symbols(dec, chips, 19200.0f, inverted);

        REQUIRE(captured.size() == 1);
        CHECK(captured[0].rate == zwave::Rate::R1_9k6);
        CHECK(captured[0].data == mpdu);
    }
}

TEST_CASE("zwave R2 40 kbps NRZ decode normal and inverted") {
    for (bool inverted : {false, true}) {
        std::vector<CapturedPacket> captured;
        zwave::Decoder dec{&on_packet, &captured};
        dec.configure(kSampleRate);

        std::vector<uint8_t> mpdu = {
            0xCA, 0xFE, 0xBA, 0xBE,  // Home ID
            0x03,                    // Src
            0x41, 0x07,              // Frame ctrl + seq
            0x0D,                    // Length (13 bytes)
            0x01,                    // Dst
            0x25, 0x03, 0x00         // SWITCH_BINARY_REPORT(OFF)
        };
        mpdu.push_back(zwave::fcs_xor8(mpdu.data(), mpdu.size()));

        std::vector<uint8_t> chips;
        for (int i = 0; i < 12; ++i)
            append_nrz_byte(chips, 0x55);
        append_nrz_byte(chips, 0xF0);  // SOF
        for (uint8_t b : mpdu)
            append_nrz_byte(chips, b);
        append_nrz_byte(chips, 0x00);

        feed_symbols(dec, chips, 40000.0f, inverted);

        REQUIRE(captured.size() == 1);
        CHECK(captured[0].rate == zwave::Rate::R2_40k);
        CHECK(captured[0].data == mpdu);
    }
}

TEST_CASE("zwave R3 100 kbps NRZ CRC16 decode and bad FCS rejection") {
    std::vector<CapturedPacket> captured;
    zwave::Decoder dec{&on_packet, &captured};
    dec.configure(kSampleRate);

    std::vector<uint8_t> mpdu = {
        0xDE, 0xAD, 0xBE, 0xEF,  // Home ID
        0x05,                    // Src
        0x41, 0x0A,              // Frame ctrl + seq
        0x0E,                    // Length (14 bytes including 2-byte CRC16)
        0x01,                    // Dst
        0x26, 0x03, 0x63         // SWITCH_MULTILEVEL_REPORT(99)
    };
    const uint16_t crc = zwave::fcs_crc16(mpdu.data(), mpdu.size());
    mpdu.push_back(static_cast<uint8_t>(crc >> 8));
    mpdu.push_back(static_cast<uint8_t>(crc & 0xFF));

    std::vector<uint8_t> chips;
    for (int i = 0; i < 16; ++i)
        append_nrz_byte(chips, 0x55);
    append_nrz_byte(chips, 0xF0);
    for (uint8_t b : mpdu)
        append_nrz_byte(chips, b);
    append_nrz_byte(chips, 0x00);

    // Also append a corrupted frame and verify it is rejected
    std::vector<uint8_t> bad_mpdu = mpdu;
    bad_mpdu.back() ^= 0xFF;
    for (int i = 0; i < 16; ++i)
        append_nrz_byte(chips, 0x55);
    append_nrz_byte(chips, 0xF0);
    for (uint8_t b : bad_mpdu)
        append_nrz_byte(chips, b);
    append_nrz_byte(chips, 0x00);

    feed_symbols(dec, chips, 100000.0f, false);

    REQUIRE(captured.size() == 1);
    CHECK(captured[0].rate == zwave::Rate::R3_100k);
    CHECK(captured[0].data == mpdu);
}
