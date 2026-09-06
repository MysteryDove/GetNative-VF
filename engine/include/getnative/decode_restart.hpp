#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace getnative::media {

enum class RestartCodec { h264, hevc };

// Inspect NAL unit types only; picture decoding and parameter-set parsing remain
// FFmpeg's responsibility. Unknown/malformed packetization cannot authorize a cut.
inline bool packet_has_closed_restart(std::span<const std::uint8_t> packet,
                                      RestartCodec codec, unsigned length_bytes = 0) {
    const auto classify = [codec](std::span<const std::uint8_t> nal) {
        if (nal.empty() || (nal[0] & 0x80U)) return -1;
        if (codec == RestartCodec::h264) {
            const unsigned type = nal[0] & 31U;
            if (type == 5) return 1;
            return type >= 1 && type <= 4 ? -1 : 0;
        }
        if (nal.size() < 2 || !(nal[1] & 7U)) return -1;
        const unsigned type = (nal[0] >> 1) & 63U;
        if (type >= 16 && type <= 20) return 1;
        return type <= 31 ? -1 : 0;
    };
    if (length_bytes) {
        if (length_bytes > 4) return false;
        std::size_t offset = 0;
        while (offset < packet.size()) {
            if (packet.size() - offset < length_bytes) return false;
            std::uint32_t size = 0;
            for (unsigned i = 0; i < length_bytes; ++i) size = (size << 8) | packet[offset++];
            if (!size || size > packet.size() - offset) return false;
            const int type = classify(packet.subspan(offset, size));
            if (type) return type > 0;
            offset += size;
        }
        return false;
    }
    std::size_t start = packet.size();
    for (std::size_t i = 0; i + 2 < packet.size(); ++i) {
        if (packet[i] || packet[i + 1] || packet[i + 2] != 1) continue;
        if (start < packet.size()) {
            const int type = classify(packet.subspan(start, i - start));
            if (type) return type > 0;
        } else {
            for (std::size_t prefix = 0; prefix < i; ++prefix) if (packet[prefix]) return false;
        }
        start = i + 3;
        i += 2;
    }
    return start < packet.size() && classify(packet.subspan(start)) > 0;
}

} // namespace getnative::media
