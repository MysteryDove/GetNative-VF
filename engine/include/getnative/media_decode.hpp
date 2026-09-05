#pragma once

#include "getnative/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace getnative::media {

enum class ScanSelection : std::uint8_t {
    all,
    every_n,
    decoded_i_picture,
};

struct ScanScope {
    ScanSelection selection = ScanSelection::all;
    std::uint64_t every_n = 0U;
    std::optional<std::uint64_t> start_frame;
    std::optional<std::uint64_t> end_frame;
};

struct FrameIdentity {
    std::uint64_t frame_index = 0U;
    std::uint64_t decode_index = 0U;
    std::optional<std::int64_t> dts;
    std::optional<std::int64_t> pts;
    std::optional<std::int64_t> best_effort_timestamp;
    std::optional<double> timestamp_seconds;
    std::optional<std::int64_t> file_position;
    std::uint32_t packet_size = 0U;
    std::optional<std::int64_t> packet_duration;
    bool key_frame = false;
    bool rap = false;
    bool leading_frame = false;
    std::optional<std::string> picture_type;
    std::optional<std::int32_t> poc;
    std::int32_t repeat_pict = 0;
    std::string field_order = "unknown";
    std::int32_t color_range = 0;
    std::int32_t color_space = 2;
    std::int32_t color_primaries = 2;
    std::int32_t color_transfer = 2;
    std::int32_t chroma_location = 0;
    bool vp8_invisible_frame = false;
    bool vp9_superframe = false;
    std::uint32_t extradata_index = 0U;
    std::uint64_t keyframe_anchor = 0U;
    std::optional<std::int64_t> keyframe_timestamp;
};

struct StreamIndexEntry {
    std::int64_t file_position = -1;
    std::int64_t timestamp = 0;
    std::uint32_t flags = 0U;
    std::uint32_t size = 0U;
    std::uint32_t distance = 0U;
};

struct ExtraDataInfo {
    std::uint32_t index = 0U;
    std::uint32_t codec_id = 0U;
    std::uint32_t fourcc = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string pixel_format;
    std::uint32_t bit_rate = 0U;
    std::vector<std::uint8_t> data;
};

struct MediaIndex {
    static constexpr std::uint32_t format_version = 3U;

    std::string fingerprint;
    std::string source_path;
    std::uint64_t source_size = 0U;
    std::int64_t source_mtime_ns = 0;
    std::uint32_t stream_index = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string codec;
    std::string profile;
    std::string pixel_format;
    std::int32_t bit_depth = 8;
    std::string range = "unknown";
    std::int32_t color_range = 0;
    std::int32_t color_space = 2;
    std::int32_t color_primaries = 2;
    std::int32_t color_transfer = 2;
    std::int32_t chroma_location = 0;
    std::int64_t duration_ticks = 0;
    std::int32_t time_base_num = 0;
    std::int32_t time_base_den = 1;
    std::string decoder = "software";
    std::string format_name;
    std::int64_t format_flags = 0;
    bool raw_demuxer = false;
    std::string index_mode = "packet_fast";
    std::string seek_method = "sample_order";
    std::uint64_t packet_count = 0U;
    std::uint64_t selective_decodes = 0U;
    double index_ms = 0.0;
    std::vector<StreamIndexEntry> stream_index_entries;
    std::vector<ExtraDataInfo> extradata;
    std::vector<std::uint64_t> decode_to_presentation;
    std::vector<std::uint64_t> presentation_to_decode;
    std::vector<FrameIdentity> frames;
};

struct IndexOptions {
    bool rap_verification = false;
    bool allow_lwi = true;
    bool generate_compat_lwi = true;
};

struct IndexedMedia {
    MediaIndex index;
    std::string index_path;
    bool rebuilt = false;
};

struct DecodeTelemetry {
    std::uint64_t decoded_frames = 0U;
    std::uint64_t selected_frames = 0U;
    std::uint64_t decode_retries = 0U;
    // Packets the decoder rejected with AVERROR_INVALIDDATA and dropped
    // (corrupted broadcast captures, damaged sectors). Decoding continues
    // past them; delivered frames are still timestamp-verified.
    std::uint64_t discarded_packets = 0U;
    std::uint64_t host_frame_bytes = 0U;
    std::uint64_t conversion_bytes = 0U;
    std::size_t decode_sessions = 0U;
    double index_ms = 0.0;
    double decode_ms = 0.0;
    double convert_ms = 0.0;
};

struct HostFrame {
    std::uint64_t seq = 0U;
    FrameIdentity identity;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<float> pixels;
    // Populated only for preview requests. Analysis and verification keep the
    // lower-bandwidth luma-only representation above.
    std::vector<std::uint8_t> rgb24;
};

struct PreviewImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> png;
};

enum class CudaLumaFormat : std::uint8_t {
    nv12,
    p010,
    p016,
    yuv444p8,
    yuv444p16,
};

struct CudaFrame {
    std::uint64_t seq = 0U;
    FrameIdentity identity;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uintptr_t device_pointer = 0U;
    std::size_t pitch_bytes = 0U;
    CudaLumaFormat format = CudaLumaFormat::nv12;
    std::int32_t bit_depth = 8;
    std::string range = "unknown";
    std::uintptr_t context = 0U;
    std::uintptr_t producer_stream = 0U;
    // Keeps the decoder-owned AVFrame (and therefore its hardware surface)
    // alive until analysis has finished with this frame.
    std::shared_ptr<void> lease;
};

struct VulkanFrame {
    std::uint64_t seq = 0U;
    FrameIdentity identity;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uintptr_t image = 0U;
    std::uint32_t image_format = 0U;
    std::uint32_t view_format = 0U;
    std::uint32_t aspect_mask = 0U;
    std::int32_t bit_depth = 8;
    std::int32_t normalized_sample_bits = 8;
    std::string range = "unknown";
    std::uint32_t layout = 0U;
    std::uint32_t access = 0U;
    std::uint32_t queue_family = 0U;
    std::uintptr_t semaphore = 0U;
    std::uint64_t semaphore_value = 0U;
    void *sync_opaque = nullptr;
    void (*mark_submitted)(void *opaque, std::uint32_t layout,
                           std::uint32_t access, std::uint32_t queue_family,
                           std::uint64_t semaphore_value) = nullptr;
    void (*release_without_submit)(void *opaque) = nullptr;
    // Owns both the AVFrame reference and the frame lock represented by the
    // callbacks above. The lock remains valid after the decode callback exits.
    std::shared_ptr<void> lease;
};

// Decoder-owned VideoToolbox surface. The pixel_buffer value is an opaque
// CVPixelBufferRef on macOS so this public header remains portable.
struct MetalFrame {
    std::uint64_t seq = 0U;
    FrameIdentity identity;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uintptr_t pixel_buffer = 0U;
    std::int32_t bit_depth = 8;
    std::string range = "unknown";
    std::string surface_format;
    std::shared_ptr<void> lease;
};

struct DecoderOptions {
    // Hardware decode is deliberately opt-in. A caller that cannot provide a
    // compatible device context must use software rather than silently moving
    // frames through an unrelated GPU.
    enum class Backend : std::uint8_t {
        software,
        cuda,
        vaapi,
        d3d11va,
        vulkan_video,
        videotoolbox,
    } backend = Backend::software;
    std::uintptr_t native_instance = 0U;
    std::uintptr_t native_physical_device = 0U;
    std::uintptr_t native_device = 0U;
    std::uintptr_t native_context = 0U;
    std::uintptr_t native_queue = 0U;
    std::uint32_t native_compute_queue_family = 0U;
    std::uint32_t native_decode_queue_family = 0U;
    std::uint32_t native_video_codec_operations = 0U;
    std::uint32_t native_instance_api_version = 0U;
    bool native_timeline_semaphore = false;
    std::vector<std::string> native_device_extensions;
    void *native_queue_lock_opaque = nullptr;
    void (*lock_native_queue)(void *opaque) = nullptr;
    void (*unlock_native_queue)(void *opaque) = nullptr;
    std::int32_t expected_bit_depth = 0;
    bool output_rgb = false;
    // Pure preview sessions convert directly from the decoded pixel format to
    // this bounding dimension. Zero keeps the source dimensions.
    std::int32_t preview_maximum_dimension = 0;
    // Decode luma into HostFrame::pixels. PNG-only preview callers can
    // disable this to skip the full-resolution float conversion.
    bool output_luma = true;
    // Number of decoded frames the caller may retain concurrently.
    std::size_t frame_concurrency = 2U;
};

using IndexProgress = std::function<void(std::uint64_t indexed_records)>;
using FrameConsumer = std::function<void(HostFrame frame)>;
using CudaFrameConsumer = std::function<void(CudaFrame frame)>;
using VulkanFrameConsumer = std::function<void(VulkanFrame frame)>;
using MetalFrameConsumer = std::function<void(MetalFrame frame)>;

[[nodiscard]] bool compiled() noexcept;
[[nodiscard]] std::string runtime_version();
[[nodiscard]] bool backend_compiled(DecoderOptions::Backend backend) noexcept;
[[nodiscard]] bool backend_runtime_available(DecoderOptions::Backend backend) noexcept;
[[nodiscard]] std::vector<std::string> hardware_codecs(DecoderOptions::Backend backend);
/** Preferred host-copy hardware decoder: D3D11VA, NVDEC, then VAAPI. */
[[nodiscard]] DecoderOptions::Backend preferred_host_hwdec() noexcept;
[[nodiscard]] const char *decoder_backend_id(DecoderOptions::Backend backend) noexcept;

[[nodiscard]] std::string quick_fingerprint(const std::string &path);
[[nodiscard]] std::uint32_t default_video_stream(const std::string &path);
[[nodiscard]] MediaIndex index_media(const std::string &path, std::uint32_t stream_index,
                                     std::stop_token stop = {},
                                     IndexProgress progress = {});
[[nodiscard]] MediaIndex index_media(const std::string &path,
                                     std::uint32_t stream_index,
                                     const DecoderOptions &options,
                                     std::stop_token stop = {},
                                     IndexProgress progress = {});
[[nodiscard]] MediaIndex index_media(const std::string &path,
                                     std::uint32_t stream_index,
                                     const IndexOptions &index_options,
                                     std::stop_token stop = {},
                                     IndexProgress progress = {});
[[nodiscard]] MediaIndex index_media(const std::string &path,
                                     std::uint32_t stream_index,
                                     const DecoderOptions &options,
                                     const IndexOptions &index_options,
                                     std::stop_token stop = {},
                                     IndexProgress progress = {});
[[nodiscard]] std::string preferred_index_path(const std::string &path,
                                               std::uint32_t stream_index,
                                               std::uint32_t default_stream_index);
[[nodiscard]] MediaIndex load_index(const std::string &index_path,
                                    const std::string &media_path,
                                    std::optional<std::string_view> expected_fingerprint = {},
                                    std::optional<std::uint32_t> expected_stream = {});
void write_index_atomic(const std::string &index_path, const MediaIndex &index,
                        std::stop_token stop = {});
[[nodiscard]] IndexedMedia ensure_index(
    const std::string &path, std::optional<std::uint32_t> stream_index,
    const std::string &cache_directory, const DecoderOptions &options = {},
    std::stop_token stop = {}, IndexProgress progress = {});
[[nodiscard]] IndexedMedia ensure_index(
    const std::string &path, std::optional<std::uint32_t> stream_index,
    const std::string &cache_directory, const DecoderOptions &options,
    const IndexOptions &index_options, std::stop_token stop = {},
    IndexProgress progress = {});
[[nodiscard]] std::vector<FrameIdentity> select_frames(const MediaIndex &index,
                                                        const ScanScope &scope);
[[nodiscard]] std::vector<FrameIdentity> frame_window(
    const MediaIndex &index, std::uint64_t target_frame, std::uint32_t radius);
void decode_selected(const std::string &path, std::uint32_t stream_index,
                     std::span<const FrameIdentity> selected,
                     const DecoderOptions &options, std::stop_token stop,
                     const FrameConsumer &consumer, DecodeTelemetry *telemetry = nullptr);
void decode_selected_indexed(const std::string &path, const MediaIndex &index,
                             std::span<const FrameIdentity> selected,
                             const DecoderOptions &options, std::stop_token stop,
                             const FrameConsumer &consumer,
                             DecodeTelemetry *telemetry = nullptr);

// Unix-epoch mtime of a media file in nanoseconds; matches the value stored
// in MediaIndex::source_mtime_ns. Cheap stat used to validate cached indexes.
[[nodiscard]] std::int64_t source_mtime_unix_ns(const std::string &path);

// Persistent indexed-decode session. Holds the demuxer and decoder open
// across requests so that a later seek only pays avformat_seek_file +
// avcodec_flush_buffers, and forward motion within one GOP continues
// decoding without any seek at all. The session owns a reference to the
// index; cancelling a decode leaves the session consistent (the next decode
// simply repositions).
class IndexedDecodeSession {
public:
    IndexedDecodeSession(const std::string &path,
                         std::shared_ptr<const MediaIndex> index,
                         const DecoderOptions &options);
    ~IndexedDecodeSession();
    IndexedDecodeSession(const IndexedDecodeSession &) = delete;
    IndexedDecodeSession &operator=(const IndexedDecodeSession &) = delete;

    [[nodiscard]] DecoderOptions::Backend backend() const;
    void decode(std::span<const FrameIdentity> selected, std::stop_token stop,
                const FrameConsumer &consumer, DecodeTelemetry *telemetry = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] PreviewImage encode_preview_png(const HostFrame &frame,
                                              std::int32_t maximum_dimension);
void decode_selected_cuda(const std::string &path, const MediaIndex &index,
                          std::span<const FrameIdentity> selected,
                          const DecoderOptions &options, std::stop_token stop,
                          const CudaFrameConsumer &consumer,
                          DecodeTelemetry *telemetry = nullptr);
void decode_selected_vulkan(const std::string &path, const MediaIndex &index,
                            std::span<const FrameIdentity> selected,
                            const DecoderOptions &options, std::stop_token stop,
                            const VulkanFrameConsumer &consumer,
                            DecodeTelemetry *telemetry = nullptr);
void decode_selected_metal(const std::string &path, const MediaIndex &index,
                           std::span<const FrameIdentity> selected,
                           const DecoderOptions &options, std::stop_token stop,
                           const MetalFrameConsumer &consumer,
                           DecodeTelemetry *telemetry = nullptr);

} // namespace getnative::media
