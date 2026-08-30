#include "videotoolbox_decode.hpp"

#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
}

namespace getnative::media {
namespace {

[[nodiscard]] bool looks_annexb(const std::uint8_t *data, int size) noexcept {
    return size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

void split_annexb(const std::uint8_t *data, int size,
                  std::vector<std::vector<std::uint8_t>> &nals) {
    nals.clear();
    int i = 0;
    while (i + 3 <= size) {
        int start = -1;
        int sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start = i + 3;
            sc = 3;
        } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0
                   && data[i + 3] == 1) {
            start = i + 4;
            sc = 4;
        }
        if (start < 0) {
            ++i;
            continue;
        }
        int next = size;
        for (int j = start; j + 3 <= size; ++j) {
            if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
                next = j;
                break;
            }
            if (j + 4 <= size && data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 0
                && data[j + 3] == 1) {
                next = j;
                break;
            }
        }
        if (next > start) {
            nals.emplace_back(data + start, data + next);
        }
        i = next;
        (void)sc;
    }
}

[[nodiscard]] std::vector<std::uint8_t> to_avcc(const std::uint8_t *data, int size) {
    if (!looks_annexb(data, size)) {
        return {data, data + size};
    }
    std::vector<std::vector<std::uint8_t>> nals;
    split_annexb(data, size, nals);
    std::vector<std::uint8_t> out;
    for (const auto &nal : nals) {
        const auto n = static_cast<std::uint32_t>(nal.size());
        out.push_back(static_cast<std::uint8_t>(n >> 24));
        out.push_back(static_cast<std::uint8_t>(n >> 16));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

[[nodiscard]] std::uint8_t h264_nal_type(const std::vector<std::uint8_t> &nal) noexcept {
    return nal.empty() ? 0 : nal[0] & 0x1f;
}

[[nodiscard]] std::uint8_t hevc_nal_type(const std::vector<std::uint8_t> &nal) noexcept {
    return nal.empty() ? 0 : (nal[0] >> 1) & 0x3f;
}

void collect_h264_parameter_sets(const std::uint8_t *data, int size,
                                 std::vector<std::vector<std::uint8_t>> &sps,
                                 std::vector<std::vector<std::uint8_t>> &pps) {
    sps.clear();
    pps.clear();
    if (size <= 0 || data == nullptr) return;
    if (looks_annexb(data, size)) {
        std::vector<std::vector<std::uint8_t>> nals;
        split_annexb(data, size, nals);
        for (auto &nal : nals) {
            const auto type = h264_nal_type(nal);
            if (type == 7) sps.push_back(std::move(nal));
            else if (type == 8) pps.push_back(std::move(nal));
        }
        return;
    }
    if (size < 7 || data[0] != 1) return;
    int offset = 5;
    const int sps_count = data[offset++] & 0x1f;
    for (int i = 0; i < sps_count && offset + 2 <= size; ++i) {
        const int n = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (n < 0 || offset + n > size) return;
        sps.emplace_back(data + offset, data + offset + n);
        offset += n;
    }
    if (offset >= size) return;
    const int pps_count = data[offset++];
    for (int i = 0; i < pps_count && offset + 2 <= size; ++i) {
        const int n = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (n < 0 || offset + n > size) return;
        pps.emplace_back(data + offset, data + offset + n);
        offset += n;
    }
}

void collect_hevc_parameter_sets(const std::uint8_t *data, int size,
                                 std::vector<std::vector<std::uint8_t>> &vps,
                                 std::vector<std::vector<std::uint8_t>> &sps,
                                 std::vector<std::vector<std::uint8_t>> &pps) {
    vps.clear();
    sps.clear();
    pps.clear();
    if (size <= 0 || data == nullptr) return;
    if (!looks_annexb(data, size)) return;
    std::vector<std::vector<std::uint8_t>> nals;
    split_annexb(data, size, nals);
    for (auto &nal : nals) {
        const auto type = hevc_nal_type(nal);
        if (type == 32) vps.push_back(std::move(nal));
        else if (type == 33) sps.push_back(std::move(nal));
        else if (type == 34) pps.push_back(std::move(nal));
    }
}

[[nodiscard]] CMTime packet_time(std::int64_t ts, int tb_num, int tb_den) {
    if (ts == AV_NOPTS_VALUE || tb_den <= 0) return kCMTimeInvalid;
    if (tb_num <= 0) tb_num = 1;
    return CMTimeMake(ts * static_cast<std::int64_t>(tb_num), tb_den);
}

[[nodiscard]] std::int64_t cm_to_stream_ts(CMTime time, int tb_num, int tb_den) {
    if (!CMTIME_IS_NUMERIC(time) || tb_den <= 0) return AV_NOPTS_VALUE;
    if (tb_num <= 0) tb_num = 1;
    const CMTime converted = CMTimeConvertScale(
        time, tb_den, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
    if (!CMTIME_IS_NUMERIC(converted) || converted.timescale == 0) return AV_NOPTS_VALUE;
    if (tb_num == 1) return converted.value;
    return converted.value / tb_num;
}

struct PixelInfo {
    const char *surface = "420v";
    std::int32_t bit_depth = 8;
    std::string range = "limited";
};

[[nodiscard]] PixelInfo describe_pixel_buffer(CVPixelBufferRef buffer) {
    PixelInfo info;
    const OSType format = CVPixelBufferGetPixelFormatType(buffer);
    switch (format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        info = {"420v", 8, "limited"};
        break;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        info = {"420f", 8, "full"};
        break;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        info = {"x420", 10, "limited"};
        break;
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        info = {"xf20", 10, "full"};
        break;
    default:
        break;
    }
    return info;
}

} // namespace

struct PendingPacket {
    std::vector<std::uint8_t> data;
    std::int64_t pts = AV_NOPTS_VALUE;
    std::int64_t dts = AV_NOPTS_VALUE;
    std::int64_t duration = 0;
};

struct VideotoolboxSession::Impl {
    VTDecompressionSessionRef session = nullptr;
    CMVideoFormatDescriptionRef format = nullptr;
    std::size_t max_inflight = 8;
    int tb_num = 1;
    int tb_den = 90000;
    int width = 0;
    int height = 0;
    std::uint32_t codec_id = 0;
    std::vector<PendingPacket> pending;

    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<VideotoolboxFrame> outputs;
    std::size_t inflight = 0;
    bool failed = false;
    std::string error;

    static void callback(void *opaque, void *, OSStatus status, VTDecodeInfoFlags,
                         CVImageBufferRef image, CMTime pts, CMTime duration) {
        auto *self = static_cast<Impl *>(opaque);
        std::unique_lock lock(self->mutex);
        if (self->inflight > 0) --self->inflight;
        if (status != noErr) {
            self->failed = true;
            self->error = "VTDecompressionSession callback failed (" + std::to_string(status) + ")";
            self->ready.notify_all();
            return;
        }
        if (image == nullptr || CFGetTypeID(image) != CVPixelBufferGetTypeID()) {
            self->ready.notify_all();
            return;
        }
        auto *buffer = static_cast<CVPixelBufferRef>(image);
        if (!CVPixelBufferIsPlanar(buffer) || CVPixelBufferGetIOSurface(buffer) == nullptr) {
            self->failed = true;
            self->error = "VideoToolbox produced a non-IOSurface pixel buffer";
            self->ready.notify_all();
            return;
        }
        CVPixelBufferRetain(buffer);
        const PixelInfo info = describe_pixel_buffer(buffer);
        VideotoolboxFrame frame;
        frame.pixel_buffer = reinterpret_cast<std::uintptr_t>(buffer);
        frame.pts = cm_to_stream_ts(pts, self->tb_num, self->tb_den);
        frame.dts = AV_NOPTS_VALUE;
        frame.duration = cm_to_stream_ts(duration, self->tb_num, self->tb_den);
        if (frame.duration == AV_NOPTS_VALUE) frame.duration = 0;
        frame.width = static_cast<std::int32_t>(CVPixelBufferGetWidth(buffer));
        frame.height = static_cast<std::int32_t>(CVPixelBufferGetHeight(buffer));
        frame.bit_depth = info.bit_depth;
        frame.surface_format = info.surface;
        frame.range = info.range;
        frame.lease = std::shared_ptr<void>{buffer, [](void *opaque) {
            if (opaque != nullptr) {
                CVPixelBufferRelease(static_cast<CVPixelBufferRef>(opaque));
            }
        }};
        self->outputs.push_back(std::move(frame));
        self->ready.notify_all();
    }

    void create_from_atoms(CMVideoCodecType codec_type, const char *atom,
                           const std::uint8_t *extradata, int extradata_size) {
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, extradata, extradata_size);
        CFStringRef atom_key = CFStringCreateWithCString(
            kCFAllocatorDefault, atom, kCFStringEncodingASCII);
        CFMutableDictionaryRef atoms = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef extensions = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(atoms, atom_key, data);
        CFDictionarySetValue(
            extensions, CFSTR("SampleDescriptionExtensionAtoms"), atoms);
        const OSStatus status = CMVideoFormatDescriptionCreate(
            kCFAllocatorDefault, codec_type, width, height, extensions, &format);
        CFRelease(data);
        CFRelease(atom_key);
        CFRelease(atoms);
        CFRelease(extensions);
        if (status != noErr || format == nullptr) {
            throw std::runtime_error("CMVideoFormatDescriptionCreate failed");
        }
    }

    void create_format(std::uint32_t codec_id, const std::uint8_t *extradata,
                       int extradata_size) {
        if (codec_id == AV_CODEC_ID_H264) {
            std::vector<std::vector<std::uint8_t>> sps;
            std::vector<std::vector<std::uint8_t>> pps;
            collect_h264_parameter_sets(extradata, extradata_size, sps, pps);
            if (!sps.empty() && !pps.empty()) {
                const std::uint8_t *sets[2] = {sps.front().data(), pps.front().data()};
                const size_t sizes[2] = {sps.front().size(), pps.front().size()};
                const OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    kCFAllocatorDefault, 2, sets, sizes, 4, &format);
                if (status == noErr && format != nullptr) return;
            }
            if (extradata != nullptr && extradata_size > 0) {
                create_from_atoms(kCMVideoCodecType_H264, "avcC", extradata, extradata_size);
                return;
            }
            throw std::runtime_error("H.264 VideoToolbox session needs SPS/PPS");
        }
        if (codec_id == AV_CODEC_ID_HEVC) {
            std::vector<std::vector<std::uint8_t>> vps;
            std::vector<std::vector<std::uint8_t>> sps;
            std::vector<std::vector<std::uint8_t>> pps;
            collect_hevc_parameter_sets(extradata, extradata_size, vps, sps, pps);
            if (!vps.empty() && !sps.empty() && !pps.empty()) {
                const std::uint8_t *sets[3] = {
                    vps.front().data(), sps.front().data(), pps.front().data()};
                const size_t sizes[3] = {
                    vps.front().size(), sps.front().size(), pps.front().size()};
                const OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    kCFAllocatorDefault, 3, sets, sizes, 4, nullptr, &format);
                if (status == noErr && format != nullptr) return;
            }
            if (extradata != nullptr && extradata_size > 0) {
                create_from_atoms(kCMVideoCodecType_HEVC, "hvcC", extradata, extradata_size);
                return;
            }
            throw std::runtime_error("HEVC VideoToolbox session needs VPS/SPS/PPS");
        }
        throw std::runtime_error("native VideoToolbox decode currently supports H.264 and HEVC");
    }

    bool try_create_from_bytes(const std::uint8_t *data, int size) {
        try {
            if (format != nullptr) {
                CFRelease(format);
                format = nullptr;
            }
            create_format(codec_id, data, size);
            create_session();
            return true;
        } catch (...) {
            if (format != nullptr) {
                CFRelease(format);
                format = nullptr;
            }
            if (session != nullptr) {
                VTDecompressionSessionInvalidate(session);
                CFRelease(session);
                session = nullptr;
            }
            return false;
        }
    }

    bool try_create_from_pending() {
        if (session != nullptr) return true;
        std::vector<std::uint8_t> joined;
        for (const auto &packet : pending) {
            joined.insert(joined.end(), packet.data.begin(), packet.data.end());
        }
        if (joined.empty()) return false;
        return try_create_from_bytes(joined.data(), static_cast<int>(joined.size()));
    }

    void create_session() {
        CFMutableDictionaryRef io_surface = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef buffer_attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        const OSType pixfmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef pix = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixfmt);
        CFDictionarySetValue(buffer_attrs, kCVPixelBufferPixelFormatTypeKey, pix);
        CFDictionarySetValue(buffer_attrs, kCVPixelBufferIOSurfacePropertiesKey, io_surface);
        CFDictionarySetValue(buffer_attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
        const int w = width;
        const int h = height;
        if (w > 0 && h > 0) {
            CFNumberRef width_n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
            CFNumberRef height_n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
            CFDictionarySetValue(buffer_attrs, kCVPixelBufferWidthKey, width_n);
            CFDictionarySetValue(buffer_attrs, kCVPixelBufferHeightKey, height_n);
            CFRelease(width_n);
            CFRelease(height_n);
        }

        CFMutableDictionaryRef decoder_spec = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(
            decoder_spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
            kCFBooleanTrue);

        VTDecompressionOutputCallbackRecord record{
            Impl::callback, this,
        };
        const OSStatus status = VTDecompressionSessionCreate(
            kCFAllocatorDefault, format, decoder_spec, buffer_attrs, &record, &session);
        CFRelease(pix);
        CFRelease(io_surface);
        CFRelease(buffer_attrs);
        CFRelease(decoder_spec);
        if (status != noErr || session == nullptr) {
            throw std::runtime_error(
                "VTDecompressionSessionCreate failed (" + std::to_string(status) + ")");
        }
        CFBooleanRef realtime = kCFBooleanFalse;
        (void)VTSessionSetProperty(session, kVTDecompressionPropertyKey_RealTime, realtime);
    }

    void destroy() noexcept {
        if (session != nullptr) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        if (format != nullptr) {
            CFRelease(format);
            format = nullptr;
        }
        outputs.clear();
        inflight = 0;
    }
};

VideotoolboxSession::VideotoolboxSession(std::uint32_t codec_id, int width, int height,
                                         const std::uint8_t *extradata, int extradata_size,
                                         std::size_t max_inflight)
    : impl_(std::make_unique<Impl>()) {
    impl_->max_inflight = std::max<std::size_t>(max_inflight, 2U);
    impl_->width = width;
    impl_->height = height;
    impl_->codec_id = codec_id;
    if (extradata != nullptr && extradata_size > 0) {
        (void)impl_->try_create_from_bytes(extradata, extradata_size);
    }
}

VideotoolboxSession::~VideotoolboxSession() {
    if (impl_) impl_->destroy();
}

void VideotoolboxSession::set_timebase(int num, int den) {
    std::lock_guard lock(impl_->mutex);
    impl_->tb_num = num > 0 ? num : 1;
    impl_->tb_den = den > 0 ? den : 90000;
}

bool VideotoolboxSession::can_submit() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->failed) return false;
    if (impl_->session == nullptr) return impl_->pending.size() < 64U;
    return impl_->inflight < impl_->max_inflight;
}

void VideotoolboxSession::submit(const std::uint8_t *data, int size, std::int64_t pts,
                                 std::int64_t dts, std::int64_t duration) {
    if (data == nullptr || size <= 0) return;
    if (impl_->session == nullptr) {
        PendingPacket packet;
        packet.data.assign(data, data + size);
        packet.pts = pts;
        packet.dts = dts;
        packet.duration = duration;
        impl_->pending.push_back(std::move(packet));
        if (!impl_->try_create_from_pending()) {
            if (impl_->pending.size() > 64U) {
                throw std::runtime_error("VideoToolbox never received H.264/HEVC parameter sets");
            }
            return;
        }
        auto queued = std::move(impl_->pending);
        impl_->pending.clear();
        for (const auto &item : queued) {
            submit_ready(item.data.data(), static_cast<int>(item.data.size()),
                         item.pts, item.dts, item.duration);
        }
        return;
    }
    submit_ready(data, size, pts, dts, duration);
}

void VideotoolboxSession::submit_ready(const std::uint8_t *data, int size, std::int64_t pts,
                                       std::int64_t dts, std::int64_t duration) {
    {
        std::unique_lock lock(impl_->mutex);
        impl_->ready.wait(lock, [&] {
            return impl_->failed || impl_->inflight < impl_->max_inflight;
        });
        if (impl_->failed) {
            throw std::runtime_error(impl_->error.empty() ? "VideoToolbox decode failed"
                                                          : impl_->error);
        }
        ++impl_->inflight;
    }

    const std::vector<std::uint8_t> avcc = to_avcc(data, size);
    if (avcc.empty()) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->inflight > 0) --impl_->inflight;
        impl_->ready.notify_all();
        return;
    }

    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, nullptr, avcc.size(), kCFAllocatorDefault, nullptr, 0,
        avcc.size(), 0, &block);
    if (status == noErr) {
        status = CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
    }
    if (status != noErr || block == nullptr) {
        if (block != nullptr) CFRelease(block);
        std::lock_guard lock(impl_->mutex);
        if (impl_->inflight > 0) --impl_->inflight;
        impl_->failed = true;
        impl_->error = "CMBlockBufferCreateWithMemoryBlock failed";
        impl_->ready.notify_all();
        throw std::runtime_error(impl_->error);
    }

    const CMTime pts_time = packet_time(pts, impl_->tb_num, impl_->tb_den);
    CMSampleTimingInfo timing;
    timing.duration = duration > 0 ? packet_time(duration, impl_->tb_num, impl_->tb_den)
                                   : kCMTimeInvalid;
    timing.presentationTimeStamp = pts_time;
    timing.decodeTimeStamp = dts != AV_NOPTS_VALUE
        ? packet_time(dts, impl_->tb_num, impl_->tb_den)
        : kCMTimeInvalid;
    const size_t sample_size = avcc.size();
    CMSampleBufferRef sample = nullptr;
    status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, impl_->format, 1,
                                       1, &timing, 1, &sample_size, &sample);
    CFRelease(block);
    if (status != noErr || sample == nullptr) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->inflight > 0) --impl_->inflight;
        impl_->failed = true;
        impl_->error = "CMSampleBufferCreateReady failed";
        impl_->ready.notify_all();
        throw std::runtime_error(impl_->error);
    }

    const VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression
        | kVTDecodeFrame_EnableTemporalProcessing;
    status = VTDecompressionSessionDecodeFrame(impl_->session, sample, flags, nullptr, nullptr);
    CFRelease(sample);
    if (status != noErr) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->inflight > 0) --impl_->inflight;
        impl_->failed = true;
        impl_->error = "VTDecompressionSessionDecodeFrame failed (" + std::to_string(status) + ")";
        impl_->ready.notify_all();
        throw std::runtime_error(impl_->error);
    }
}

bool VideotoolboxSession::try_pop(VideotoolboxFrame &out) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->outputs.empty()) return false;
    out = std::move(impl_->outputs.front());
    impl_->outputs.pop_front();
    impl_->ready.notify_all();
    return true;
}

bool VideotoolboxSession::wait_pop(VideotoolboxFrame &out, std::stop_token stop) {
    std::unique_lock lock(impl_->mutex);
    while (!stop.stop_requested()) {
        if (impl_->failed) {
            throw std::runtime_error(impl_->error.empty() ? "VideoToolbox decode failed"
                                                          : impl_->error);
        }
        if (!impl_->outputs.empty()) {
            out = std::move(impl_->outputs.front());
            impl_->outputs.pop_front();
            impl_->ready.notify_all();
            return true;
        }
        if (impl_->inflight == 0) return false;
        impl_->ready.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

void VideotoolboxSession::finish() {
    if (impl_->session != nullptr) {
        (void)VTDecompressionSessionFinishDelayedFrames(impl_->session);
        (void)VTDecompressionSessionWaitForAsynchronousFrames(impl_->session);
    }
}

void VideotoolboxSession::flush() {
    if (impl_->session != nullptr) {
        (void)VTDecompressionSessionFinishDelayedFrames(impl_->session);
        (void)VTDecompressionSessionWaitForAsynchronousFrames(impl_->session);
    }
    std::lock_guard lock(impl_->mutex);
    impl_->outputs.clear();
    impl_->inflight = 0;
    impl_->ready.notify_all();
}

std::size_t VideotoolboxSession::inflight() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->inflight;
}

bool VideotoolboxSession::failed() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->failed;
}

std::string VideotoolboxSession::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}

} // namespace getnative::media
