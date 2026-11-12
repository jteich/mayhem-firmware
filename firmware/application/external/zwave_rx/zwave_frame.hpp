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

#ifndef __ZWAVE_FRAME_H__
#define __ZWAVE_FRAME_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ui::external_app::zwave_rx {

class Aes128 {
   public:
    void set_key(const uint8_t key[16]);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    void clear();

   private:
    std::array<uint8_t, 176> round_keys_{};
};

// Z-Wave Security S0 (COMMAND_CLASS_SECURITY 0x98) keyring and nonce cache.
// Tracks 8-byte nonces from SECURITY_NONCE_REPORT (0x98 0x80) and decrypts
// SECURITY_MESSAGE_ENCAPSULATION (0x98 0x81 / 0xC1) using AES-128-OFB + CBC-MAC.
class S0Keyring {
   public:
    static constexpr size_t KEY_BYTES = 16;
    static constexpr size_t NONCE_BYTES = 8;

    S0Keyring();

    bool has_user_key() const { return has_user_key_; }
    void set_user_key(const uint8_t key[KEY_BYTES]);
    void clear_user_key();

    void record_nonce(const uint8_t nonce[NONCE_BYTES]);

    // Attempts to verify MAC and decrypt an S0 encapsulated payload.
    // Returns true if authenticated and decrypted into plaintext[0..plain_len-1].
    bool decrypt_encap(
        uint8_t cmd_byte,
        uint8_t src_node,
        uint8_t dst_node,
        const uint8_t sender_nonce[NONCE_BYTES],
        uint8_t receiver_nonce_id,
        const uint8_t* ciphertext,
        size_t cipher_len,
        const uint8_t rx_mac[8],
        uint8_t* plaintext,
        size_t& plain_len,
        bool& used_default_key) const;

   private:
    struct KeyPair {
        Aes128 auth_aes{};
        Aes128 enc_aes{};
    };

    struct NonceEntry {
        bool valid{false};
        std::array<uint8_t, NONCE_BYTES> bytes{};
    };

    static constexpr size_t MAX_NONCES = 16;

    bool has_user_key_{false};
    KeyPair user_keys_{};
    KeyPair default_keys_{};  // Z-Wave S0 inclusion key (all zeros)
    std::array<NonceEntry, MAX_NONCES> nonces_{};
    size_t nonce_write_idx_{0};

    static void derive_keys(const uint8_t network_key[KEY_BYTES], KeyPair& out);
    static bool try_decrypt_with(
        const KeyPair& keys,
        uint8_t cmd_byte,
        uint8_t src_node,
        uint8_t dst_node,
        const uint8_t iv[16],
        const uint8_t* ciphertext,
        size_t cipher_len,
        const uint8_t rx_mac[8],
        uint8_t* plaintext);
};

bool parse_hex_key_128(const std::string& text, uint8_t key[16]);
void wipe_bytes(void* p, size_t length);

struct Frame {
    enum class HeaderType : uint8_t {
        Singlecast = 0x01,
        Multicast = 0x02,
        Ack = 0x03,
        Routed = 0x08,
        Unknown = 0xFF,
    };

    uint8_t rate{0};  // 0 = R1 (9.6k), 1 = R2 (40k), 2 = R3 (100k)
    uint32_t home_id{0};
    uint8_t src_node{0};
    uint8_t dst_node{0};
    HeaderType header_type{HeaderType::Unknown};
    bool routed{false};
    bool ack_req{false};
    bool low_power{false};
    bool speed_modified{false};
    uint8_t seq_number{0};
    uint8_t mpdu_length{0};

    uint8_t repeater_count{0};
    std::array<uint8_t, 4> repeaters{};

    bool has_cc{false};
    uint8_t cmd_class{0};
    uint8_t cmd{0};
    bool s0_encrypted{false};
    bool s0_decrypted{false};
    bool s0_used_default_key{false};
    uint8_t inner_cc{0};
    uint8_t inner_cmd{0};

    std::string summary{};
    std::string details{};
};

bool parse_mpdu(const uint8_t* mpdu, size_t length, uint8_t rate, S0Keyring* keyring, Frame& out);
const char* rate_name(uint8_t rate);

}  // namespace ui::external_app::zwave_rx

#endif /*__ZWAVE_FRAME_H__*/
