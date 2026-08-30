#pragma once

#include "getnative/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace getnative::media {

struct VideotoolboxFrame {
    std::uintptr_t pixel_buffer = 0U;
    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::int64_t duration = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t bit_depth = 8;
    const char *surface_format = "420v";
    std::string range = "unknown";
    std::shared_ptr<void> lease;
};

// Native VideoToolbox decoder. FFmpeg is used only as a demuxer; packets are
// submitted asynchronously so the media engine can keep multiple frames in
// flight. The FFmpeg hwaccel path waits after every DecodeFrame and cannot.
class VideotoolboxSession {
public:
    VideotoolboxSession(std::uint32_t codec_id, int width, int height,
                        const std::uint8_t *extradata, int extradata_size,
                        std::size_t max_inflight);
    VideotoolboxSession(const VideotoolboxSession &) = delete;
    VideotoolboxSession &operator=(const VideotoolboxSession &) = delete;
    ~VideotoolboxSession();

    void set_timebase(int num, int den);
    [[nodiscard]] bool can_submit() const;
    void submit(const std::uint8_t *data, int size, std::int64_t pts,
                std::int64_t dts, std::int64_t duration);
    // Non-blocking pop. Returns false when no completed frame is ready.
    bool try_pop(VideotoolboxFrame &out);
    // Wait until a frame is ready, the in-flight set drains, stop is requested,
    // or the decoder fails. Returns false when nothing remains.
    bool wait_pop(VideotoolboxFrame &out, std::stop_token stop);
    void finish();
    void flush();
    [[nodiscard]] std::size_t inflight() const;
    [[nodiscard]] bool failed() const;
    [[nodiscard]] std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void submit_ready(const std::uint8_t *data, int size, std::int64_t pts,
                      std::int64_t dts, std::int64_t duration);
};

} // namespace getnative::media
