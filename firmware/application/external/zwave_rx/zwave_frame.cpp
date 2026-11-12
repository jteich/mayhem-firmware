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

#include "zwave_frame.hpp"
#include "string_format.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ui::external_app::zwave_rx {

namespace {

constexpr uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

inline uint8_t xtime(const uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

int hex_nibble(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string hex_bytes(const uint8_t* p, const size_t n) {
    std::string s{};
    for (size_t i = 0; i < n; i++)
        s += to_string_hex(p[i], 2);
    return s;
}

std::string decode_command_payload(
    const uint8_t* msdu,
    const size_t len,
    const uint8_t src_node,
    const uint8_t dst_node,
    S0Keyring* keyring,
    Frame& out,
    const int depth) {
    if (len == 0)
        return "";

    const uint8_t cc = msdu[0];
    const uint8_t cmd = (len >= 2) ? msdu[1] : 0;
    if (depth == 0) {
        out.has_cc = true;
        out.cmd_class = cc;
        out.cmd = cmd;
    } else {
        out.inner_cc = cc;
        out.inner_cmd = cmd;
    }

    switch (cc) {
        case 0x00:
            return "NOP";

        case 0x20:  // COMMAND_CLASS_BASIC
            if (cmd == 0x01 && len >= 3)
                return "Basic Set=" + to_string_dec_uint(msdu[2]);
            if (cmd == 0x02)
                return "Basic Get";
            if (cmd == 0x03 && len >= 3)
                return "Basic Rpt=" + to_string_dec_uint(msdu[2]);
            return "Basic 0x" + to_string_hex(cmd, 2);

        case 0x25:  // COMMAND_CLASS_SWITCH_BINARY
            if (cmd == 0x01 && len >= 3)
                return std::string{"SwBin Set "} + (msdu[2] ? "ON" : "OFF");
            if (cmd == 0x02)
                return "SwBin Get";
            if (cmd == 0x03 && len >= 3)
                return std::string{"SwBin Rpt "} + (msdu[2] ? "ON" : "OFF");
            return "SwBin 0x" + to_string_hex(cmd, 2);

        case 0x26:  // COMMAND_CLASS_SWITCH_MULTILEVEL
            if (cmd == 0x01 && len >= 3)
                return "SwLvl Set=" + to_string_dec_uint(msdu[2]);
            if (cmd == 0x02)
                return "SwLvl Get";
            if (cmd == 0x03 && len >= 3)
                return "SwLvl Rpt=" + to_string_dec_uint(msdu[2]);
            return "SwLvl 0x" + to_string_hex(cmd, 2);

        case 0x30:  // COMMAND_CLASS_SENSOR_BINARY
            if (cmd == 0x02)
                return "SensBin Get";
            if (cmd == 0x03 && len >= 3)
                return std::string{"SensBin Rpt="} + (msdu[2] ? "TRIG" : "IDLE");
            return "SensBin 0x" + to_string_hex(cmd, 2);

        case 0x31: {  // COMMAND_CLASS_SENSOR_MULTILEVEL
            if (cmd == 0x04)
                return "SensMulti Get";
            if (cmd == 0x05 && len >= 5) {
                const uint8_t sens_type = msdu[2];
                const uint8_t fmt = msdu[3];
                const uint8_t precision = (fmt >> 5) & 0x07;
                const uint8_t scale = (fmt >> 3) & 0x03;
                const uint8_t size = fmt & 0x07;
                const char* tname = "Sens";
                const char* unit = "";
                if (sens_type == 0x01) {
                    tname = "Temp";
                    unit = (scale == 1) ? "F" : "C";
                } else if (sens_type == 0x03) {
                    tname = "Lum";
                    unit = (scale == 1) ? "lx" : "%";
                } else if (sens_type == 0x04) {
                    tname = "Pwr";
                    unit = "W";
                } else if (sens_type == 0x05) {
                    tname = "Hum";
                    unit = "%";
                }
                if (size >= 1 && size <= 4 && 4 + size <= len) {
                    int32_t raw = 0;
                    for (uint8_t i = 0; i < size; i++)
                        raw = (raw << 8) | msdu[4 + i];
                    if (size == 1 && (raw & 0x80)) raw -= 0x100;
                    if (size == 2 && (raw & 0x8000)) raw -= 0x10000;
                    std::string val_str{};
                    if (precision == 0) {
                        val_str = to_string_dec_int(raw);
                    } else {
                        int32_t div = 1;
                        for (uint8_t p = 0; p < precision && p < 4; p++) div *= 10;
                        const int32_t whole = raw / div;
                        const int32_t frac = std::abs(raw % div);
                        val_str = to_string_dec_int(whole) + "." + to_string_dec_uint(frac, precision, '0');
                    }
                    return std::string{tname} + "=" + val_str + unit;
                }
                return std::string{tname} + " Rpt";
            }
            return "SensMulti 0x" + to_string_hex(cmd, 2);
        }

        case 0x32:  // COMMAND_CLASS_METER
            if (cmd == 0x01) return "Meter Get";
            if (cmd == 0x02) return "Meter Rpt";
            return "Meter 0x" + to_string_hex(cmd, 2);

        case 0x56:  // COMMAND_CLASS_CRC_16_ENCAP
            if (cmd == 0x01 && len >= 5 && depth < 2)
                return "CRC16[" + decode_command_payload(msdu + 2, len - 4, src_node, dst_node, keyring, out, depth + 1) + "]";
            return "CRC16Encap";

        case 0x5A:
            return "DevResetLocal";
        case 0x5E:
            return "ZWavePlusInfo";

        case 0x60:  // COMMAND_CLASS_MULTI_CHANNEL
            if (cmd == 0x0D && len >= 5 && depth < 2) {
                return "MC(" + to_string_dec_uint(msdu[2] & 0x7F) + "->" + to_string_dec_uint(msdu[3] & 0x7F) + ")[" +
                       decode_command_payload(msdu + 4, len - 4, src_node, dst_node, keyring, out, depth + 1) + "]";
            }
            return "MultiChan 0x" + to_string_hex(cmd, 2);

        case 0x62:  // COMMAND_CLASS_DOOR_LOCK
            if (cmd == 0x01 && len >= 3)
                return std::string{"DoorLock Set="} + (msdu[2] == 0xFF ? "LOCK" : "UNLOCK");
            if (cmd == 0x02)
                return "DoorLock Get";
            if (cmd == 0x03 && len >= 3)
                return std::string{"DoorLock Rpt="} + (msdu[2] == 0xFF ? "LOCKED" : "UNLOCKED");
            return "DoorLock 0x" + to_string_hex(cmd, 2);

        case 0x6C:  // COMMAND_CLASS_SUPERVISION
            if (cmd == 0x01 && len >= 5 && depth < 2) {
                const uint8_t inner_len = msdu[3];
                if (4U + inner_len <= len)
                    return "Supv[" + decode_command_payload(msdu + 4, inner_len, src_node, dst_node, keyring, out, depth + 1) + "]";
            }
            if (cmd == 0x02) return "Supv Rpt";
            return "Supv 0x" + to_string_hex(cmd, 2);

        case 0x70:
            return "Config 0x" + to_string_hex(cmd, 2);

        case 0x71:  // COMMAND_CLASS_NOTIFICATION / ALARM
            if (cmd == 0x05 && len >= 8)
                return "Notify Type=" + to_string_hex(msdu[6], 2) + " Evt=" + to_string_hex(msdu[7], 2);
            return "Notify 0x" + to_string_hex(cmd, 2);

        case 0x72:
            return "MfgSpec 0x" + to_string_hex(cmd, 2);

        case 0x80:  // COMMAND_CLASS_BATTERY
            if (cmd == 0x02) return "Batt Get";
            if (cmd == 0x03 && len >= 3) {
                if (msdu[2] == 0xFF) return "Batt=LOW!";
                return "Batt=" + to_string_dec_uint(msdu[2]) + "%";
            }
            return "Batt 0x" + to_string_hex(cmd, 2);

        case 0x84:  // COMMAND_CLASS_WAKE_UP
            if (cmd == 0x07) return "WakeUp Notify";
            if (cmd == 0x08) return "WakeUp NoMoreInfo";
            return "WakeUp 0x" + to_string_hex(cmd, 2);

        case 0x85:
            return "Assoc 0x" + to_string_hex(cmd, 2);
        case 0x86:
            return "Version 0x" + to_string_hex(cmd, 2);

        case 0x98: {  // COMMAND_CLASS_SECURITY (S0)
            if (cmd == 0x06 && len >= 18) {
                // NETWORK_KEY_SET inside a decrypted inclusion frame
                if (keyring && !keyring->has_user_key())
                    keyring->set_user_key(msdu + 2);
                return "S0 NetKeySet=" + hex_bytes(msdu + 2, 16);
            }
            if (cmd == 0x07) return "S0 NetKeyVerify";
            if (cmd == 0x08) return "S0 SchemeInherit";
            if (cmd == 0x40) return "S0 NonceGet";
            if (cmd == 0x80 && len >= 10) {
                if (keyring)
                    keyring->record_nonce(msdu + 2);
                return "S0 NonceRpt ID=0x" + to_string_hex(msdu[2], 2);
            }
            if ((cmd == 0x81 || cmd == 0xC1) && len >= 20) {
                out.s0_encrypted = true;
                const uint8_t* sender_nonce = msdu + 2;
                const size_t cipher_len = len - 19;
                const uint8_t* ciphertext = msdu + 10;
                const uint8_t rx_nonce_id = msdu[len - 9];
                const uint8_t* rx_mac = msdu + (len - 8);

                if (keyring && cipher_len <= 160 && depth < 2) {
                    std::array<uint8_t, 160> plain{};
                    size_t plain_len = 0;
                    bool used_default = false;
                    if (keyring->decrypt_encap(cmd, src_node, dst_node, sender_nonce, rx_nonce_id,
                                               ciphertext, cipher_len, rx_mac, plain.data(), plain_len, used_default)) {
                        out.s0_decrypted = true;
                        out.s0_used_default_key = used_default;
                        if (plain_len >= 2) {
                            const std::string inner = decode_command_payload(
                                plain.data() + 1, plain_len - 1, src_node, dst_node, keyring, out, depth + 1);
                            wipe_bytes(plain.data(), plain.size());
                            return std::string{used_default ? "S0(K0)[" : "S0["} + inner + "]";
                        }
                        wipe_bytes(plain.data(), plain.size());
                        return "S0[SeqFrag]";
                    }
                }
                return "S0 Encap(NID=0x" + to_string_hex(rx_nonce_id, 2) + ")";
            }
            return "S0 0x" + to_string_hex(cmd, 2);
        }

        case 0x9F:  // COMMAND_CLASS_SECURITY_2 (S2)
            if (cmd == 0x01) return "S2 NonceGet";
            if (cmd == 0x02) return "S2 NonceRpt";
            if (cmd == 0x03) return "S2 MsgEncap";
            return "S2 0x" + to_string_hex(cmd, 2);

        default:
            return "CC:0x" + to_string_hex(cc, 2) + " Cmd:0x" + to_string_hex(cmd, 2);
    }
}

}  // namespace

void wipe_bytes(void* p, const size_t length) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    for (size_t i = 0; i < length; i++)
        v[i] = 0;
}

bool parse_hex_key_128(const std::string& text, uint8_t key[16]) {
    size_t n = 0;
    uint8_t high = 0;
    for (const char c : text) {
        if (c == ' ' || c == ':' || c == '-')
            continue;
        const int v = hex_nibble(c);
        if (v < 0 || n >= 32)
            return false;
        if ((n & 1) == 0)
            high = static_cast<uint8_t>(v << 4);
        else
            key[n >> 1] = static_cast<uint8_t>(high | v);
        n++;
    }
    return n == 32;
}

void Aes128::set_key(const uint8_t key[16]) {
    uint8_t* w = round_keys_.data();
    memcpy(w, key, 16);
    uint8_t rcon = 0x01;
    for (size_t i = 16; i < round_keys_.size(); i += 4) {
        uint8_t t[4];
        memcpy(t, w + i - 4, 4);
        if (i % 16 == 0) {
            const uint8_t t0 = t[0];
            t[0] = sbox[t[1]] ^ rcon;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[t0];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++)
            w[i + j] = w[i + j - 16] ^ t[j];
    }
}

void Aes128::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ round_keys_[i];

    for (int round = 1; round <= 10; round++) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = sbox[s[((c + r) & 3) * 4 + r]];

        const uint8_t* rk = round_keys_.data() + round * 16;
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                const uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
                const uint8_t x = a0 ^ a1 ^ a2 ^ a3;
                s[c * 4 + 0] = a0 ^ x ^ xtime(a0 ^ a1) ^ rk[c * 4 + 0];
                s[c * 4 + 1] = a1 ^ x ^ xtime(a1 ^ a2) ^ rk[c * 4 + 1];
                s[c * 4 + 2] = a2 ^ x ^ xtime(a2 ^ a3) ^ rk[c * 4 + 2];
                s[c * 4 + 3] = a3 ^ x ^ xtime(a3 ^ a0) ^ rk[c * 4 + 3];
            }
        } else {
            for (int i = 0; i < 16; i++)
                s[i] = t[i] ^ rk[i];
        }
    }
    memcpy(out, s, 16);
}

void Aes128::clear() {
    wipe_bytes(round_keys_.data(), round_keys_.size());
}

S0Keyring::S0Keyring() {
    const uint8_t zero_key[KEY_BYTES]{};
    derive_keys(zero_key, default_keys_);
}

void S0Keyring::derive_keys(const uint8_t network_key[KEY_BYTES], KeyPair& out) {
    Aes128 master{};
    master.set_key(network_key);

    uint8_t pattern[16];
    uint8_t derived[16];

    memset(pattern, 0x55, sizeof(pattern));
    master.encrypt_block(pattern, derived);
    out.auth_aes.set_key(derived);

    memset(pattern, 0xAA, sizeof(pattern));
    master.encrypt_block(pattern, derived);
    out.enc_aes.set_key(derived);

    wipe_bytes(derived, sizeof(derived));
    master.clear();
}

void S0Keyring::set_user_key(const uint8_t key[KEY_BYTES]) {
    derive_keys(key, user_keys_);
    has_user_key_ = true;
}

void S0Keyring::clear_user_key() {
    user_keys_.auth_aes.clear();
    user_keys_.enc_aes.clear();
    has_user_key_ = false;
}

void S0Keyring::record_nonce(const uint8_t nonce[NONCE_BYTES]) {
    // Update existing slot with same nonce ID if present, else write to ring buffer
    for (auto& entry : nonces_) {
        if (entry.valid && entry.bytes[0] == nonce[0]) {
            memcpy(entry.bytes.data(), nonce, NONCE_BYTES);
            return;
        }
    }
    auto& slot = nonces_[nonce_write_idx_];
    slot.valid = true;
    memcpy(slot.bytes.data(), nonce, NONCE_BYTES);
    nonce_write_idx_ = (nonce_write_idx_ + 1) % MAX_NONCES;
}

bool S0Keyring::try_decrypt_with(
    const KeyPair& keys,
    const uint8_t cmd_byte,
    const uint8_t src_node,
    const uint8_t dst_node,
    const uint8_t iv[16],
    const uint8_t* ciphertext,
    const size_t cipher_len,
    const uint8_t rx_mac[8],
    uint8_t* plaintext) {
    // 1. Compute S0 AES-128 CBC-MAC over IV and [cmd, src, dst, cipher_len, ciphertext...]
    uint8_t mac[16];
    keys.auth_aes.encrypt_block(iv, mac);

    const size_t auth_len = 4 + cipher_len;
    for (size_t offset = 0; offset < auth_len; offset += 16) {
        uint8_t block[16]{};
        for (size_t i = 0; i < 16 && (offset + i) < auth_len; i++) {
            const size_t pos = offset + i;
            if (pos == 0)
                block[i] = cmd_byte;
            else if (pos == 1)
                block[i] = src_node;
            else if (pos == 2)
                block[i] = dst_node;
            else if (pos == 3)
                block[i] = static_cast<uint8_t>(cipher_len);
            else
                block[i] = ciphertext[pos - 4];
        }
        for (int i = 0; i < 16; i++)
            mac[i] ^= block[i];
        keys.auth_aes.encrypt_block(mac, mac);
    }

    uint8_t diff = 0;
    for (int i = 0; i < 8; i++)
        diff |= (mac[i] ^ rx_mac[i]);
    if (diff != 0)
        return false;

    // 2. Decrypt ciphertext with AES-128-OFB
    uint8_t ofb[16];
    memcpy(ofb, iv, 16);
    for (size_t offset = 0; offset < cipher_len; offset += 16) {
        keys.enc_aes.encrypt_block(ofb, ofb);
        const size_t chunk = std::min<size_t>(16, cipher_len - offset);
        for (size_t i = 0; i < chunk; i++)
            plaintext[offset + i] = ciphertext[offset + i] ^ ofb[i];
    }
    return true;
}

bool S0Keyring::decrypt_encap(
    const uint8_t cmd_byte,
    const uint8_t src_node,
    const uint8_t dst_node,
    const uint8_t sender_nonce[NONCE_BYTES],
    const uint8_t receiver_nonce_id,
    const uint8_t* ciphertext,
    const size_t cipher_len,
    const uint8_t rx_mac[8],
    uint8_t* plaintext,
    size_t& plain_len,
    bool& used_default_key) const {
    for (const auto& entry : nonces_) {
        if (!entry.valid || entry.bytes[0] != receiver_nonce_id)
            continue;

        uint8_t iv[16];
        memcpy(iv, sender_nonce, NONCE_BYTES);
        memcpy(iv + NONCE_BYTES, entry.bytes.data(), NONCE_BYTES);

        if (has_user_key_ &&
            try_decrypt_with(user_keys_, cmd_byte, src_node, dst_node, iv, ciphertext, cipher_len, rx_mac, plaintext)) {
            plain_len = cipher_len;
            used_default_key = false;
            return true;
        }
        if (try_decrypt_with(default_keys_, cmd_byte, src_node, dst_node, iv, ciphertext, cipher_len, rx_mac, plaintext)) {
            plain_len = cipher_len;
            used_default_key = true;
            return true;
        }
    }
    return false;
}

const char* rate_name(const uint8_t rate) {
    switch (rate) {
        case 0:
            return "R1 9.6k";
        case 1:
            return "R2 40k";
        case 2:
            return "R3 100k";
        default:
            return "R?";
    }
}

bool parse_mpdu(const uint8_t* mpdu, const size_t length, const uint8_t rate, S0Keyring* keyring, Frame& out) {
    out = Frame{};
    out.rate = rate;

    const size_t fcs_bytes = (rate == 2) ? 2 : 1;
    if (length < 9 + fcs_bytes)
        return false;

    out.home_id = (static_cast<uint32_t>(mpdu[0]) << 24) |
                  (static_cast<uint32_t>(mpdu[1]) << 16) |
                  (static_cast<uint32_t>(mpdu[2]) << 8) |
                  mpdu[3];
    out.src_node = mpdu[4];

    const uint8_t fc0 = mpdu[5];
    const uint8_t fc1 = mpdu[6];
    out.mpdu_length = mpdu[7];

    out.routed = (fc0 & 0x80) != 0 || (fc0 & 0x0F) == 0x08;
    out.ack_req = (fc0 & 0x40) != 0;
    out.low_power = (fc0 & 0x20) != 0;
    out.speed_modified = (fc0 & 0x10) != 0;
    out.seq_number = fc1 & 0x0F;

    const uint8_t htype = fc0 & 0x0F;
    if (htype == 0x01)
        out.header_type = Frame::HeaderType::Singlecast;
    else if (htype == 0x02)
        out.header_type = Frame::HeaderType::Multicast;
    else if (htype == 0x03)
        out.header_type = Frame::HeaderType::Ack;
    else if (htype == 0x08)
        out.header_type = Frame::HeaderType::Routed;
    else
        out.header_type = Frame::HeaderType::Unknown;

    const size_t msdu_end = length - fcs_bytes;
    size_t offset = 8;

    if (out.header_type == Frame::HeaderType::Multicast) {
        out.dst_node = 0xFE;  // Multicast indicator
        const uint8_t mask_bytes = mpdu[offset] & 0x1F;
        offset += 1 + mask_bytes;
    } else {
        out.dst_node = mpdu[offset++];
        if (out.routed && offset + 2 <= msdu_end) {
            const uint8_t route_props = mpdu[offset];
            if (route_props & 0x20) {
                // Explorer frame header (8 bytes)
                offset += 8;
            } else {
                const uint8_t hops_byte = mpdu[offset + 1];
                const uint8_t num_rep = (hops_byte >> 4) & 0x0F;
                out.repeater_count = std::min<uint8_t>(num_rep, 4);
                for (uint8_t i = 0; i < out.repeater_count && (offset + 2 + i) < msdu_end; i++)
                    out.repeaters[i] = mpdu[offset + 2 + i];
                offset += 2 + num_rep;
            }
        }
    }

    // Skip extended header if present (G.9959 R3 bit 7 of FC1)
    if ((fc1 & 0x80) && offset < msdu_end) {
        const uint8_t ext_len = mpdu[offset] & 0x07;
        offset += 1 + ext_len;
    }

    std::string dst_str = (out.dst_node == 0xFF)   ? "BC"
                          : (out.dst_node == 0xFE) ? "MC"
                                                   : to_string_dec_uint(out.dst_node);
    out.summary = std::string{rate_name(rate)} + " " + to_string_hex(out.home_id, 8) + " " +
                  to_string_dec_uint(out.src_node) + "->" + dst_str;

    if (out.header_type == Frame::HeaderType::Ack || offset >= msdu_end) {
        out.details = (out.header_type == Frame::HeaderType::Ack) ? "ACK" : "Empty";
    } else {
        out.details = decode_command_payload(mpdu + offset, msdu_end - offset, out.src_node, out.dst_node, keyring, out, 0);
    }

    if (out.routed && out.repeater_count > 0) {
        out.details += " Rt:";
        for (uint8_t i = 0; i < out.repeater_count; i++) {
            if (i > 0) out.details += ",";
            out.details += to_string_dec_uint(out.repeaters[i]);
        }
    }
    out.details += " #" + to_string_dec_uint(out.seq_number);
    return true;
}

}  // namespace ui::external_app::zwave_rx
