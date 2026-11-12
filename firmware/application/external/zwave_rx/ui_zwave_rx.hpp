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

#ifndef __UI_ZWAVE_RX_H__
#define __UI_ZWAVE_RX_H__

#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_receiver.hpp"
#include "ui_freq_field.hpp"
#include "ui_rssi.hpp"
#include "ui_channel.hpp"
#include "app_settings.hpp"
#include "radio_state.hpp"
#include "log_file.hpp"
#include "message.hpp"
#include "zwave_frame.hpp"

#include <deque>
#include <string>

namespace ui::external_app::zwave_rx {

// Scrollable packet log, newest packet at the bottom. With focus on it, Up/Down and the rotary
// encoder scroll back through packet history; Up at the oldest line returns focus to the top bar.
class ZWaveLog : public Widget {
   public:
    enum class Kind : uint8_t {
        Command,
        Decrypted,
        Encrypted,
        AckOrInfo,
        BadFcs,
    };

    ZWaveLog(Rect parent_rect);

    void add(Kind kind, const std::string& text);
    void clear();

    void paint(Painter& painter) override;
    bool on_key(KeyEvent key) override;
    bool on_encoder(EncoderEvent delta) override;
    void on_focus() override { set_dirty(); }
    void on_blur() override { set_dirty(); }

   private:
    struct Entry {
        Kind kind;
        std::string text;
    };
    static constexpr size_t HISTORY_BYTES = 2048;
    std::deque<Entry> entries_{};
    size_t bytes_{0};
    size_t scroll_{0};

    size_t columns() const;
    size_t rows_of(const Entry& e) const;
    size_t visible_rows() const;
    size_t max_scroll() const;
    bool scroll_by(int32_t rows);
};

struct ZWaveSettings {
    uint8_t channel_preset{0};
    bool enable_log{false};
    bool small_font{false};
    bool show_hex{true};

    // Session S0 network key (32 hex chars = 16 bytes), wiped on exit.
    bool use_s0_key{false};
    std::string s0_key_hex{};
    bool key_changed{false};
};

class ZWaveSettingsView : public View {
   public:
    ZWaveSettingsView(NavigationView& nav, ZWaveSettings& settings);
    ~ZWaveSettingsView();

    std::string title() const override { return "Z-Wave Config"; }
    void focus() override { button_save.focus(); }

   private:
    ZWaveSettings& settings_;
    std::string key_hex_{};

    void update_key_button();

    Checkbox check_log{
        {UI_POS_X(2), UI_POS_Y(1)},
        10,
        "Enable Log"};
    Checkbox check_small_font{
        {UI_POS_X(2), UI_POS_Y(3)},
        14,
        "Use Small Font"};
    Checkbox check_show_hex{
        {UI_POS_X(2), UI_POS_Y(5)},
        18,
        "Raw hex + bad FCS"};
    Checkbox check_s0_key{
        {UI_POS_X(2), UI_POS_Y(7)},
        18,
        "S0 Custom Net Key"};
    Button button_key{
        {UI_POS_X(2), UI_POS_Y(9), UI_POS_WIDTH(26), UI_POS_HEIGHT(2)},
        ""};
    Button button_save{
        {UI_POS_X_CENTER(10), UI_POS_Y_BOTTOM(4), UI_POS_WIDTH(10), UI_POS_HEIGHT(2)},
        "Save"};
};

class ZWaveRxView : public View {
   public:
    ZWaveRxView(NavigationView& nav);
    ~ZWaveRxView();

    void focus() override;

    std::string title() const override { return "Z-Wave RX"; }

   private:
    NavigationView& nav_;
    RxRadioState radio_state_{868420000 /* frequency */, 1750000 /* bandwidth */, 3200000 /* sampling rate */};
    ZWaveSettings zwave_settings_{};
    app_settings::SettingsManager settings_{
        "rx_zwave",
        app_settings::Mode::RX,
        {
            {"channel"sv, &zwave_settings_.channel_preset},
            {"log"sv, &zwave_settings_.enable_log},
            {"small_font"sv, &zwave_settings_.small_font},
            {"show_hex"sv, &zwave_settings_.show_hex},
        }};

    LogFile log_file_{};
    S0Keyring keyring_{};
    uint32_t packet_count_{0};

    OptionsField options_channel{
        {UI_POS_X(0), UI_POS_Y(0)},
        12,
        {
            {"EU R1/2 868", 0},
            {"EU R3 869.8", 1},
            {"US R1/2 908", 2},
            {"US R3 916.0", 3},
            {"AU R1/2 921", 4},
            {"AU R3 919.8", 5},
            {"IN 865.20", 6},
            {"RU 869.00", 7},
            {"CN 868.40", 8},
            {"JP Ch1 922", 9},
            {"JP Ch2 923", 10},
            {"KR Ch1 920", 11},
        }};
    RFAmpField field_rf_amp{{UI_POS_X(13), UI_POS_Y(0)}};
    LNAGainField field_lna{{UI_POS_X(15), UI_POS_Y(0)}};
    VGAGainField field_vga{{UI_POS_X(18), UI_POS_Y(0)}};
    RSSI rssi{{UI_POS_X(21), 0, UI_POS_WIDTH_REMAINING(22), 4}};
    Channel channel{{UI_POS_X(21), 5, UI_POS_WIDTH_REMAINING(22), 4}};

    RxFrequencyField field_frequency{{UI_POS_X(0), UI_POS_Y(1)}, nav_};
    Text text_pkt_count{{UI_POS_X(10), UI_POS_Y(1), UI_POS_WIDTH(8), UI_POS_HEIGHT(1)}, "Pkts:0"};
    Button button_clear{{UI_POS_X_RIGHT(12), UI_POS_Y(1), UI_POS_WIDTH(5), UI_POS_HEIGHT(1)}, "Clr"};
    Button button_config{{UI_POS_X_RIGHT(6), UI_POS_Y(1), UI_POS_WIDTH(6), UI_POS_HEIGHT(1)}, "Cfg"};

    ZWaveLog packet_log{
        {UI_POS_X(0), UI_POS_Y(3), UI_POS_MAXWIDTH, UI_POS_HEIGHT_REMAINING(3)}};

    // "R1:syncs/bad R2:syncs/bad R3:syncs/bad" - preambles locked / frames that failed the FCS.
    Text text_stats{{UI_POS_X(0), UI_POS_Y(2), UI_POS_MAXWIDTH, UI_POS_HEIGHT(1)}, "Lk/Bad --"};

    void on_channel_changed(int32_t index);
    void refresh_ui();
    void apply_key();
    void on_packet(const ZWavePacketMessage& message);
    void on_stats(const ZWavePacketMessage& message);

    MessageHandlerRegistration message_handler_packet{
        Message::ID::ZWavePacket,
        [this](Message* const p) {
            this->on_packet(*static_cast<const ZWavePacketMessage*>(p));
        }};
};

}  // namespace ui::external_app::zwave_rx

#endif /*__UI_ZWAVE_RX_H__*/
