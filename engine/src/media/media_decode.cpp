#include "getnative/media_decode.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hash.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#if defined(GETNATIVE_HAS_CUDA)
extern "C" {
#include <libavutil/hwcontext_cuda.h>
}
#include <cuda.h>
#endif

#if defined(GETNATIVE_HAS_VULKAN)
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#include <vulkan/vulkan.h>
#endif

namespace getnative::media {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::string ffmpeg_error(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return std::string{buffer.data()};
}

void check_ffmpeg(int error, std::string_view operation) {
    if (error < 0) {
        throw std::runtime_error(std::string{operation} + ": " + ffmpeg_error(error));
    }
}

struct FormatCloser {
    void operator()(AVFormatContext *context) const noexcept {
        if (context != nullptr) avformat_close_input(&context);
    }
};
struct CodecCloser {
    void operator()(AVCodecContext *context) const noexcept {
        avcodec_free_context(&context);
    }
};
struct FrameCloser {
    void operator()(AVFrame *frame) const noexcept { av_frame_free(&frame); }
};
struct PacketCloser {
    void operator()(AVPacket *packet) const noexcept { av_packet_free(&packet); }
};
struct ParserCloser {
    void operator()(AVCodecParserContext *parser) const noexcept {
        if (parser != nullptr) av_parser_close(parser);
    }
};
struct BsfCloser {
    void operator()(AVBSFContext *context) const noexcept {
        av_bsf_free(&context);
    }
};
struct BufferCloser {
    void operator()(AVBufferRef *buffer) const noexcept { av_buffer_unref(&buffer); }
};
struct SwsCloser {
    void operator()(SwsContext *context) const noexcept { sws_freeContext(context); }
};
struct HashCloser {
    void operator()(AVHashContext *context) const noexcept { av_hash_freep(&context); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using ParserPtr = std::unique_ptr<AVCodecParserContext, ParserCloser>;
using BsfPtr = std::unique_ptr<AVBSFContext, BsfCloser>;
using BufferPtr = std::unique_ptr<AVBufferRef, BufferCloser>;
using SwsPtr = std::unique_ptr<SwsContext, SwsCloser>;
using HashPtr = std::unique_ptr<AVHashContext, HashCloser>;

[[maybe_unused, nodiscard]] std::shared_ptr<void> retain_frame(const AVFrame &source,
                                                              std::string_view operation) {
    AVFrame *frame = av_frame_clone(&source);
    if (frame == nullptr) {
        throw std::runtime_error(std::string{operation} + " failed");
    }
    return std::shared_ptr<void>{frame, [](void *opaque) {
        auto *owned = static_cast<AVFrame *>(opaque);
        av_frame_free(&owned);
    }};
}

struct OpenedDecoder {
    FormatPtr format;
    CodecPtr codec;
    const AVStream *stream = nullptr;
    const AVCodec *decoder = nullptr;
};

#if defined(GETNATIVE_HAS_CUDA)
[[nodiscard]] AVPixelFormat select_cuda_format(
    AVCodecContext *, const AVPixelFormat *formats) {
    for (const AVPixelFormat *format = formats;
         *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_CUDA) return *format;
    }
    return AV_PIX_FMT_NONE;
}

[[nodiscard]] bool decoder_supports_cuda(const AVCodec &decoder) {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(&decoder, index);
        if (config == nullptr) return false;
        if (config->device_type == AV_HWDEVICE_TYPE_CUDA
            && config->pix_fmt == AV_PIX_FMT_CUDA
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return true;
        }
    }
}

void configure_cuda_decoder(AVCodecContext &codec, const AVCodec &decoder,
                            const DecoderOptions &options) {
    if (options.native_context == 0U || options.native_queue == 0U) {
        throw std::runtime_error(
            "CUDA hardware decode requires a shared context and stream");
    }
    if (!decoder_supports_cuda(decoder)) {
        throw std::runtime_error(
            "FFmpeg decoder exposes no CUDA hardware-device configuration");
    }
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (device_ref == nullptr) {
        throw std::runtime_error("av_hwdevice_ctx_alloc(CUDA) failed");
    }
    auto *device = reinterpret_cast<AVHWDeviceContext *>(device_ref->data);
    auto *cuda = static_cast<AVCUDADeviceContext *>(device->hwctx);
    cuda->cuda_ctx = reinterpret_cast<CUcontext>(options.native_context);
    cuda->stream = reinterpret_cast<CUstream>(options.native_queue);
    const int init_result = av_hwdevice_ctx_init(device_ref);
    if (init_result < 0) {
        av_buffer_unref(&device_ref);
        check_ffmpeg(init_result, "av_hwdevice_ctx_init(CUDA)");
    }
    codec.hw_device_ctx = device_ref;
    codec.get_format = select_cuda_format;
    codec.extra_hw_frames = static_cast<int>(8U + options.frame_concurrency);
}
#endif

#if defined(GETNATIVE_HAS_VULKAN)
template <class Handle>
[[nodiscard]] Handle native_handle(std::uintptr_t value) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}

template <class Handle>
[[nodiscard]] std::uintptr_t native_value(Handle value) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<std::uintptr_t>(value);
    } else {
        return static_cast<std::uintptr_t>(value);
    }
}

[[nodiscard]] AVPixelFormat select_vulkan_format(
    AVCodecContext *, const AVPixelFormat *formats) {
    for (const AVPixelFormat *format = formats;
         *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VULKAN) return *format;
    }
    return AV_PIX_FMT_NONE;
}

[[nodiscard]] bool decoder_supports_vulkan(const AVCodec &decoder) {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(&decoder, index);
        if (config == nullptr) return false;
        if (config->device_type == AV_HWDEVICE_TYPE_VULKAN
            && config->pix_fmt == AV_PIX_FMT_VULKAN
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return true;
        }
    }
}

[[nodiscard]] VkVideoCodecOperationFlagBitsKHR codec_video_operation(
    AVCodecID codec) {
    switch (codec) {
    case AV_CODEC_ID_H264:
        return VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    case AV_CODEC_ID_HEVC:
        return VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    case AV_CODEC_ID_AV1:
        return VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
#if defined(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)
    case AV_CODEC_ID_VP9:
        return VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
#endif
    default:
        return VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    }
}

struct VulkanDeviceOwner {
    explicit VulkanDeviceOwner(const DecoderOptions &source)
        : extensions(source.native_device_extensions),
          queue_lock_opaque(source.native_queue_lock_opaque),
          lock_queue(source.lock_native_queue),
          unlock_queue(source.unlock_native_queue) {
        extension_names.reserve(extensions.size());
        for (const std::string &extension : extensions) {
            extension_names.push_back(extension.c_str());
        }
        timeline.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timeline.timelineSemaphore = source.native_timeline_semaphore
            ? VK_TRUE : VK_FALSE;
    }

    std::vector<std::string> extensions;
    std::vector<const char *> extension_names;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    void *queue_lock_opaque = nullptr;
    void (*lock_queue)(void *) = nullptr;
    void (*unlock_queue)(void *) = nullptr;
};

void free_vulkan_device_owner(AVHWDeviceContext *context) {
    delete static_cast<VulkanDeviceOwner *>(context->user_opaque);
    context->user_opaque = nullptr;
}

void lock_vulkan_queue(AVHWDeviceContext *context, std::uint32_t,
                       std::uint32_t) {
    auto *owner = static_cast<VulkanDeviceOwner *>(context->user_opaque);
    if (owner != nullptr && owner->lock_queue != nullptr) {
        owner->lock_queue(owner->queue_lock_opaque);
    }
}

void unlock_vulkan_queue(AVHWDeviceContext *context, std::uint32_t,
                         std::uint32_t) {
    auto *owner = static_cast<VulkanDeviceOwner *>(context->user_opaque);
    if (owner != nullptr && owner->unlock_queue != nullptr) {
        owner->unlock_queue(owner->queue_lock_opaque);
    }
}

void configure_vulkan_decoder(AVCodecContext &codec, const AVCodec &decoder,
                              const DecoderOptions &options) {
    if (options.native_instance == 0U
        || options.native_physical_device == 0U
        || options.native_device == 0U
        || options.native_queue == 0U
        || options.lock_native_queue == nullptr
        || options.unlock_native_queue == nullptr) {
        throw std::runtime_error(
            "Vulkan Video decode requires a shared device, queue, and lock");
    }
    if (VK_API_VERSION_MAJOR(options.native_instance_api_version) < 1U
        || (VK_API_VERSION_MAJOR(options.native_instance_api_version) == 1U
            && VK_API_VERSION_MINOR(options.native_instance_api_version) < 3U)
        || !options.native_timeline_semaphore) {
        throw std::runtime_error(
            "Vulkan Video decode requires Vulkan 1.3 and timeline semaphores");
    }
    if (!decoder_supports_vulkan(decoder)) {
        throw std::runtime_error(
            "FFmpeg decoder exposes no Vulkan hardware-device configuration");
    }
    const VkVideoCodecOperationFlagBitsKHR operation =
        codec_video_operation(decoder.id);
    if (operation == VK_VIDEO_CODEC_OPERATION_NONE_KHR
        || (options.native_video_codec_operations
            & static_cast<std::uint32_t>(operation)) == 0U) {
        throw std::runtime_error(
            "the shared Vulkan device does not support this video codec");
    }

    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (device_ref == nullptr) {
        throw std::runtime_error("av_hwdevice_ctx_alloc(Vulkan) failed");
    }
    auto *device = reinterpret_cast<AVHWDeviceContext *>(device_ref->data);
    auto *vulkan = static_cast<AVVulkanDeviceContext *>(device->hwctx);
    auto owner = std::make_unique<VulkanDeviceOwner>(options);
    device->user_opaque = owner.get();
    device->free = free_vulkan_device_owner;
    vulkan->get_proc_addr = vkGetInstanceProcAddr;
    vulkan->inst = native_handle<VkInstance>(options.native_instance);
    vulkan->phys_dev = native_handle<VkPhysicalDevice>(
        options.native_physical_device);
    vulkan->act_dev = native_handle<VkDevice>(options.native_device);
    vulkan->device_features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vulkan->device_features.pNext = &owner->timeline;
    vulkan->enabled_dev_extensions = owner->extension_names.data();
    vulkan->nb_enabled_dev_extensions =
        static_cast<int>(owner->extension_names.size());
    vulkan->lock_queue = lock_vulkan_queue;
    vulkan->unlock_queue = unlock_vulkan_queue;

    vulkan->nb_qf = 0;
    vulkan->qf[vulkan->nb_qf++] = AVVulkanDeviceQueueFamily{
        static_cast<int>(options.native_compute_queue_family), 1,
        static_cast<VkQueueFlagBits>(
            VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
        VK_VIDEO_CODEC_OPERATION_NONE_KHR};
    if (options.native_decode_queue_family
        != options.native_compute_queue_family) {
        vulkan->qf[vulkan->nb_qf++] = AVVulkanDeviceQueueFamily{
            static_cast<int>(options.native_decode_queue_family), 1,
            static_cast<VkQueueFlagBits>(
                VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT),
            static_cast<VkVideoCodecOperationFlagBitsKHR>(
                options.native_video_codec_operations)};
    } else {
        vulkan->qf[0].flags = static_cast<VkQueueFlagBits>(
            vulkan->qf[0].flags | VK_QUEUE_VIDEO_DECODE_BIT_KHR);
        vulkan->qf[0].video_caps =
            static_cast<VkVideoCodecOperationFlagBitsKHR>(
                options.native_video_codec_operations);
    }
#if FF_API_VULKAN_FIXED_QUEUES
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    vulkan->queue_family_index = -1;
    vulkan->nb_graphics_queues = 0;
    vulkan->queue_family_tx_index =
        static_cast<int>(options.native_compute_queue_family);
    vulkan->nb_tx_queues = 1;
    vulkan->queue_family_comp_index =
        static_cast<int>(options.native_compute_queue_family);
    vulkan->nb_comp_queues = 1;
    vulkan->queue_family_encode_index = -1;
    vulkan->nb_encode_queues = 0;
    vulkan->queue_family_decode_index =
        static_cast<int>(options.native_decode_queue_family);
    vulkan->nb_decode_queues = 1;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
    (void)owner.release();
    const int init_result = av_hwdevice_ctx_init(device_ref);
    if (init_result < 0) {
        av_buffer_unref(&device_ref);
        check_ffmpeg(init_result, "av_hwdevice_ctx_init(Vulkan)");
    }
    codec.hw_device_ctx = device_ref;
    codec.get_format = select_vulkan_format;
    codec.extra_hw_frames = static_cast<int>(8U + options.frame_concurrency);
}

struct VulkanLumaDescription {
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0U;
    std::int32_t normalized_bits = 0;
};

[[nodiscard]] VulkanLumaDescription vulkan_luma_description(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return {VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 8};
    case VK_FORMAT_R10X6_UNORM_PACK16:
        return {VK_FORMAT_R10X6_UNORM_PACK16, VK_IMAGE_ASPECT_COLOR_BIT, 10};
    case VK_FORMAT_R12X4_UNORM_PACK16:
        return {VK_FORMAT_R12X4_UNORM_PACK16, VK_IMAGE_ASPECT_COLOR_BIT, 12};
    case VK_FORMAT_R16_UNORM:
        return {VK_FORMAT_R16_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 16};
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:
        return {VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT, 8};
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
        return {VK_FORMAT_R10X6_UNORM_PACK16,
                VK_IMAGE_ASPECT_PLANE_0_BIT, 10};
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16:
        return {VK_FORMAT_R12X4_UNORM_PACK16,
                VK_IMAGE_ASPECT_PLANE_0_BIT, 12};
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
    case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
        return {VK_FORMAT_R16_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT, 16};
    default:
        throw std::runtime_error(
            "unsupported Vulkan decoder luma image format: "
            + std::to_string(static_cast<std::uint32_t>(format)));
    }
}

class VulkanFrameLock {
public:
    VulkanFrameLock(AVHWFramesContext &frames, AVVkFrame &frame)
        : frames_(&frames), frame_(&frame),
          vulkan_frames_(static_cast<AVVulkanFramesContext *>(frames.hwctx)) {
        if (vulkan_frames_ == nullptr || vulkan_frames_->lock_frame == nullptr
            || vulkan_frames_->unlock_frame == nullptr) {
            throw std::runtime_error("FFmpeg Vulkan frame lock callbacks are unavailable");
        }
        vulkan_frames_->lock_frame(frames_, frame_);
        locked_ = true;
    }

    ~VulkanFrameLock() { unlock(); }
    VulkanFrameLock(const VulkanFrameLock &) = delete;
    VulkanFrameLock &operator=(const VulkanFrameLock &) = delete;

    static void mark_submitted(void *opaque, std::uint32_t layout,
                               std::uint32_t access,
                               std::uint32_t queue_family,
                               std::uint64_t semaphore_value) {
        auto &self = *static_cast<VulkanFrameLock *>(opaque);
        self.frame_->layout[0] = static_cast<VkImageLayout>(layout);
        self.frame_->access[0] = static_cast<VkAccessFlagBits>(access);
        self.frame_->queue_family[0] = queue_family;
        self.frame_->sem_value[0] = semaphore_value;
        self.unlock();
    }

    static void release_without_submit(void *opaque) {
        static_cast<VulkanFrameLock *>(opaque)->unlock();
    }

private:
    void unlock() noexcept {
        if (!locked_) return;
        vulkan_frames_->unlock_frame(frames_, frame_);
        locked_ = false;
    }

    AVHWFramesContext *frames_ = nullptr;
    AVVkFrame *frame_ = nullptr;
    AVVulkanFramesContext *vulkan_frames_ = nullptr;
    bool locked_ = false;
};

struct VulkanFrameLease {
    explicit VulkanFrameLease(const AVFrame &source)
        : frame(av_frame_clone(&source)) {
        if (!frame) throw std::runtime_error("av_frame_clone(Vulkan) failed");
        if (frame->hw_frames_ctx == nullptr || frame->data[0] == nullptr) {
            throw std::runtime_error("cloned Vulkan frame is incomplete");
        }
        auto *frames = reinterpret_cast<AVHWFramesContext *>(
            frame->hw_frames_ctx->data);
        auto *vulkan_frame = reinterpret_cast<AVVkFrame *>(frame->data[0]);
        lock = std::make_unique<VulkanFrameLock>(*frames, *vulkan_frame);
    }

    FramePtr frame;
    std::unique_ptr<VulkanFrameLock> lock;
};
#endif

[[nodiscard]] OpenedDecoder open_decoder(const std::string &path,
                                          std::uint32_t stream_index,
                                          const DecoderOptions *options = nullptr) {
    AVFormatContext *raw_format = nullptr;
    check_ffmpeg(avformat_open_input(&raw_format, path.c_str(), nullptr, nullptr),
                 "avformat_open_input");
    FormatPtr format{raw_format};
    check_ffmpeg(avformat_find_stream_info(format.get(), nullptr),
                 "avformat_find_stream_info");
    if (stream_index >= format->nb_streams
        || format->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        throw std::runtime_error("media stream index is not a video stream");
    }
    const AVStream *stream = format->streams[stream_index];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) throw std::runtime_error("no FFmpeg decoder for video codec");
    CodecPtr codec{avcodec_alloc_context3(decoder)};
    if (!codec) throw std::runtime_error("avcodec_alloc_context3 failed");
    check_ffmpeg(avcodec_parameters_to_context(codec.get(), stream->codecpar),
                 "avcodec_parameters_to_context");
    if (options != nullptr && options->backend != DecoderOptions::Backend::software) {
        if (options->backend == DecoderOptions::Backend::cuda) {
#if defined(GETNATIVE_HAS_CUDA)
            configure_cuda_decoder(*codec, *decoder, *options);
#else
            throw std::runtime_error("CUDA hardware decode was not compiled");
#endif
        } else {
#if defined(GETNATIVE_HAS_VULKAN)
            configure_vulkan_decoder(*codec, *decoder, *options);
#else
            throw std::runtime_error("Vulkan Video decode was not compiled");
#endif
        }
    }
    check_ffmpeg(avcodec_open2(codec.get(), decoder, nullptr), "avcodec_open2");
    return {std::move(format), std::move(codec), stream, decoder};
}

[[nodiscard]] std::optional<std::int64_t> timestamp_value(std::int64_t value) {
    return value == AV_NOPTS_VALUE ? std::nullopt : std::optional{value};
}

template <class Callback>
void receive_frames(AVCodecContext &codec, std::stop_token stop,
                    Callback &&callback) {
    FramePtr frame{av_frame_alloc()};
    if (!frame) throw std::runtime_error("av_frame_alloc failed");
    for (;;) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        const int result = avcodec_receive_frame(&codec, frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
        check_ffmpeg(result, "avcodec_receive_frame");
        callback(*frame);
        av_frame_unref(frame.get());
    }
}

[[maybe_unused, nodiscard]] std::string frame_range(const AVFrame &frame) {
    switch (frame.color_range) {
    case AVCOL_RANGE_MPEG: return "limited";
    case AVCOL_RANGE_JPEG: return "full";
    default: return "unknown";
    }
}

[[nodiscard]] bool selected(const FrameIdentity &frame, const ScanScope &scope) {
    const std::uint64_t start = scope.start_frame.value_or(0U);
    const std::uint64_t end = scope.end_frame.value_or(std::numeric_limits<std::uint64_t>::max());
    if (start > end || frame.frame_index < start || frame.frame_index > end) return false;
    switch (scope.selection) {
    case ScanSelection::all: return true;
    case ScanSelection::decoded_i_picture: return frame.key_frame;
    case ScanSelection::every_n:
        return scope.every_n > 0U && ((frame.frame_index - start) % scope.every_n == 0U);
    }
    return false;
}

} // namespace

bool compiled() noexcept { return true; }

std::string runtime_version() {
    const auto major = [](unsigned version) { return version >> 16U; };
    return std::to_string(major(avformat_version())) + "."
        + std::to_string(major(avcodec_version())) + "."
        + std::to_string(major(avutil_version())) + "."
        + std::to_string(major(swscale_version()));
}

bool backend_compiled(DecoderOptions::Backend backend) noexcept {
    switch (backend) {
    case DecoderOptions::Backend::software: return true;
    case DecoderOptions::Backend::cuda:
#if defined(GETNATIVE_HAS_CUDA)
        return true;
#else
        return false;
#endif
    case DecoderOptions::Backend::vulkan_video:
#if defined(GETNATIVE_HAS_VULKAN)
        return true;
#else
        return false;
#endif
    }
    return false;
}

bool backend_runtime_available(DecoderOptions::Backend backend) noexcept {
    // The media layer does not create a second accelerator device. Hardware
    // decode is enabled by the worker only after the analysis owner supplies
    // a native context on the selected device.
    if (backend == DecoderOptions::Backend::software) return true;
#if defined(GETNATIVE_HAS_CUDA)
    if (backend == DecoderOptions::Backend::cuda) {
        return av_hwdevice_find_type_by_name("cuda") == AV_HWDEVICE_TYPE_CUDA;
    }
#endif
    if (backend == DecoderOptions::Backend::vulkan_video) {
#if defined(GETNATIVE_HAS_VULKAN)
        return av_hwdevice_find_type_by_name("vulkan")
            == AV_HWDEVICE_TYPE_VULKAN;
#else
        return false;
#endif
    }
    return false;
}

std::vector<std::string> hardware_codecs(DecoderOptions::Backend backend) {
    if (backend == DecoderOptions::Backend::cuda) {
#if defined(GETNATIVE_HAS_CUDA)
        std::set<std::string> names;
        void *state = nullptr;
        while (const AVCodec *codec = av_codec_iterate(&state)) {
            if (!av_codec_is_decoder(codec) || !decoder_supports_cuda(*codec)) continue;
            names.emplace(codec->name);
        }
        return {names.begin(), names.end()};
#else
        return {};
#endif
    }
    if (backend == DecoderOptions::Backend::vulkan_video) {
#if defined(GETNATIVE_HAS_VULKAN)
        std::set<std::string> names;
        void *state = nullptr;
        while (const AVCodec *codec = av_codec_iterate(&state)) {
            if (!av_codec_is_decoder(codec)
                || !decoder_supports_vulkan(*codec)) continue;
            names.emplace(codec->name);
        }
        return {names.begin(), names.end()};
#else
        return {};
#endif
    }
    return {};
}

std::string quick_fingerprint(const std::string &path) {
    const std::uint64_t size = std::filesystem::file_size(path);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("media fingerprint: failed to open file");
    AVHashContext *hash_raw = nullptr;
    check_ffmpeg(av_hash_alloc(&hash_raw, "sha256"), "av_hash_alloc(sha256)");
    HashPtr hash{hash_raw};
    av_hash_init(hash.get());
    std::array<std::uint8_t, sizeof(size)> size_bytes{};
    for (std::size_t i = 0; i < size_bytes.size(); ++i) {
        size_bytes[i] = static_cast<std::uint8_t>(size >> (i * 8U));
    }
    av_hash_update(hash.get(), size_bytes.data(), size_bytes.size());
    constexpr std::size_t edge = 64U * 1024U;
    std::array<std::uint8_t, edge> buffer{};
    file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    av_hash_update(hash.get(), buffer.data(), static_cast<std::size_t>(file.gcount()));
    if (size > edge) {
        file.clear();
        file.seekg(-static_cast<std::streamoff>(edge), std::ios::end);
        file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        av_hash_update(hash.get(), buffer.data(), static_cast<std::size_t>(file.gcount()));
    }
    std::array<std::uint8_t, 2U * AV_HASH_MAX_SIZE + 1U> digest{};
    av_hash_final_hex(hash.get(), digest.data(), static_cast<int>(digest.size()));
    return "quick-sha256-v1:"
        + std::string{reinterpret_cast<const char *>(digest.data())};
}

std::uint32_t default_video_stream(const std::string &path) {
    AVFormatContext *raw_format = nullptr;
    check_ffmpeg(avformat_open_input(&raw_format, path.c_str(), nullptr, nullptr),
                 "avformat_open_input");
    FormatPtr format{raw_format};
    check_ffmpeg(avformat_find_stream_info(format.get(), nullptr),
                 "avformat_find_stream_info");
    std::optional<std::uint32_t> attached;
    for (std::uint32_t index = 0U; index < format->nb_streams; ++index) {
        AVStream *stream = format->streams[index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
            if (!attached) attached = index;
            continue;
        }
        return index;
    }
    if (attached) return *attached;
    throw std::runtime_error("media contains no video stream");
}

[[nodiscard]] std::optional<std::string> parser_picture_name(int value) {
    if (value == AV_PICTURE_TYPE_NONE) return std::nullopt;
    const char type = av_get_picture_type_char(static_cast<AVPictureType>(value));
    if (type == 'I') return "I";
    if (type == 'P') return "P";
    if (type == 'B') return "B";
    return "other";
}

[[nodiscard]] std::string parser_field_order(enum AVFieldOrder order) {
    switch (order) {
    case AV_FIELD_TT: return "tt";
    case AV_FIELD_BB: return "bb";
    case AV_FIELD_TB: return "tb";
    case AV_FIELD_BT: return "bt";
    case AV_FIELD_PROGRESSIVE: return "progressive";
    default: return "unknown";
    }
}

[[nodiscard]] bool monotonic_positions(const std::vector<FrameIdentity> &frames,
                                       bool dts) {
    std::optional<std::int64_t> previous;
    for (const FrameIdentity &frame : frames) {
        const auto value = dts ? frame.dts : frame.file_position;
        if (!value) continue;
        if (previous && *value <= *previous) return false;
        previous = *value;
    }
    return previous.has_value();
}

[[nodiscard]] MediaIndex index_media_impl(
    const std::string &path, std::uint32_t stream_index,
    const DecoderOptions *, const IndexOptions &index_options,
    std::stop_token stop, IndexProgress progress) {
    (void)index_options;
    const auto index_start = Clock::now();
    MediaIndex result;
    result.fingerprint = quick_fingerprint(path);
    result.source_path = path;
    result.stream_index = stream_index;

    AVFormatContext *format_raw = nullptr;
    check_ffmpeg(avformat_open_input(&format_raw, path.c_str(), nullptr, nullptr),
                 "avformat_open_input");
    FormatPtr format{format_raw};
    check_ffmpeg(avformat_find_stream_info(format.get(), nullptr),
                 "avformat_find_stream_info");
    if (stream_index >= format->nb_streams
        || format->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        throw std::runtime_error("requested stream is not a video stream");
    }
    AVStream &stream = *format->streams[stream_index];
    const AVCodecParameters &parameters = *stream.codecpar;
    result.duration_ticks = stream.duration;
    result.time_base_num = stream.time_base.num;
    result.time_base_den = stream.time_base.den;
    result.source_size = std::filesystem::file_size(path);
    result.source_mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::filesystem::last_write_time(path).time_since_epoch()).count();
    result.format_name = format->iformat != nullptr && format->iformat->name != nullptr
        ? format->iformat->name : "";
    result.format_flags = format->flags;
    result.raw_demuxer = result.format_name == "h264" || result.format_name == "hevc"
        || result.format_name == "mpegvideo" || result.format_name == "vc1";
    result.width = parameters.width;
    result.height = parameters.height;
    result.codec = avcodec_get_name(parameters.codec_id);
    if (parameters.profile != AV_PROFILE_UNKNOWN) {
        const char *profile = avcodec_profile_name(parameters.codec_id, parameters.profile);
        if (profile != nullptr) result.profile = profile;
    }
    if (parameters.format >= 0) {
        const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters.format));
        if (name != nullptr) result.pixel_format = name;
    }
    if (parameters.bits_per_raw_sample > 0) {
        result.bit_depth = parameters.bits_per_raw_sample;
    } else if (parameters.format >= 0) {
        const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(
            static_cast<AVPixelFormat>(parameters.format));
        result.bit_depth = descriptor != nullptr && descriptor->comp[0].depth > 0
            ? descriptor->comp[0].depth : 8;
    } else {
        result.bit_depth = 8;
    }
    switch (parameters.color_range) {
    case AVCOL_RANGE_MPEG: result.range = "limited"; break;
    case AVCOL_RANGE_JPEG: result.range = "full"; break;
    default: break;
    }
    if (parameters.extradata != nullptr && parameters.extradata_size > 0) {
        ExtraDataInfo extra;
        extra.codec_id = static_cast<std::uint32_t>(parameters.codec_id);
        extra.width = parameters.width;
        extra.height = parameters.height;
        extra.pixel_format = result.pixel_format;
        extra.bit_rate = static_cast<std::uint32_t>(std::min<std::int64_t>(
            parameters.bit_rate, std::numeric_limits<std::uint32_t>::max()));
        extra.data.assign(parameters.extradata,
                          parameters.extradata + parameters.extradata_size);
        result.extradata.push_back(std::move(extra));
    }
    const int index_entry_count = avformat_index_get_entries_count(&stream);
    for (int index = 0; index < index_entry_count; ++index) {
        const AVIndexEntry *entry_ptr = avformat_index_get_entry(&stream, index);
        if (entry_ptr == nullptr) continue;
        const AVIndexEntry &entry = *entry_ptr;
        result.stream_index_entries.push_back({entry.pos, entry.timestamp,
                                                static_cast<std::uint32_t>(entry.flags),
                                                static_cast<std::uint32_t>(entry.size),
                                                static_cast<std::uint32_t>(entry.min_distance)});
    }

    AVCodecParserContext *parser_raw = av_parser_init(parameters.codec_id);
    ParserPtr parser{parser_raw};
    CodecPtr parser_codec{avcodec_alloc_context3(nullptr)};
    if (parser_codec) check_ffmpeg(avcodec_parameters_to_context(parser_codec.get(), &parameters),
                                   "avcodec_parameters_to_context(parser)");
    if (parser) parser->flags |= PARSER_FLAG_COMPLETE_FRAMES | PARSER_FLAG_FETCHED_OFFSET;

    BsfPtr bitstream_filter;
    const char *bitstream_filter_name = nullptr;
    if (parameters.codec_id == AV_CODEC_ID_H264 && !result.raw_demuxer
        && parameters.extradata_size > 0) {
        bitstream_filter_name = "h264_mp4toannexb";
    } else if (parameters.codec_id == AV_CODEC_ID_MPEG4
               && !result.raw_demuxer) {
        bitstream_filter_name = "mpeg4_unpack_bframes";
    }
    if (bitstream_filter_name != nullptr) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name(bitstream_filter_name);
        if (filter != nullptr) {
            AVBSFContext *filter_raw = nullptr;
            check_ffmpeg(av_bsf_alloc(filter, &filter_raw), "av_bsf_alloc");
            bitstream_filter.reset(filter_raw);
            check_ffmpeg(avcodec_parameters_copy(bitstream_filter->par_in, &parameters),
                         "avcodec_parameters_copy(bitstream filter)");
            bitstream_filter->time_base_in = stream.time_base;
            check_ffmpeg(av_bsf_init(bitstream_filter.get()), "av_bsf_init");
            if (parser_codec && bitstream_filter->par_out != nullptr) {
                check_ffmpeg(avcodec_parameters_to_context(parser_codec.get(),
                                                            bitstream_filter->par_out),
                             "avcodec_parameters_to_context(bitstream filter output)");
            }
        }
    }

    AVPacket *packet_raw = av_packet_alloc();
    if (!packet_raw) throw std::runtime_error("av_packet_alloc failed");
    PacketPtr packet{packet_raw};
    std::uint32_t current_extradata_index = result.extradata.empty()
        ? 0U : result.extradata.front().index;
    auto append_record = [&](const AVPacket *source, const std::uint8_t *data,
                             int size, bool flushed) {
        FrameIdentity identity;
        identity.decode_index = result.frames.size();
        const std::int64_t packet_pts = source != nullptr ? source->pts : AV_NOPTS_VALUE;
        const std::int64_t packet_dts = source != nullptr ? source->dts : AV_NOPTS_VALUE;
        const std::int64_t packet_pos = source != nullptr ? source->pos : -1;
        bool parser_key_known = false;
        if (parser) {
            identity.pts = timestamp_value(parser->pts);
            identity.dts = timestamp_value(parser->dts);
            identity.file_position = parser->pos >= 0 ? std::optional{parser->pos}
                                                       : (packet_pos >= 0 ? std::optional{packet_pos} : std::nullopt);
            identity.picture_type = parser_picture_name(parser->pict_type);
            identity.poc = parser->output_picture_number >= 0
                ? std::optional{parser->output_picture_number} : std::nullopt;
            identity.repeat_pict = parser->repeat_pict;
            identity.field_order = parser_field_order(parser->field_order);
            parser_key_known = parser->key_frame >= 0;
            identity.key_frame = parser->key_frame == 1
                || (parser->key_frame < 0 && identity.picture_type == std::optional<std::string>{"I"});
        }
        if (!identity.pts) identity.pts = timestamp_value(packet_pts);
        if (!identity.dts) identity.dts = timestamp_value(packet_dts);
        if (!identity.file_position && packet_pos >= 0) identity.file_position = packet_pos;
        identity.packet_size = static_cast<std::uint32_t>(std::max(0, size));
        identity.extradata_index = current_extradata_index;
        if (!parser_key_known && source != nullptr && (source->flags & AV_PKT_FLAG_KEY) != 0) {
            identity.key_frame = true;
        }
        identity.rap = identity.key_frame || identity.picture_type == std::optional<std::string>{"I"};
        const std::uint8_t *packet_data = source != nullptr && source->data != nullptr
            ? source->data : data;
        const int packet_data_size = source != nullptr ? source->size : size;
        if (parameters.codec_id == AV_CODEC_ID_VP9 && packet_data != nullptr && packet_data_size > 0) {
            const std::uint8_t marker = packet_data[packet_data_size - 1];
            identity.vp9_superframe = (marker & 0xe0U) == 0xc0U;
        }
        if (parameters.codec_id == AV_CODEC_ID_VP8 && packet_data != nullptr && packet_data_size > 0) {
            identity.vp8_invisible_frame = (packet_data[0] & 1U) == 0U;
        }
        if (flushed && identity.packet_size == 0U) identity.packet_size = 0U;
        result.frames.push_back(std::move(identity));
        if (progress) progress(result.frames.size());
    };

    auto parse_packet = [&](AVPacket *input_packet) {
        std::size_t side_data_size = 0U;
        const std::uint8_t *side_data = av_packet_get_side_data(
            input_packet, AV_PKT_DATA_NEW_EXTRADATA, &side_data_size);
        if (side_data != nullptr && side_data_size > 0U) {
            const bool changed = result.extradata.empty()
                || result.extradata.back().data.size() != side_data_size
                || !std::equal(result.extradata.back().data.begin(), result.extradata.back().data.end(), side_data);
            if (changed) {
                ExtraDataInfo extra;
                extra.index = static_cast<std::uint32_t>(result.extradata.size());
                extra.codec_id = static_cast<std::uint32_t>(parameters.codec_id);
                extra.width = parameters.width;
                extra.height = parameters.height;
                extra.pixel_format = result.pixel_format;
                extra.data.assign(side_data, side_data + side_data_size);
                result.extradata.push_back(std::move(extra));
                current_extradata_index = result.extradata.back().index;
            }
        }
        bool emitted = false;
        if (parser && parser_codec) {
            const std::uint8_t *input = input_packet->data;
            int remaining = input_packet->size;
            while (remaining > 0) {
                std::uint8_t *output = nullptr;
                int output_size = 0;
                const int consumed = av_parser_parse2(
                    parser.get(), parser_codec.get(), &output, &output_size,
                    input, remaining, input_packet->pts, input_packet->dts, input_packet->pos);
                if (consumed < 0) throw std::runtime_error("media parser failed");
                if (output_size > 0) {
                    append_record(input_packet, output, output_size, false);
                    emitted = true;
                }
                if (consumed == 0) break;
                input += consumed;
                remaining -= consumed;
            }
        }
        if (!parser && input_packet->size > 0) {
            append_record(input_packet, input_packet->data, input_packet->size, false);
            emitted = true;
        }
        return emitted;
    };
    PacketPtr filtered_packet{av_packet_alloc()};
    if (!filtered_packet) throw std::runtime_error("av_packet_alloc failed");
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        if (packet->stream_index == static_cast<int>(stream_index)) {
            ++result.packet_count;
            if (bitstream_filter) {
                check_ffmpeg(av_bsf_send_packet(bitstream_filter.get(), packet.get()),
                             "av_bsf_send_packet");
                for (;;) {
                    const int filtered = av_bsf_receive_packet(bitstream_filter.get(), filtered_packet.get());
                    if (filtered == AVERROR(EAGAIN) || filtered == AVERROR_EOF) break;
                    check_ffmpeg(filtered, "av_bsf_receive_packet");
                    parse_packet(filtered_packet.get());
                    av_packet_unref(filtered_packet.get());
                }
            } else {
                parse_packet(packet.get());
            }
        }
        av_packet_unref(packet.get());
    }
    if (bitstream_filter) {
        check_ffmpeg(av_bsf_send_packet(bitstream_filter.get(), nullptr), "av_bsf_send_packet(flush)");
        for (;;) {
            const int filtered = av_bsf_receive_packet(bitstream_filter.get(), filtered_packet.get());
            if (filtered == AVERROR(EAGAIN) || filtered == AVERROR_EOF) break;
            check_ffmpeg(filtered, "av_bsf_receive_packet");
            parse_packet(filtered_packet.get());
            av_packet_unref(filtered_packet.get());
        }
    }
    if (parser && parser_codec) {
        for (;;) {
            std::uint8_t *output = nullptr;
            int output_size = 0;
            av_parser_parse2(parser.get(), parser_codec.get(), &output, &output_size,
                             nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
            if (output_size <= 0) break;
            append_record(nullptr, output, output_size, true);
        }
    }
    if (result.frames.empty()) throw std::runtime_error("media contains no indexed video frames");

    bool unique_pts = true;
    std::vector<std::int64_t> seen_pts;
    for (const FrameIdentity &frame : result.frames) {
        if (!frame.pts || std::find(seen_pts.begin(), seen_pts.end(), *frame.pts) != seen_pts.end()) {
            unique_pts = false;
            break;
        }
        seen_pts.push_back(*frame.pts);
    }
    std::vector<std::size_t> order(result.frames.size());
    std::iota(order.begin(), order.end(), 0U);
    if (unique_pts) {
        std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
            return result.frames[left].pts.value() < result.frames[right].pts.value();
        });
    }
    std::vector<FrameIdentity> presentation;
    presentation.reserve(order.size());
    result.decode_to_presentation.resize(order.size());
    result.presentation_to_decode.resize(order.size());
    for (std::size_t presentation_index = 0; presentation_index < order.size(); ++presentation_index) {
        FrameIdentity frame = std::move(result.frames[order[presentation_index]]);
        result.decode_to_presentation[frame.decode_index] = presentation_index;
        result.presentation_to_decode[presentation_index] = frame.decode_index;
        frame.frame_index = presentation_index;
        if (frame.pts && result.time_base_den != 0) {
            frame.timestamp_seconds = static_cast<double>(*frame.pts)
                * static_cast<double>(result.time_base_num) / result.time_base_den;
        }
        presentation.push_back(std::move(frame));
    }
    result.frames = std::move(presentation);
    for (FrameIdentity &frame : result.frames) {
        std::size_t anchor = 0U;
        std::uint64_t best_decode = 0U;
        bool found = false;
        for (const FrameIdentity &candidate : result.frames) {
            if (!candidate.rap || candidate.decode_index > frame.decode_index) continue;
            if (!found || candidate.decode_index >= best_decode) {
                found = true;
                best_decode = candidate.decode_index;
                anchor = candidate.frame_index;
            }
        }
        frame.keyframe_anchor = anchor;
        frame.leading_frame = frame.frame_index < anchor;
        frame.keyframe_timestamp = result.frames[anchor].pts;
    }
    if (unique_pts) result.seek_method = "pts";
    else if (monotonic_positions(result.frames, true)) result.seek_method = "dts";
    else if (monotonic_positions(result.frames, false)) result.seek_method = "file_position";
    else result.seek_method = "sample_order";
    result.index_mode = "packet_rebuilt";
    result.decoder = "software";
    result.index_ms = std::chrono::duration<double, std::milli>(Clock::now() - index_start).count();
    return result;
}

MediaIndex index_media(const std::string &path, std::uint32_t stream_index,
                       std::stop_token stop, IndexProgress progress) {
    return index_media_impl(
        path, stream_index, nullptr, {}, stop, std::move(progress));
}

MediaIndex index_media(const std::string &path, std::uint32_t stream_index,
                       const DecoderOptions &options, std::stop_token stop,
                       IndexProgress progress) {
    return index_media_impl(
        path, stream_index, &options, {}, stop, std::move(progress));
}

MediaIndex index_media(const std::string &path, std::uint32_t stream_index,
                       const IndexOptions &index_options, std::stop_token stop,
                       IndexProgress progress) {
    return index_media_impl(path, stream_index, nullptr, index_options,
                            stop, std::move(progress));
}

MediaIndex index_media(const std::string &path, std::uint32_t stream_index,
                       const DecoderOptions &options, const IndexOptions &index_options,
                       std::stop_token stop, IndexProgress progress) {
    return index_media_impl(path, stream_index, &options, index_options,
                            stop, std::move(progress));
}

std::vector<FrameIdentity> select_frames(const MediaIndex &index, const ScanScope &scope) {
    if (scope.selection == ScanSelection::every_n && scope.every_n == 0U) {
        throw std::invalid_argument("every_n selection requires every_n >= 1");
    }
    const std::uint64_t start = scope.start_frame.value_or(0U);
    const std::uint64_t end = scope.end_frame.value_or(
        index.frames.empty() ? 0U : index.frames.back().frame_index);
    if (start > end) throw std::invalid_argument("scan range start must be <= end");
    std::vector<FrameIdentity> result;
    for (const FrameIdentity &frame : index.frames) {
        if (selected(frame, scope)) result.push_back(frame);
    }
    if (result.empty()) throw std::invalid_argument("scan scope selects zero frames");
    return result;
}

void decode_selected(const std::string &path, std::uint32_t stream_index,
                     std::span<const FrameIdentity> selected_frames,
                     const DecoderOptions &options, std::stop_token stop,
                     const FrameConsumer &consumer, DecodeTelemetry *telemetry) {
    if (options.backend != DecoderOptions::Backend::software) {
        throw std::runtime_error("requested hardware decoder is unavailable for this device");
    }
    const auto start = Clock::now();
    OpenedDecoder opened = open_decoder(path, stream_index);
    FramePtr frame{av_frame_alloc()};
    PacketPtr packet{av_packet_alloc()};
    if (!frame || !packet) throw std::runtime_error("FFmpeg frame allocation failed");
    SwsPtr scaler;
    std::uint64_t frame_index = 0U;
    std::size_t selected_index = 0U;
    double consumer_ms = 0.0;
    auto process = [&](AVFrame &source) {
        if (selected_index >= selected_frames.size()) return;
        if (frame_index != selected_frames[selected_index].frame_index) {
            ++frame_index;
            return;
        }
        if (!scaler) {
            scaler.reset(sws_getContext(source.width, source.height,
                static_cast<AVPixelFormat>(source.format), source.width, source.height,
                AV_PIX_FMT_GRAYF32LE, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!scaler) throw std::runtime_error("sws_getContext(grayf32le) failed");
            const int src_range = source.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
            const int *coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
            (void)sws_setColorspaceDetails(scaler.get(), coefficients, src_range,
                                            coefficients, 1, 0, 1 << 16, 1 << 16);
        }
        HostFrame output;
        output.seq = selected_index;
        output.identity = selected_frames[selected_index];
        output.width = source.width;
        output.height = source.height;
        output.pixels.resize(static_cast<std::size_t>(source.width)
                             * static_cast<std::size_t>(source.height));
        std::uint8_t *planes[4] = {
            reinterpret_cast<std::uint8_t *>(output.pixels.data()), nullptr, nullptr, nullptr};
        int lines[4] = {source.width * static_cast<int>(sizeof(float)), 0, 0, 0};
        const auto convert_start = Clock::now();
        check_ffmpeg(sws_scale(scaler.get(), source.data, source.linesize, 0, source.height,
                               planes, lines), "sws_scale(grayf32le)");
        if (telemetry) {
            telemetry->host_frame_bytes += output.pixels.size() * sizeof(float);
            telemetry->conversion_bytes += output.pixels.size() * sizeof(float);
            telemetry->convert_ms += std::chrono::duration<double, std::milli>(
                Clock::now() - convert_start).count();
            telemetry->selected_frames += 1U;
        }
        const auto consumer_start = Clock::now();
        consumer(std::move(output));
        consumer_ms += std::chrono::duration<double, std::milli>(
            Clock::now() - consumer_start).count();
        ++selected_index;
        ++frame_index;
    };
    while (selected_index < selected_frames.size()
           && av_read_frame(opened.format.get(), packet.get()) >= 0) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        if (packet->stream_index == static_cast<int>(stream_index)) {
            check_ffmpeg(avcodec_send_packet(opened.codec.get(), packet.get()),
                         "avcodec_send_packet");
            receive_frames(*opened.codec, stop, process);
        }
        av_packet_unref(packet.get());
    }
    check_ffmpeg(avcodec_send_packet(opened.codec.get(), nullptr), "avcodec_send_packet(flush)");
    receive_frames(*opened.codec, stop, process);
    if (selected_index != selected_frames.size()) {
        throw std::runtime_error("decoder ended before all selected frames were produced");
    }
    if (telemetry) {
        telemetry->decoded_frames = frame_index;
        telemetry->decode_ms = std::max(0.0,
            std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                - telemetry->convert_ms - consumer_ms);
    }
}

void decode_selected_indexed(const std::string &path, const MediaIndex &index,
                             std::span<const FrameIdentity> selected_frames,
                             const DecoderOptions &options, std::stop_token stop,
                             const FrameConsumer &consumer,
                             DecodeTelemetry *telemetry) {
    if (options.backend != DecoderOptions::Backend::software) {
        throw std::runtime_error(
            "indexed host-frame decode currently requires the software decoder");
    }
    if (selected_frames.empty()) return;
    for (std::size_t ordinal = 0U; ordinal < selected_frames.size(); ++ordinal) {
        const FrameIdentity &identity = selected_frames[ordinal];
        if (identity.frame_index >= index.frames.size()
            || index.frames[identity.frame_index].frame_index != identity.frame_index
            || (ordinal != 0U
                && identity.frame_index <= selected_frames[ordinal - 1U].frame_index)) {
            throw std::invalid_argument(
                "selected frame identities are not an ordered subset of the media index");
        }
    }

    const auto start = Clock::now();
    std::size_t selected_begin = 0U;
    double consumer_ms = 0.0;
    while (selected_begin < selected_frames.size()) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        const std::uint64_t anchor = selected_frames[selected_begin].keyframe_anchor;
        std::size_t selected_end = selected_begin + 1U;
        while (selected_end < selected_frames.size()
               && selected_frames[selected_end].keyframe_anchor == anchor) {
            ++selected_end;
        }
        const std::uint64_t last_target = selected_frames[selected_end - 1U].frame_index;
        OpenedDecoder opened = open_decoder(path, index.stream_index);
        std::uint64_t frame_cursor = 0U;
        const bool leading_target = selected_frames[selected_begin].leading_frame
            || selected_frames[selected_begin].frame_index < anchor;
        if (!leading_target && anchor < index.frames.size()
            && index.frames[anchor].keyframe_timestamp) {
            const std::int64_t timestamp = *index.frames[anchor].keyframe_timestamp;
            check_ffmpeg(avformat_seek_file(opened.format.get(),
                                           static_cast<int>(index.stream_index),
                                           std::numeric_limits<std::int64_t>::min(),
                                           timestamp, timestamp,
                                           AVSEEK_FLAG_BACKWARD),
                         "avformat_seek_file");
            avcodec_flush_buffers(opened.codec.get());
            frame_cursor = anchor;
        }

        PacketPtr packet{av_packet_alloc()};
        if (!packet) throw std::runtime_error("av_packet_alloc failed");
        SwsPtr scaler;
        SwsPtr rgb_scaler;
        std::size_t selected_cursor = selected_begin;
        bool finished = false;
        auto process = [&](AVFrame &source) {
            if (finished) return;
            if (telemetry) ++telemetry->decoded_frames;
            if (frame_cursor > last_target) {
                finished = true;
                return;
            }
            std::size_t matching_target = selected_end;
            for (std::size_t target = selected_begin; target < selected_end; ++target) {
                if (selected_frames[target].frame_index == frame_cursor) {
                    matching_target = target;
                    break;
                }
            }
            if (matching_target < selected_end) {
                if (!scaler) {
                    scaler.reset(sws_getContext(
                        source.width, source.height,
                        static_cast<AVPixelFormat>(source.format),
                        source.width, source.height, AV_PIX_FMT_GRAYF32LE,
                        SWS_BILINEAR, nullptr, nullptr, nullptr));
                    if (!scaler) {
                        throw std::runtime_error("sws_getContext(grayf32le) failed");
                    }
                    const int src_range = source.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
                    const int *coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
                    (void)sws_setColorspaceDetails(
                        scaler.get(), coefficients, src_range, coefficients, 1,
                        0, 1 << 16, 1 << 16);
                }
                HostFrame output;
                output.seq = selected_cursor++;
                output.identity = selected_frames[matching_target];
                output.width = source.width;
                output.height = source.height;
                output.pixels.resize(static_cast<std::size_t>(source.width)
                                     * static_cast<std::size_t>(source.height));
                std::uint8_t *planes[4] = {
                    reinterpret_cast<std::uint8_t *>(output.pixels.data()),
                    nullptr, nullptr, nullptr};
                int lines[4] = {
                    source.width * static_cast<int>(sizeof(float)), 0, 0, 0};
                const auto convert_start = Clock::now();
                check_ffmpeg(sws_scale(scaler.get(), source.data, source.linesize,
                                       0, source.height, planes, lines),
                             "sws_scale(grayf32le)");
                if (options.output_rgb) {
                    if (!rgb_scaler) {
                        rgb_scaler.reset(sws_getContext(
                            source.width, source.height,
                            static_cast<AVPixelFormat>(source.format),
                            source.width, source.height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr));
                        if (!rgb_scaler) {
                            throw std::runtime_error("sws_getContext(rgb24) failed");
                        }
                    }
                    output.rgb24.resize(
                        static_cast<std::size_t>(source.width)
                        * static_cast<std::size_t>(source.height) * 3U);
                    std::uint8_t *rgb_planes[4] = {
                        output.rgb24.data(), nullptr, nullptr, nullptr};
                    int rgb_lines[4] = {source.width * 3, 0, 0, 0};
                    check_ffmpeg(sws_scale(
                        rgb_scaler.get(), source.data, source.linesize, 0,
                        source.height, rgb_planes, rgb_lines),
                        "sws_scale(rgb24)");
                }
                if (telemetry) {
                    const std::uint64_t bytes = output.pixels.size() * sizeof(float)
                        + output.rgb24.size();
                    telemetry->host_frame_bytes += bytes;
                    telemetry->conversion_bytes += bytes;
                    telemetry->convert_ms += std::chrono::duration<double, std::milli>(
                        Clock::now() - convert_start).count();
                    ++telemetry->selected_frames;
                }
                const auto consumer_start = Clock::now();
                consumer(std::move(output));
                consumer_ms += std::chrono::duration<double, std::milli>(
                    Clock::now() - consumer_start).count();
            }
            ++frame_cursor;
            finished = selected_cursor == selected_end || frame_cursor > last_target;
        };
        while (!finished && av_read_frame(opened.format.get(), packet.get()) >= 0) {
            if (stop.stop_requested()) throw std::runtime_error("cancelled");
            if (packet->stream_index == static_cast<int>(index.stream_index)) {
                const int sent = avcodec_send_packet(opened.codec.get(), packet.get());
                if (sent != AVERROR(EAGAIN)) check_ffmpeg(sent, "avcodec_send_packet");
                receive_frames(*opened.codec, stop, process);
            }
            av_packet_unref(packet.get());
        }
        if (!finished) {
            check_ffmpeg(avcodec_send_packet(opened.codec.get(), nullptr),
                         "avcodec_send_packet(flush)");
            receive_frames(*opened.codec, stop, process);
        }
        if (selected_cursor != selected_end) {
            throw std::runtime_error(
                "decoder ended before all indexed GOP frames were produced");
        }
        selected_begin = selected_end;
    }
    if (telemetry) {
        telemetry->decode_ms = std::max(
            0.0, std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                - telemetry->convert_ms - consumer_ms);
    }
}

PreviewImage encode_preview_png(const HostFrame &source,
                                std::int32_t maximum_dimension) {
    if (source.width <= 0 || source.height <= 0
        || source.pixels.size() != static_cast<std::size_t>(source.width)
                                  * static_cast<std::size_t>(source.height)) {
        throw std::invalid_argument("preview source frame geometry is invalid");
    }
    if (maximum_dimension < 16 || maximum_dimension > 8192) {
        throw std::invalid_argument("preview maximum dimension must be within 16..8192");
    }
    const double scale = std::min(
        1.0, static_cast<double>(maximum_dimension)
                 / static_cast<double>(std::max(source.width, source.height)));
    const std::int32_t width = std::max(
        1, static_cast<std::int32_t>(std::llround(source.width * scale)));
    const std::int32_t height = std::max(
        1, static_cast<std::int32_t>(std::llround(source.height * scale)));

    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (encoder == nullptr) throw std::runtime_error("FFmpeg PNG encoder is unavailable");
    CodecPtr codec{avcodec_alloc_context3(encoder)};
    FramePtr frame{av_frame_alloc()};
    PacketPtr packet{av_packet_alloc()};
    if (!codec || !frame || !packet) throw std::runtime_error("PNG encoder allocation failed");
    codec->width = width;
    codec->height = height;
    const bool color = source.rgb24.size()
        == static_cast<std::size_t>(source.width)
            * static_cast<std::size_t>(source.height) * 3U;
    codec->pix_fmt = color ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAY8;
    codec->time_base = AVRational{1, 1};
    check_ffmpeg(avcodec_open2(codec.get(), encoder, nullptr), "avcodec_open2(png)");
    frame->format = codec->pix_fmt;
    frame->width = width;
    frame->height = height;
    check_ffmpeg(av_frame_get_buffer(frame.get(), 32), "av_frame_get_buffer(png)");

    const AVPixelFormat source_format = color
        ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAYF32LE;
    SwsPtr scaler{sws_getContext(
        source.width, source.height, source_format,
        width, height, codec->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr)};
    if (!scaler) throw std::runtime_error("sws_getContext(preview) failed");
    const std::uint8_t *source_planes[4] = {
        color ? source.rgb24.data()
              : reinterpret_cast<const std::uint8_t *>(source.pixels.data()),
        nullptr, nullptr, nullptr};
    const int source_lines[4] = {
        color ? source.width * 3
              : source.width * static_cast<int>(sizeof(float)), 0, 0, 0};
    check_ffmpeg(sws_scale(scaler.get(), source_planes, source_lines, 0,
                           source.height, frame->data, frame->linesize),
                 "sws_scale(preview)");
    frame->pts = 0;
    check_ffmpeg(avcodec_send_frame(codec.get(), frame.get()),
                 "avcodec_send_frame(png)");
    check_ffmpeg(avcodec_receive_packet(codec.get(), packet.get()),
                 "avcodec_receive_packet(png)");
    PreviewImage result;
    result.width = width;
    result.height = height;
    result.png.assign(packet->data, packet->data + packet->size);
    return result;
}

void decode_selected_cuda(const std::string &path, const MediaIndex &index,
                          std::span<const FrameIdentity> selected_frames,
                          const DecoderOptions &options, std::stop_token stop,
                          const CudaFrameConsumer &consumer,
                          DecodeTelemetry *telemetry) {
#if !defined(GETNATIVE_HAS_CUDA)
    (void)path;
    (void)index;
    (void)selected_frames;
    (void)options;
    (void)stop;
    (void)consumer;
    (void)telemetry;
    throw std::runtime_error("CUDA hardware decode was not compiled");
#else
    if (options.backend != DecoderOptions::Backend::cuda) {
        throw std::invalid_argument("decode_selected_cuda requires the CUDA backend");
    }
    if (selected_frames.empty()) return;
    const auto start = Clock::now();
    OpenedDecoder opened = open_decoder(path, index.stream_index, &options);
    std::uint64_t frame_index = 0U;
    const std::uint64_t anchor = selected_frames.front().keyframe_anchor;
    if (anchor < index.frames.size() && index.frames[anchor].keyframe_timestamp) {
        const std::int64_t timestamp = *index.frames[anchor].keyframe_timestamp;
        check_ffmpeg(avformat_seek_file(opened.format.get(),
                                       static_cast<int>(index.stream_index),
                                       std::numeric_limits<std::int64_t>::min(),
                                       timestamp, timestamp, AVSEEK_FLAG_BACKWARD),
                     "avformat_seek_file(CUDA)");
        avcodec_flush_buffers(opened.codec.get());
        frame_index = anchor;
    }
    FramePtr frame{av_frame_alloc()};
    PacketPtr packet{av_packet_alloc()};
    if (!frame || !packet) throw std::runtime_error("FFmpeg frame allocation failed");
    std::uint64_t decoded_frames = 0U;
    std::size_t selected_index = 0U;
    double consumer_ms = 0.0;
    auto process = [&](AVFrame &source) {
        if (selected_index >= selected_frames.size()) return;
        ++decoded_frames;
        if (frame_index != selected_frames[selected_index].frame_index) {
            ++frame_index;
            return;
        }
        if (source.format != AV_PIX_FMT_CUDA || source.hw_frames_ctx == nullptr) {
            throw std::runtime_error(
                "FFmpeg CUDA decoder returned a non-CUDA frame");
        }
        const auto *frames = reinterpret_cast<const AVHWFramesContext *>(
            source.hw_frames_ctx->data);
        CudaLumaFormat format;
        std::int32_t bit_depth = options.expected_bit_depth;
        switch (frames->sw_format) {
        case AV_PIX_FMT_NV12:
            format = CudaLumaFormat::nv12;
            bit_depth = 8;
            break;
        case AV_PIX_FMT_P010LE:
            format = CudaLumaFormat::p010;
            bit_depth = 10;
            break;
        case AV_PIX_FMT_P016LE:
            format = CudaLumaFormat::p016;
            if (bit_depth == 0) bit_depth = 16;
            break;
        case AV_PIX_FMT_YUV444P:
            format = CudaLumaFormat::yuv444p8;
            bit_depth = 8;
            break;
        case AV_PIX_FMT_YUV444P9LE:
        case AV_PIX_FMT_YUV444P10LE:
        case AV_PIX_FMT_YUV444P12LE:
        case AV_PIX_FMT_YUV444P14LE:
        case AV_PIX_FMT_YUV444P16LE:
            format = CudaLumaFormat::yuv444p16;
            if (const AVPixFmtDescriptor *descriptor =
                    av_pix_fmt_desc_get(frames->sw_format)) {
                bit_depth = descriptor->comp[0].depth;
            }
            break;
        case AV_PIX_FMT_YUV444P10MSBLE:
        case AV_PIX_FMT_YUV444P12MSBLE:
            format = CudaLumaFormat::p016;
            if (const AVPixFmtDescriptor *descriptor =
                    av_pix_fmt_desc_get(frames->sw_format)) {
                bit_depth = descriptor->comp[0].depth;
            }
            break;
        default:
            throw std::runtime_error(
                std::string{"unsupported CUDA decoder surface format: "}
                + av_get_pix_fmt_name(frames->sw_format));
        }
        if (source.data[0] == nullptr || source.linesize[0] <= 0) {
            throw std::runtime_error("FFmpeg CUDA frame has no luma plane");
        }
        CudaFrame output;
        output.seq = selected_index;
        output.identity = selected_frames[selected_index];
        output.width = source.width;
        output.height = source.height;
        output.device_pointer = reinterpret_cast<std::uintptr_t>(source.data[0]);
        output.pitch_bytes = static_cast<std::size_t>(source.linesize[0]);
        output.format = format;
        output.bit_depth = bit_depth;
        output.range = frame_range(source);
        output.context = options.native_context;
        output.producer_stream = options.native_queue;
        output.lease = retain_frame(source, "av_frame_clone(CUDA)");
        const auto consumer_start = Clock::now();
        consumer(std::move(output));
        consumer_ms += std::chrono::duration<double, std::milli>(
            Clock::now() - consumer_start).count();
        if (telemetry) telemetry->selected_frames += 1U;
        ++selected_index;
        ++frame_index;
    };
    while (selected_index < selected_frames.size()
           && av_read_frame(opened.format.get(), packet.get()) >= 0) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        if (packet->stream_index == static_cast<int>(index.stream_index)) {
            check_ffmpeg(avcodec_send_packet(opened.codec.get(), packet.get()),
                         "avcodec_send_packet(CUDA)");
            receive_frames(*opened.codec, stop, process);
        }
        av_packet_unref(packet.get());
    }
    check_ffmpeg(avcodec_send_packet(opened.codec.get(), nullptr),
                 "avcodec_send_packet(CUDA flush)");
    receive_frames(*opened.codec, stop, process);
    if (selected_index != selected_frames.size()) {
        throw std::runtime_error(
            "CUDA decoder ended before all selected frames were produced");
    }
    if (telemetry) {
        telemetry->decoded_frames = decoded_frames;
        telemetry->decode_ms = std::max(
            0.0,
            std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                - consumer_ms);
    }
#endif
}

void decode_selected_vulkan(const std::string &path,
                            const MediaIndex &index,
                            std::span<const FrameIdentity> selected_frames,
                            const DecoderOptions &options, std::stop_token stop,
                            const VulkanFrameConsumer &consumer,
                            DecodeTelemetry *telemetry) {
#if !defined(GETNATIVE_HAS_VULKAN)
    (void)path;
    (void)index;
    (void)selected_frames;
    (void)options;
    (void)stop;
    (void)consumer;
    (void)telemetry;
    throw std::runtime_error("Vulkan Video decode was not compiled");
#else
    if (options.backend != DecoderOptions::Backend::vulkan_video) {
        throw std::invalid_argument(
            "decode_selected_vulkan requires the Vulkan Video backend");
    }
    if (selected_frames.empty()) return;
    const auto start = Clock::now();
    OpenedDecoder opened = open_decoder(path, index.stream_index, &options);
    std::uint64_t frame_index = 0U;
    const std::uint64_t anchor = selected_frames.front().keyframe_anchor;
    if (anchor < index.frames.size() && index.frames[anchor].keyframe_timestamp) {
        const std::int64_t timestamp = *index.frames[anchor].keyframe_timestamp;
        check_ffmpeg(avformat_seek_file(opened.format.get(),
                                       static_cast<int>(index.stream_index),
                                       std::numeric_limits<std::int64_t>::min(),
                                       timestamp, timestamp, AVSEEK_FLAG_BACKWARD),
                     "avformat_seek_file(Vulkan)");
        avcodec_flush_buffers(opened.codec.get());
        frame_index = anchor;
    }
    PacketPtr packet{av_packet_alloc()};
    if (!packet) throw std::runtime_error("FFmpeg packet allocation failed");
    std::uint64_t decoded_frames = 0U;
    std::size_t selected_index = 0U;
    double consumer_ms = 0.0;
    auto process = [&](AVFrame &source) {
        if (selected_index >= selected_frames.size()) return;
        ++decoded_frames;
        if (frame_index != selected_frames[selected_index].frame_index) {
            ++frame_index;
            return;
        }
        if (source.format != AV_PIX_FMT_VULKAN
            || source.hw_frames_ctx == nullptr || source.data[0] == nullptr) {
            throw std::runtime_error(
                "FFmpeg Vulkan decoder returned a non-Vulkan frame");
        }
        auto *frames = reinterpret_cast<AVHWFramesContext *>(
            source.hw_frames_ctx->data);
        auto *vulkan_frames = static_cast<AVVulkanFramesContext *>(frames->hwctx);
        auto *vulkan_frame = reinterpret_cast<AVVkFrame *>(source.data[0]);
        if (vulkan_frames == nullptr || vulkan_frame->img[0] == VK_NULL_HANDLE
            || vulkan_frame->sem[0] == VK_NULL_HANDLE) {
            throw std::runtime_error("FFmpeg Vulkan frame has no luma image");
        }
        const VkFormat image_format = vulkan_frames->format[0];
        const VulkanLumaDescription luma =
            vulkan_luma_description(image_format);
        std::int32_t bit_depth = options.expected_bit_depth;
        if (const AVPixFmtDescriptor *descriptor =
                av_pix_fmt_desc_get(frames->sw_format)) {
            bit_depth = descriptor->comp[0].depth;
        }
        if (bit_depth <= 0) bit_depth = luma.normalized_bits;

        auto lease = std::make_shared<VulkanFrameLease>(source);
        auto *leased_frame = reinterpret_cast<AVVkFrame *>(lease->frame->data[0]);
        VulkanFrame output;
        output.seq = selected_index;
        output.identity = selected_frames[selected_index];
        output.width = source.width;
        output.height = source.height;
        output.image = native_value(leased_frame->img[0]);
        output.image_format = static_cast<std::uint32_t>(image_format);
        output.view_format = static_cast<std::uint32_t>(luma.view_format);
        output.aspect_mask = static_cast<std::uint32_t>(luma.aspect);
        output.bit_depth = bit_depth;
        output.normalized_sample_bits = luma.normalized_bits;
        output.range = frame_range(source);
        output.layout = static_cast<std::uint32_t>(leased_frame->layout[0]);
        output.access = static_cast<std::uint32_t>(leased_frame->access[0]);
        output.queue_family = leased_frame->queue_family[0];
        output.semaphore = native_value(leased_frame->sem[0]);
        output.semaphore_value = leased_frame->sem_value[0];
        output.sync_opaque = lease->lock.get();
        output.mark_submitted = VulkanFrameLock::mark_submitted;
        output.release_without_submit =
            VulkanFrameLock::release_without_submit;
        output.lease = std::move(lease);
        const auto consumer_start = Clock::now();
        consumer(std::move(output));
        consumer_ms += std::chrono::duration<double, std::milli>(
            Clock::now() - consumer_start).count();
        if (telemetry) telemetry->selected_frames += 1U;
        ++selected_index;
        ++frame_index;
    };
    while (selected_index < selected_frames.size()
           && av_read_frame(opened.format.get(), packet.get()) >= 0) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        if (packet->stream_index == static_cast<int>(index.stream_index)) {
            check_ffmpeg(avcodec_send_packet(opened.codec.get(), packet.get()),
                         "avcodec_send_packet(Vulkan)");
            receive_frames(*opened.codec, stop, process);
        }
        av_packet_unref(packet.get());
    }
    check_ffmpeg(avcodec_send_packet(opened.codec.get(), nullptr),
                 "avcodec_send_packet(Vulkan flush)");
    receive_frames(*opened.codec, stop, process);
    if (selected_index != selected_frames.size()) {
        throw std::runtime_error(
            "Vulkan decoder ended before all selected frames were produced");
    }
    if (telemetry) {
        telemetry->decoded_frames = decoded_frames;
        telemetry->decode_ms = std::max(
            0.0,
            std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                - consumer_ms);
    }
#endif
}

} // namespace getnative::media
