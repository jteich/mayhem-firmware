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

#include "external/zwave_rx/zwave_frame.hpp"
#include "doctest.h"

#include <cstring>
#include <vector>

using namespace ui::external_app::zwave_rx;

TEST_CASE("zwave AES-128 FIPS-197 test vector") {
    const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    const uint8_t input[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t expected[16] = {
        0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
        0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};

    uint8_t output[16]{};
    Aes128 aes;
    aes.set_key(key);
    aes.encrypt_block(input, output);
    CHECK(std::memcmp(output, expected, 16) == 0);
}

TEST_CASE("zwave parse_mpdu singlecast and routed frames") {
    // 1. Direct Singlecast BASIC_SET(0xFF) at R2 (40k)
    const uint8_t singlecast[] = {
        0xD4, 0x98, 0x05, 0x96,  // Home ID
        0x01,                    // Src
        0x41, 0x08,              // Frame ctrl (Singlecast, ACK req) + Seq 8
        0x0D,                    // Length 13
        0x02,                    // Dst
        0x20, 0x01, 0xFF,        // BASIC_SET(0xFF)
        0xAA                     // FCS
    };
    Frame df{};
    REQUIRE(parse_mpdu(singlecast, sizeof(singlecast), 1, nullptr, df));
    CHECK(df.home_id == 0xD4980596);
    CHECK(df.src_node == 1);
    CHECK(df.dst_node == 2);
    CHECK(df.ack_req == true);
    CHECK(df.routed == false);
    CHECK(df.seq_number == 8);
    CHECK(df.details.find("Basic Set=255") != std::string::npos);

    // 2. Routed Singlecast SWITCH_BINARY_REPORT(OFF) at R3 (100k, 2-byte FCS)
    const uint8_t routed[] = {
        0xCA, 0xFE, 0xBA, 0xBE,  // Home ID
        0x05,                    // Src
        0x81, 0x03,              // Routed Singlecast + Seq 3
        0x11,                    // Length 17
        0x01,                    // Dst
        0x00,                    // Routed header: dir=0, Err=0
        0x10,                    // 1 repeater, hop 0
        0x03,                    // Repeater node 3
        0x25, 0x03, 0x00,        // SWITCH_BINARY_REPORT(OFF)
        0x12, 0x34               // CRC16
    };
    Frame rf{};
    REQUIRE(parse_mpdu(routed, sizeof(routed), 2, nullptr, rf));
    CHECK(rf.routed == true);
    CHECK(rf.repeater_count == 1);
    CHECK(rf.repeaters[0] == 3);
    CHECK(rf.details.find("SwBin Rpt OFF") != std::string::npos);
}

TEST_CASE("zwave S0 Security Nonce Report and Message Encapsulation decryption") {
    S0Keyring keyring;
    uint8_t net_key[16]{};
    REQUIRE(parse_hex_key_128("0102030405060708090A0B0C0D0E0F10", net_key));
    keyring.set_user_key(net_key);

    // Step 1: Receiver (node 2) sends SECURITY_NONCE_REPORT (0x98 0x80) to Sender (node 1)
    const uint8_t rx_nonce[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
    std::vector<uint8_t> nonce_mpdu = {
        0xD4, 0x98, 0x05, 0x96,  // Home ID
        0x02,                    // Src = 2
        0x41, 0x01,              // Singlecast
        0x14,                    // Length = 20
        0x01,                    // Dst = 1
        0x98, 0x80               // SECURITY_NONCE_REPORT
    };
    nonce_mpdu.insert(nonce_mpdu.end(), rx_nonce, rx_nonce + 8);
    nonce_mpdu.push_back(0x00);  // FCS placeholder

    Frame nonce_df{};
    REQUIRE(parse_mpdu(nonce_mpdu.data(), nonce_mpdu.size(), 1, &keyring, nonce_df));
    CHECK(nonce_df.details.find("S0 NonceRpt ID=0xA1") != std::string::npos);

    // Step 2: Construct an S0 SECURITY_MESSAGE_ENCAPSULATION (0x98 0x81) from node 1 to node 2
    // Plaintext inside S0: [seq=0x00, CC=0x62 (DOOR_LOCK), cmd=0x01 (OPERATION_SET), mode=0xFF (LOCK)]
    const uint8_t tx_nonce[8] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};
    const uint8_t plaintext[4] = {0x00, 0x62, 0x01, 0xFF};

    // Derive K_A and K_E for key 0102030405060708090A0B0C0D0E0F10
    Aes128 aes_root, aes_ka, aes_ke;
    aes_root.set_key(net_key);
    uint8_t block_55[16], block_aa[16], derived_ka[16], derived_ke[16];
    std::memset(block_55, 0x55, 16);
    std::memset(block_aa, 0xAA, 16);
    aes_root.encrypt_block(block_55, derived_ka);
    aes_root.encrypt_block(block_aa, derived_ke);
    aes_ka.set_key(derived_ka);
    aes_ke.set_key(derived_ke);

    // IV = tx_nonce || rx_nonce
    uint8_t iv[16];
    std::memcpy(iv, tx_nonce, 8);
    std::memcpy(iv + 8, rx_nonce, 8);

    // OFB encrypt plaintext
    uint8_t ks[16];
    aes_ke.encrypt_block(iv, ks);
    uint8_t ciphertext[4];
    for (size_t i = 0; i < 4; ++i)
        ciphertext[i] = plaintext[i] ^ ks[i];

    // Compute CBC-MAC over IV, then [cmd=0x81, src=1, dst=2, len=4, ciphertext...]
    uint8_t mac_acc[16];
    aes_ka.encrypt_block(iv, mac_acc);

    const uint8_t auth_data[8] = {
        0x81, 0x01, 0x02, 0x04,
        ciphertext[0], ciphertext[1], ciphertext[2], ciphertext[3]};
    for (size_t i = 0; i < sizeof(auth_data); ++i)
        mac_acc[i] ^= auth_data[i];
    aes_ka.encrypt_block(mac_acc, mac_acc);

    std::vector<uint8_t> enc_mpdu = {
        0xD4, 0x98, 0x05, 0x96,  // Home ID
        0x01,                    // Src = 1
        0x41, 0x02,              // Singlecast
        0x21,                    // Length = 33
        0x02,                    // Dst = 2
        0x98, 0x81               // SECURITY_MESSAGE_ENCAPSULATION
    };
    enc_mpdu.insert(enc_mpdu.end(), tx_nonce, tx_nonce + 8);
    enc_mpdu.insert(enc_mpdu.end(), ciphertext, ciphertext + 4);
    enc_mpdu.push_back(rx_nonce[0]);  // Receiver nonce ID
    enc_mpdu.insert(enc_mpdu.end(), mac_acc, mac_acc + 8);
    enc_mpdu.push_back(0x00);  // FCS placeholder

    Frame dec_df{};
    REQUIRE(parse_mpdu(enc_mpdu.data(), enc_mpdu.size(), 1, &keyring, dec_df));
    CHECK(dec_df.s0_encrypted == true);
    CHECK(dec_df.s0_decrypted == true);
    CHECK(dec_df.s0_used_default_key == false);
    CHECK(dec_df.details.find("S0[DoorLock Set=LOCK]") != std::string::npos);

    // Tamper with MAC and verify decryption is rejected
    enc_mpdu[enc_mpdu.size() - 2] ^= 0xFF;
    Frame bad_df{};
    REQUIRE(parse_mpdu(enc_mpdu.data(), enc_mpdu.size(), 1, &keyring, bad_df));
    CHECK(bad_df.s0_encrypted == true);
    CHECK(bad_df.s0_decrypted == false);
}
