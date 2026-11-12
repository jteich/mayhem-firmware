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

#include "ui_zwave_rx.hpp"

#include "baseband_api.hpp"
#include "portapack.hpp"
#include "string_format.hpp"
#include "file_path.hpp"
#include "ui_textentry.hpp"

#include <algorithm>

using namespace portapack;

namespace ui::external_app::zwave_rx {

namespace {

constexpr rf::Frequency preset_frequencies[12] = {
    868420000,  // EU R1/R2
    869850000,  // EU R3
    908420000,  // US R1/R2
    916000000,  // US R3 / IL
    921420000,  // AU/NZ R1/R2
    919800000,  // AU/NZ R3 / HK
    865200000,  // IN
    869000000,  // RU
    868400000,  // CN
    922500000,  // JP Ch1
    923900000,  // JP Ch2
    920900000,  // KR Ch1
};

}  // namespace

ZWaveLog::ZWaveLog(Rect parent_rect)
    : Widget{parent_rect} {
    set_focusable(true);
}

void ZWaveLog::add(const Kind kind, const std::string& text) {
    entries_.push_back({kind, text});
    bytes_ += text.size();
    while (bytes_ > HISTORY_BYTES && entries_.size() > 1) {
        bytes_ -= entries_.front().text.size();
        entries_.pop_front();
    }
    if (scroll_)
        scroll_ = std::min(scroll_ + rows_of(entries_.back()), max_scroll());
    set_dirty();
}

void ZWaveLog::clear() {
    entries_.clear();
    bytes_ = 0;
    scroll_ = 0;
    set_dirty();
}

size_t ZWaveLog::columns() const {
    return std::max<size_t>(1, screen_rect().width() / style().font.char_width());
}

size_t ZWaveLog::rows_of(const Entry& e) const {
    return std::max<size_t>(1, (e.text.size() + columns() - 1) / columns());
}

size_t ZWaveLog::visible_rows() const {
    return screen_rect().height() / style().font.line_height();
}

size_t ZWaveLog::max_scroll() const {
    size_t rows = 0;
    for (const auto& e : entries_)
        rows += rows_of(e);
    return rows > visible_rows() ? rows - visible_rows() : 0;
}

bool ZWaveLog::scroll_by(const int32_t rows) {
    const int32_t target = std::clamp<int32_t>(static_cast<int32_t>(scroll_) + rows, 0, max_scroll());
    if (static_cast<size_t>(target) == scroll_)
        return false;
    scroll_ = target;
    set_dirty();
    return true;
}

bool ZWaveLog::on_key(const KeyEvent key) {
    if (key == KeyEvent::Up)
        return scroll_by(1);
    if (key == KeyEvent::Down) {
        scroll_by(-1);
        return true;
    }
    return false;
}

bool ZWaveLog::on_encoder(const EncoderEvent delta) {
    scroll_by(-delta);
    return true;
}

void ZWaveLog::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto& s = style();
    const auto line_height = s.font.line_height();
    const size_t cols = columns();
    painter.fill_rectangle(r, s.background);

    Coord y = r.top() + (visible_rows() - 1) * line_height;
    size_t skip = scroll_;
    for (auto e = entries_.rbegin(); e != entries_.rend() && y >= r.top(); ++e) {
        const Color color = (e->kind == Kind::Decrypted)   ? Color::green()
                            : (e->kind == Kind::Encrypted) ? Color::yellow()
                            : (e->kind == Kind::AckOrInfo) ? Color::grey()
                            : (e->kind == Kind::BadFcs)    ? Color::red()
                                                           : s.foreground;
        for (size_t row = rows_of(*e); row-- > 0 && y >= r.top();) {
            if (skip) {
                skip--;
                continue;
            }
            painter.draw_string({r.left(), y}, s.font, color, s.background,
                                std::string_view{e->text}.substr(row * cols, cols));
            y -= line_height;
        }
    }

    if (scroll_) {
        const std::string more = " more below ";
        const Coord w = more.size() * s.font.char_width();
        painter.draw_string({r.right() - w, r.top() + static_cast<Coord>(visible_rows() - 1) * line_height},
                            s.font, s.background, s.foreground, more);
    }
    if (has_focus())
        painter.draw_rectangle(r, Theme::getInstance()->fg_light->foreground);
}

ZWaveSettingsView::ZWaveSettingsView(NavigationView& nav, ZWaveSettings& settings)
    : settings_{settings}, key_hex_{settings.s0_key_hex} {
    add_children({&check_log, &check_small_font, &check_show_hex, &check_s0_key, &button_key, &button_save});

    check_log.set_value(settings_.enable_log);
    check_small_font.set_value(settings_.small_font);
    check_show_hex.set_value(settings_.show_hex);
    check_s0_key.set_value(settings_.use_s0_key);
    check_s0_key.on_select = [this](Checkbox&, bool) {
        update_key_button();
    };

    button_key.on_select = [this, &nav](Button&) {
        text_prompt(nav, key_hex_, 32, ENTER_KEYBOARD_MODE_HEX, [this](std::string&) {
            update_key_button();
        });
    };
    update_key_button();

    button_save.on_select = [this, &nav](Button&) {
        const bool use_key = check_s0_key.value();
        uint8_t parsed[16];
        if (use_key && !parse_hex_key_128(key_hex_, parsed)) {
            button_key.set_text("Need 32 hex digits");
            return;
        }
        wipe_bytes(parsed, sizeof(parsed));

        settings_.key_changed = (use_key != settings_.use_s0_key) || (key_hex_ != settings_.s0_key_hex);
        settings_.use_s0_key = use_key;
        wipe_bytes(&settings_.s0_key_hex[0], settings_.s0_key_hex.size());
        settings_.s0_key_hex = use_key ? key_hex_ : "";

        settings_.enable_log = check_log.value();
        settings_.small_font = check_small_font.value();
        settings_.show_hex = check_show_hex.value();
        nav.pop();
    };
}

ZWaveSettingsView::~ZWaveSettingsView() {
    wipe_bytes(&key_hex_[0], key_hex_.size());
}

void ZWaveSettingsView::update_key_button() {
    const bool use_key = check_s0_key.value();
    button_key.set_text(!use_key          ? "S0 inclusion key (00..00)"
                        : key_hex_.empty() ? "Enter 32-hex S0 Key"
                                           : "S0 Key set (change)");
    button_key.set_focusable(use_key);
}

ZWaveRxView::ZWaveRxView(NavigationView& nav)
    : nav_{nav} {
    baseband::run_image(portapack::spi_flash::image_tag_zwave_rx);

    add_children({&options_channel, &field_rf_amp, &field_lna, &field_vga, &rssi, &channel,
                  &field_frequency, &text_pkt_count, &button_clear, &button_config, &text_stats, &packet_log});

    field_frequency.set_step(10000);
    log_file_.append(logs_dir / u"ZWAVE.TXT");

    options_channel.on_change = [this](size_t, int32_t v) {
        on_channel_changed(v);
    };
    if (zwave_settings_.channel_preset >= 12)
        zwave_settings_.channel_preset = 0;
    options_channel.set_selected_index(zwave_settings_.channel_preset);

    button_clear.on_select = [this](Button&) {
        packet_log.clear();
        packet_count_ = 0;
        text_pkt_count.set("Pkts:0");
    };

    button_config.on_select = [this](Button&) {
        nav_.push<ZWaveSettingsView>(zwave_settings_);
        nav_.set_on_pop([this]() {
            apply_key();
            refresh_ui();
        });
    };

    packet_log.add(ZWaveLog::Kind::AckOrInfo, "Z-Wave R1(9.6k)/R2(40k)/R3(100k) ready");
    refresh_ui();
    receiver_model.enable();
}

ZWaveRxView::~ZWaveRxView() {
    receiver_model.disable();
    baseband::shutdown();
    keyring_.clear_user_key();
    wipe_bytes(&zwave_settings_.s0_key_hex[0], zwave_settings_.s0_key_hex.size());
}

void ZWaveRxView::focus() {
    options_channel.focus();
}

void ZWaveRxView::on_channel_changed(const int32_t index) {
    if (index < 0 || index >= 12)
        return;
    zwave_settings_.channel_preset = static_cast<uint8_t>(index);
    const auto freq = preset_frequencies[index];
    receiver_model.set_target_frequency(freq);
    field_frequency.set_value(freq);
}

void ZWaveRxView::refresh_ui() {
    packet_log.set_style(zwave_settings_.small_font ? Theme::getInstance()->bg_darkest_small
                                                    : Theme::getInstance()->bg_darkest);
}

void ZWaveRxView::apply_key() {
    if (!zwave_settings_.key_changed)
        return;
    zwave_settings_.key_changed = false;
    if (!zwave_settings_.use_s0_key) {
        keyring_.clear_user_key();
        packet_log.add(ZWaveLog::Kind::AckOrInfo, "S0 custom key cleared");
        return;
    }
    uint8_t key[16];
    if (parse_hex_key_128(zwave_settings_.s0_key_hex, key)) {
        keyring_.set_user_key(key);
        wipe_bytes(key, sizeof(key));
        packet_log.add(ZWaveLog::Kind::AckOrInfo, "S0 custom network key active");
    }
}

void ZWaveRxView::on_stats(const ZWavePacketMessage& message) {
    std::string s{"Lk/Bad "};
    for (size_t r = 0; r < 3; r++) {
        s += "R" + to_string_dec_uint(r + 1) + ":" + to_string_dec_uint(message.syncs[r]) + "/" +
             to_string_dec_uint(message.bad_fcs[r]) + (r < 2 ? " " : "");
    }
    text_stats.set(s);
}

void ZWaveRxView::on_packet(const ZWavePacketMessage& message) {
    if (message.type == ZWavePacketMessage::Type::Stats) {
        on_stats(message);
        return;
    }

    if (!message.fcs_ok) {
        // Frame was on the air but is corrupted: still show it so a weak link is visible.
        if (!zwave_settings_.show_hex && !zwave_settings_.enable_log)
            return;
        std::string bad_hex{};
        for (size_t i = 0; i < message.length; i++)
            bad_hex += to_string_hex(message.data[i], 2);
        const std::string line = std::string{"BAD FCS "} + rate_name(message.rate) + " " + bad_hex;
        if (zwave_settings_.show_hex)
            packet_log.add(ZWaveLog::Kind::BadFcs, line);
        if (zwave_settings_.enable_log)
            log_file_.write_entry(line);
        return;
    }

    Frame frame{};
    if (!parse_mpdu(message.data, message.length, message.rate, &keyring_, frame))
        return;

    packet_count_++;
    text_pkt_count.set("Pkts:" + to_string_dec_uint(packet_count_));

    ZWaveLog::Kind kind = ZWaveLog::Kind::Command;
    if (frame.s0_decrypted)
        kind = ZWaveLog::Kind::Decrypted;
    else if (frame.s0_encrypted)
        kind = ZWaveLog::Kind::Encrypted;
    else if (frame.header_type == Frame::HeaderType::Ack)
        kind = ZWaveLog::Kind::AckOrInfo;

    const std::string line = frame.summary + " " + frame.details;
    packet_log.add(kind, line);

    std::string hex_str{};
    if (zwave_settings_.show_hex || zwave_settings_.enable_log) {
        for (size_t i = 0; i < message.length; i++)
            hex_str += to_string_hex(message.data[i], 2);
    }
    if (zwave_settings_.show_hex)
        packet_log.add(ZWaveLog::Kind::AckOrInfo, "Hex:" + hex_str);

    if (zwave_settings_.enable_log)
        log_file_.write_entry(line + " [" + hex_str + "]");
}

}  // namespace ui::external_app::zwave_rx
