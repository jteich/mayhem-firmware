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

#ifndef __PROC_ZWAVE_H__
#define __PROC_ZWAVE_H__

#include "baseband_processor.hpp"
#include "baseband_thread.hpp"
#include "rssi_thread.hpp"

#include "dsp_decimate.hpp"
#include "dsp_demodulate.hpp"
#include "zwave_decoder.hpp"

#include "message.hpp"

class ZWaveProcessor : public BasebandProcessor {
   public:
    ZWaveProcessor();

    void execute(const buffer_c8_t& buffer) override;

   private:
    static constexpr size_t baseband_fs = 3200000;
    static constexpr size_t demod_fs = 400000;

    std::array<complex16_t, 512> dst{};
    const buffer_c16_t dst_buffer{dst.data(), dst.size()};
    std::array<int16_t, 256> fm{};
    const buffer_s16_t fm_buffer{fm.data(), fm.size()};

    // 3200 -> 800 -> 400 kHz with the WFM filters (+/-200 kHz passband).
    dsp::decimate::FIRC8xR16x24FS4Decim4 decim_0{};
    dsp::decimate::FIRC16xR16x16Decim2 decim_1{};
    dsp::demodulate::FM demod{};

    zwave::Decoder decoder{on_packet, this};
    ZWavePacketMessage packet_message{};

    static void on_packet(void* context, zwave::Rate rate, const uint8_t* mpdu, size_t length);

    /* NB: Threads should be the last members in the class definition. */
    BasebandThread baseband_thread{baseband_fs, this, baseband::Direction::Receive, false};
    RSSIThread rssi_thread{};
};

#endif /*__PROC_ZWAVE_H__*/
