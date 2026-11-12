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

#include "proc_zwave.hpp"
#include "portapack_shared_memory.hpp"
#include "dsp_fir_taps.hpp"
#include "event_m4.hpp"

#include <cstring>

ZWaveProcessor::ZWaveProcessor() {
    decim_0.configure(taps_200k_wfm_decim_0.taps);
    decim_1.configure(taps_200k_wfm_decim_1.taps);
    demod.configure(demod_fs, 60000);  // headroom for 20-29 kHz deviation plus tuning offset
    decoder.configure(demod_fs);
    baseband_thread.start();
}

void ZWaveProcessor::execute(const buffer_c8_t& buffer) {
    const auto decim_0_out = decim_0.execute(buffer, dst_buffer);
    const auto decim_1_out = decim_1.execute(decim_0_out, dst_buffer);
    feed_channel_stats(decim_1_out);

    const auto f = demod.execute(decim_1_out, fm_buffer);
    for (size_t i = 0; i < f.count; i++)
        decoder.execute(f.p[i]);

    // About twice a second, tell the UI how many preambles were locked and how many frames
    // failed the FCS, so a weak/garbled link can be told apart from "nothing on the air".
    stats_counter += f.count;
    if (stats_counter >= demod_fs / 2) {
        stats_counter = 0;
        send_stats();
    }
}

void ZWaveProcessor::send_stats() {
    ZWavePacketMessage m{};
    m.type = ZWavePacketMessage::Type::Stats;
    m.length = 0;
    for (size_t r = 0; r < 3; r++) {
        const auto rate = static_cast<zwave::Rate>(r);
        m.syncs[r] = static_cast<uint16_t>(decoder.syncs(rate));
        m.bad_fcs[r] = static_cast<uint16_t>(decoder.bad_fcs(rate));
    }
    shared_memory.application_queue.push(m);
}

void ZWaveProcessor::on_packet(void* context, const zwave::Rate rate, const uint8_t* mpdu, const size_t length, const bool fcs_ok) {
    if (length > ZWavePacketMessage::MAX_LENGTH)
        return;
    auto& m = static_cast<ZWaveProcessor*>(context)->packet_message;
    m.type = ZWavePacketMessage::Type::Packet;
    m.fcs_ok = fcs_ok;
    m.rate = static_cast<uint8_t>(rate);
    m.length = static_cast<uint8_t>(length);
    memcpy(m.data, mpdu, length);
    shared_memory.application_queue.push(m);
}

int main() {
    EventDispatcher event_dispatcher{std::make_unique<ZWaveProcessor>()};
    event_dispatcher.run();
    return 0;
}
