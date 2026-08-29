#include "getnative/media_decode.hpp"
#include "getnative/utf8_path.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <thread>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/hash.h>
#include <libavutil/hwcontext.h>
#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <libavutil/hwcontext_videotoolbox.h>
#endif
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

#if defined(__APPLE__)
[[nodiscard]] AVPixelFormat select_videotoolbox_format(AVCodecContext *, const AVPixelFormat *formats) {
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format)
        if (*format == AV_PIX_FMT_VIDEOTOOLBOX) return *format;
    return AV_PIX_FMT_NONE;
}
[[nodiscard]] bool decoder_supports_videotoolbox(const AVCodec &decoder) {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(&decoder, index);
        if (config == nullptr) return false;
        if (config->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX
            && config->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) return true;
    }
}
[[nodiscard]] std::optional<CMVideoCodecType> videotoolbox_codec_type(
    const AVCodec &decoder) noexcept {
    switch (decoder.id) {
    case AV_CODEC_ID_AV1: return kCMVideoCodecType_AV1;
    case AV_CODEC_ID_H264: return kCMVideoCodecType_H264;
    case AV_CODEC_ID_HEVC: return kCMVideoCodecType_HEVC;
    case AV_CODEC_ID_MPEG1VIDEO: return kCMVideoCodecType_MPEG1Video;
    case AV_CODEC_ID_MPEG2VIDEO: return kCMVideoCodecType_MPEG2Video;
    case AV_CODEC_ID_MPEG4: return kCMVideoCodecType_MPEG4Video;
    case AV_CODEC_ID_PRORES: return kCMVideoCodecType_AppleProRes422;
    case AV_CODEC_ID_VP9: return kCMVideoCodecType_VP9;
    default: return std::nullopt;
    }
}
[[nodiscard]] bool videotoolbox_runtime_supports(const AVCodec &decoder) noexcept {
    const auto codec_type = videotoolbox_codec_type(decoder);
    return codec_type.has_value() && VTIsHardwareDecodeSupported(*codec_type);
}
void configure_videotoolbox_decoder(AVCodecContext &codec, const AVCodec &decoder) {
    if (!decoder_supports_videotoolbox(decoder))
        throw std::runtime_error("FFmpeg decoder exposes no VideoToolbox hardware-device configuration");
    AVBufferRef *device_ref = nullptr;
    const int init_result = av_hwdevice_ctx_create(
        &device_ref, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (init_result < 0) {
        av_buffer_unref(&device_ref);
        throw std::runtime_error("av_hwdevice_ctx_create(VideoToolbox) failed");
    }
    codec.hw_device_ctx = device_ref;
    codec.get_format = select_videotoolbox_format;
    codec.extra_hw_frames = 8;
}
#endif

#if defined(GETNATIVE_HAS_VULKAN)
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#include <vulkan/vulkan.h>
#endif

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
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
    AVStream *stream = nullptr;
    const AVCodec *decoder = nullptr;
};

void import_index_entries(AVStream &stream, const MediaIndex *index);

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

struct BuiltDecoder {
    CodecPtr codec;
    const AVCodec *decoder = nullptr;
};

[[nodiscard]] BuiltDecoder build_decoder(AVStream &stream,
                                         const DecoderOptions *options,
                                         const ExtraDataInfo *configuration = nullptr) {
    const AVCodecID codec_id = configuration != nullptr && configuration->codec_id != 0U
        ? static_cast<AVCodecID>(configuration->codec_id)
        : stream.codecpar->codec_id;
    const AVCodec *decoder = avcodec_find_decoder(codec_id);
    if (decoder == nullptr) throw std::runtime_error("no FFmpeg decoder for video codec");
    CodecPtr codec{avcodec_alloc_context3(decoder)};
    if (!codec) throw std::runtime_error("avcodec_alloc_context3 failed");
    check_ffmpeg(avcodec_parameters_to_context(codec.get(), stream.codecpar),
                 "avcodec_parameters_to_context");
    codec->codec_id = codec_id;
    if (configuration != nullptr) {
        codec->codec_tag = configuration->fourcc;
        if (configuration->width > 0) codec->width = configuration->width;
        if (configuration->height > 0) codec->height = configuration->height;
        if (!configuration->pixel_format.empty()) {
            const AVPixelFormat format = av_get_pix_fmt(configuration->pixel_format.c_str());
            if (format != AV_PIX_FMT_NONE) codec->pix_fmt = format;
        }
        av_freep(&codec->extradata);
        codec->extradata_size = 0;
        if (!configuration->data.empty()) {
            codec->extradata = static_cast<std::uint8_t *>(av_mallocz(
                configuration->data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (codec->extradata == nullptr) {
                throw std::runtime_error("failed to allocate decoder extradata");
            }
            std::memcpy(codec->extradata, configuration->data.data(),
                        configuration->data.size());
            codec->extradata_size = static_cast<int>(configuration->data.size());
        }
    }
    // Software GOP scans benefit from FFmpeg's automatic frame threading.
    // NVDEC folds the thread count into its decode-surface pool, and large
    // host CPU counts can otherwise exceed the driver's 32-surface limit.
    codec->thread_count = options == nullptr
            || options->backend == DecoderOptions::Backend::software
        ? 0 : 1;
    if (options != nullptr && options->backend != DecoderOptions::Backend::software) {
        if (options->backend == DecoderOptions::Backend::cuda) {
#if defined(GETNATIVE_HAS_CUDA)
            configure_cuda_decoder(*codec, *decoder, *options);
#else
            throw std::runtime_error("CUDA hardware decode was not compiled");
#endif
        } else if (options->backend == DecoderOptions::Backend::vulkan_video) {
#if defined(GETNATIVE_HAS_VULKAN)
            configure_vulkan_decoder(*codec, *decoder, *options);
#else
            throw std::runtime_error("Vulkan Video decode was not compiled");
#endif
        } else {
#if defined(__APPLE__)
            configure_videotoolbox_decoder(*codec, *decoder);
#else
            throw std::runtime_error("VideoToolbox hardware decode is only available on macOS");
#endif
        }
    }
    check_ffmpeg(avcodec_open2(codec.get(), decoder, nullptr), "avcodec_open2");
    return {std::move(codec), decoder};
}

[[nodiscard]] OpenedDecoder open_decoder(const std::string &path,
                                          std::uint32_t stream_index,
                                          const DecoderOptions *options = nullptr,
                                          const MediaIndex *index = nullptr,
                                          const ExtraDataInfo *configuration = nullptr) {
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
    AVStream *stream = format->streams[stream_index];
    import_index_entries(*stream, index);
    BuiltDecoder built = build_decoder(*stream, options, configuration);
    return {std::move(format), std::move(built.codec), stream, built.decoder};
}

[[nodiscard]] const ExtraDataInfo *indexed_decoder_configuration(
    const MediaIndex &index, std::uint64_t anchor) {
    if (anchor >= index.frames.size()) return nullptr;
    const std::uint32_t extradata_index =
        index.frames[anchor].extradata_index;
    const auto configuration = std::find_if(
        index.extradata.begin(), index.extradata.end(),
        [extradata_index](const ExtraDataInfo &entry) {
            return entry.index == extradata_index;
        });
    return configuration == index.extradata.end() ? nullptr : &*configuration;
}

[[nodiscard]] std::optional<std::int64_t> timestamp_value(std::int64_t value) {
    return value == AV_NOPTS_VALUE ? std::nullopt : std::optional{value};
}

[[nodiscard]] std::int64_t source_mtime_ns(const std::filesystem::path &path) {
    const auto value = std::filesystem::last_write_time(path).time_since_epoch();
#if defined(_WIN32)
    // MSVC's file clock counts 100 ns ticks since 1601-01-01 (FILETIME).
    // Rebase onto the Unix epoch so stored mtimes match indexes written on
    // other platforms and no longer overflow int64 into negative values.
    constexpr std::int64_t filetime_unix_offset = 116444736000000000LL;
    const auto ticks = std::chrono::duration_cast<
        std::chrono::duration<std::int64_t, std::ratio<1, 10000000LL>>>(value).count();
    return (ticks - filetime_unix_offset) * 100LL;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
#endif
}

// MPEG-TS/PS and raw elementary streams have no keyframe index, so
// avformat_seek_file() binary-searches by timestamp and lands on the last
// packet with dts <= target. With B-frame reordering the IDR's pts equals
// the dts of a following packet, so seeking by the keyframe pts starts
// decoding mid-GOP without SPS/PPS; FFmpeg 8 rejects the first such packet
// with AVERROR_INVALIDDATA while older releases silently decoded the wrong
// GOP. The index stores the exact byte offset of every frame's payload, so
// position these demuxers directly and only fall back to a timestamp seek
// (using the keyframe dts, not pts) when no offset is known.
[[nodiscard]] bool seeks_by_byte_position(const AVFormatContext &format,
                                          const MediaIndex &index) {
    if (index.raw_demuxer) return true;
    const char *name = format.iformat != nullptr ? format.iformat->name : nullptr;
    if (name == nullptr) return false;
    const std::string_view view{name};
    return view == "mpegts" || view == "mpegtsraw" || view == "mpeg";
}

void import_index_entries(AVStream &stream, const MediaIndex *index) {
    if (index == nullptr || index->stream_index_entries.empty()) return;
    for (const StreamIndexEntry &entry : index->stream_index_entries) {
        if (entry.file_position < 0) continue;
        if (entry.size > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || entry.distance > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || entry.flags > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("LWI stream index entry exceeds FFmpeg integer range");
        }
        const int result = av_add_index_entry(
            &stream, entry.file_position, entry.timestamp,
            static_cast<int>(entry.size), static_cast<int>(entry.distance),
            static_cast<int>(entry.flags));
        if (result < 0) {
            throw std::runtime_error("failed to import LWI stream index entry");
        }
    }
}

[[nodiscard]] std::int64_t m2ts_seek_position(AVFormatContext &format,
                                               std::int64_t position) {
    if (format.pb == nullptr || format.iformat == nullptr
        || format.iformat->name == nullptr
        || std::string_view{format.iformat->name} != "mpegts") {
        return position;
    }
    std::array<unsigned char, 5> probe{};
    if (avio_seek(format.pb, position, SEEK_SET) < 0
        || avio_read(format.pb, probe.data(), static_cast<int>(probe.size())) != 5) {
        return position;
    }
    // Blu-ray M2TS packets carry a four-byte time-code header before the TS
    // sync byte. Plain transport streams begin directly with 0x47.
    if (probe[0] != 0x47U && probe[4] == 0x47U) return position + 4;
    return position;
}

[[nodiscard]] bool seek_to_keyframe(AVFormatContext &format,
                                    const MediaIndex &index,
                                    const FrameIdentity &anchor,
                                    bool require_exact = false) {
    if (seeks_by_byte_position(format, index)) {
        if (anchor.file_position && format.pb != nullptr
            && avio_seek(format.pb, m2ts_seek_position(format, *anchor.file_position), SEEK_SET) >= 0) {
            avformat_flush(&format);
            return true;
        }
        if (anchor.dts || anchor.pts) {
            const std::int64_t timestamp = anchor.dts ? *anchor.dts : *anchor.pts;
            int result = avformat_seek_file(&format,
                                            static_cast<int>(index.stream_index),
                                            std::numeric_limits<std::int64_t>::min(),
                                            timestamp, timestamp, AVSEEK_FLAG_BACKWARD);
            if (result < 0) {
                result = avformat_seek_file(&format,
                                            static_cast<int>(index.stream_index),
                                            std::numeric_limits<std::int64_t>::min(),
                                            timestamp, timestamp,
                                            AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            }
            return result >= 0;
        }
        return false;
    }
    // FFmpeg's demuxer index entries are generally keyed by packet DTS. For
    // reordered streams, seeking with the RAP's PTS can select a later packet
    // whose DTS is already past the IDR (notably MP4/MOV with B-frames).
    const std::optional<std::int64_t> timestamp = anchor.dts && *anchor.dts >= 0
        ? anchor.dts : anchor.keyframe_timestamp.has_value()
            ? anchor.keyframe_timestamp : anchor.pts;
    if (timestamp) {
        int result = -1;
        if (require_exact) {
            result = avformat_seek_file(&format,
                                        static_cast<int>(index.stream_index),
                                        *timestamp, *timestamp, *timestamp,
                                        AVSEEK_FLAG_BACKWARD);
        }
        if (result < 0) {
            result = avformat_seek_file(&format,
                                        static_cast<int>(index.stream_index),
                                        std::numeric_limits<std::int64_t>::min(),
                                        *timestamp, *timestamp,
                                        AVSEEK_FLAG_BACKWARD);
        }
        if (result < 0) {
            result = avformat_seek_file(&format,
                                        static_cast<int>(index.stream_index),
                                        std::numeric_limits<std::int64_t>::min(),
                                        *timestamp, *timestamp,
                                        AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        }
        return result >= 0;
    }
    if (anchor.dts || anchor.pts) {
        const std::int64_t fallback = anchor.dts ? *anchor.dts : *anchor.pts;
        if (avformat_seek_file(&format, static_cast<int>(index.stream_index),
                               std::numeric_limits<std::int64_t>::min(), fallback,
                               fallback, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) >= 0) {
            return true;
        }
    }
    return false;
}

// Frames are matched to index entries by output order; a packet the
// decoder dropped (or a seek that landed mid-GOP) would silently shift
// that mapping and deliver the wrong pixels. Whenever both sides carry a
// timestamp, require them to agree so a desync is an explicit error.
void ensure_presentation_match(const AVFrame &frame, const FrameIdentity &expected) {
    if (frame.pts == AV_NOPTS_VALUE || !expected.pts) return;
    if (frame.pts != *expected.pts) {
        throw std::runtime_error(
            "decoder output desynchronized from the media index (expected pts "
            + std::to_string(*expected.pts) + ", got "
            + std::to_string(frame.pts) + ")");
    }
}

[[nodiscard]] int frame_identity_score(const AVFrame &frame,
                                       const FrameIdentity &expected) {
    const bool output_has_presentation_timestamp = frame.pts != AV_NOPTS_VALUE
        || frame.best_effort_timestamp != AV_NOPTS_VALUE;
    const bool index_has_presentation_timestamp = expected.pts.has_value()
        || expected.best_effort_timestamp.has_value();
    int presentation_score = 0;
    const auto add_match = [&](std::int64_t actual,
                               const std::optional<std::int64_t> &indexed,
                               int weight) {
        if (actual != AV_NOPTS_VALUE && indexed && actual == *indexed) {
            presentation_score += weight;
        }
    };
    add_match(frame.pts, expected.pts, 16);
    add_match(frame.best_effort_timestamp, expected.best_effort_timestamp, 16);
    add_match(frame.best_effort_timestamp, expected.pts, 12);
    add_match(frame.pts, expected.best_effort_timestamp, 12);
    if (output_has_presentation_timestamp && index_has_presentation_timestamp
        && presentation_score == 0) {
        return 0;
    }
    int score = presentation_score;
    if (frame.pkt_dts != AV_NOPTS_VALUE && expected.dts
        && frame.pkt_dts == *expected.dts) {
        score += 6;
    }
    if (frame.duration > 0 && expected.packet_duration
        && frame.duration == *expected.packet_duration) {
        ++score;
    }
    return score;
}

struct IndexedDecodeRun {
    std::size_t selected_begin = 0U;
    std::size_t selected_end = 0U;
    std::uint64_t anchor = 0U;
    std::uint64_t decode_anchor = 0U;
    std::optional<std::int64_t> end_position;
    std::optional<std::uint64_t> end_decode_index;
    bool streaming = false;
};

[[nodiscard]] std::optional<std::uint64_t> preceding_rap(
    const MediaIndex &index, std::uint64_t current) {
    if (current >= index.frames.size()) return std::nullopt;
    const std::uint64_t current_decode = index.frames[current].decode_index;
    std::optional<std::uint64_t> best;
    std::uint64_t best_decode = 0U;
    for (const FrameIdentity &candidate : index.frames) {
        if (!candidate.rap || candidate.decode_index >= current_decode) continue;
        if (!best || candidate.decode_index > best_decode) {
            best = candidate.frame_index;
            best_decode = candidate.decode_index;
        }
    }
    return best;
}

void validate_selected_frames(const MediaIndex &index,
                              std::span<const FrameIdentity> selected) {
    for (std::size_t ordinal = 0U; ordinal < selected.size(); ++ordinal) {
        const FrameIdentity &identity = selected[ordinal];
        if (identity.frame_index >= index.frames.size()
            || index.frames[identity.frame_index].frame_index != identity.frame_index
            || (ordinal != 0U
                && identity.frame_index <= selected[ordinal - 1U].frame_index)) {
            throw std::invalid_argument(
                "selected frame identities are not an ordered subset of the media index");
        }
    }
}

[[nodiscard]] std::vector<IndexedDecodeRun> plan_indexed_decode_runs(
    const MediaIndex &index, std::span<const FrameIdentity> selected,
    bool split_dense_at_rap = false) {
    validate_selected_frames(index, selected);
    std::vector<IndexedDecodeRun> runs;
    if (selected.empty()) return runs;
    const std::uint64_t covered = selected.back().frame_index
        - selected.front().frame_index + 1U;
    const bool dense = !split_dense_at_rap && selected.size() * 2U >= covered;
    std::size_t begin = 0U;
    while (begin < selected.size()) {
        const std::uint64_t anchor = selected[begin].keyframe_anchor;
        const std::uint32_t extradata_index = selected[begin].extradata_index;
        std::size_t end = begin + 1U;
        while (end < selected.size()
               && selected[end].extradata_index == extradata_index
               && (dense || selected[end].keyframe_anchor == anchor)) {
            ++end;
        }
        bool leading = false;
        for (std::size_t target = begin; target < end; ++target) {
            const FrameIdentity &identity = selected[target];
            leading = leading || identity.leading_frame || identity.frame_index < anchor;
        }
        const std::uint64_t last_anchor = selected[end - 1U].keyframe_anchor;
        std::optional<std::int64_t> end_position;
        std::optional<std::uint64_t> end_decode_index;
        bool passed_drain_rap = false;
        for (std::size_t next = static_cast<std::size_t>(last_anchor) + 1U;
             next < index.frames.size(); ++next) {
            if (!index.frames[next].rap) continue;
            if (passed_drain_rap) {
                end_position = index.frames[next].file_position;
                end_decode_index = index.frames[next].decode_index;
                break;
            }
            passed_drain_rap = true;
        }
        runs.push_back({begin, end, anchor,
                        leading ? preceding_rap(index, anchor).value_or(0U) : anchor,
                        end_position, end_decode_index, dense && !leading});
        begin = end;
    }
    return runs;
}

class IndexedIdentityLookup {
public:
    explicit IndexedIdentityLookup(const MediaIndex &index) : index_(index) {
        for (std::size_t ordinal = 0U; ordinal < index_.frames.size(); ++ordinal) {
            const FrameIdentity &identity = index_.frames[ordinal];
            add(presentation_, identity.pts, ordinal);
            if (identity.best_effort_timestamp != identity.pts) {
                add(presentation_, identity.best_effort_timestamp, ordinal);
            }
            add(decode_, identity.dts, ordinal);
        }
    }

    [[nodiscard]] std::vector<std::size_t> candidates(const AVFrame &frame) const {
        std::vector<std::size_t> result;
        append(result, presentation_, frame.pts);
        append(result, presentation_, frame.best_effort_timestamp);
        append(result, decode_, frame.pkt_dts);
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    [[nodiscard]] const FrameIdentity &identity(std::size_t ordinal) const {
        return index_.frames[ordinal];
    }

    [[nodiscard]] const FrameIdentity *frame(std::uint64_t frame_index) const {
        if (frame_index >= index_.frames.size()
            || index_.frames[frame_index].frame_index != frame_index) {
            return nullptr;
        }
        return &index_.frames[frame_index];
    }

private:
    using Lookup = std::unordered_map<std::int64_t, std::vector<std::size_t>>;

    static void add(Lookup &lookup, const std::optional<std::int64_t> &value,
                    std::size_t ordinal) {
        if (value) lookup[*value].push_back(ordinal);
    }

    static void append(std::vector<std::size_t> &output, const Lookup &lookup,
                       std::int64_t value) {
        if (value == AV_NOPTS_VALUE) return;
        const auto found = lookup.find(value);
        if (found != lookup.end()) {
            output.insert(output.end(), found->second.begin(), found->second.end());
        }
    }

    const MediaIndex &index_;
    Lookup presentation_;
    Lookup decode_;
};

struct IndexedFrameMatch {
    std::optional<std::size_t> selected_target;
    std::optional<std::uint64_t> frame_index;
};

class IndexedFrameMatcher {
public:
    IndexedFrameMatcher(const IndexedIdentityLookup &lookup,
                        std::span<const FrameIdentity> selected,
                        std::uint64_t minimum_decode_index,
                        std::optional<std::uint64_t> maximum_decode_index)
        : lookup_(lookup), selected_(selected),
          minimum_decode_index_(minimum_decode_index),
          maximum_decode_index_(maximum_decode_index) {
        for (std::size_t target = 0U; target < selected_.size(); ++target) {
            selected_by_frame_.emplace(selected_[target].frame_index, target);
        }
    }

    [[nodiscard]] IndexedFrameMatch match(
        const AVFrame &frame, std::span<const char> delivered,
        bool allow_unstamped_order, std::uint64_t presentation_cursor) const {
        int best_score = 0;
        std::optional<std::size_t> best_ordinal;
        bool ambiguous = false;
        for (const std::size_t ordinal : lookup_.candidates(frame)) {
            const FrameIdentity &candidate = lookup_.identity(ordinal);
            if (candidate.decode_index < minimum_decode_index_
                || (maximum_decode_index_
                    && candidate.decode_index >= *maximum_decode_index_)) {
                continue;
            }
            const int score = frame_identity_score(frame, candidate);
            if (score > best_score) {
                best_score = score;
                best_ordinal = ordinal;
                ambiguous = false;
            } else if (score > 0 && score == best_score) {
                ambiguous = true;
            }
        }
        if (ambiguous) {
            throw std::runtime_error(
                "decoder output ambiguously matches duplicate indexed timestamps");
        }
        if (best_ordinal) {
            const std::uint64_t frame_index =
                lookup_.identity(*best_ordinal).frame_index;
            const auto target = selected_by_frame_.find(frame_index);
            if (target != selected_by_frame_.end() && !delivered[target->second]) {
                return {target->second, frame_index};
            }
            return {std::nullopt, frame_index};
        }
        const bool output_timestamped = frame.pts != AV_NOPTS_VALUE
            || frame.best_effort_timestamp != AV_NOPTS_VALUE
            || frame.pkt_dts != AV_NOPTS_VALUE;
        if (output_timestamped) {
            // An index entry may be completely unstamped even when FFmpeg can
            // synthesize a timestamp during decode. Order fallback is valid
            // only for that specific indexed identity; timestamped neighbors
            // continue through the strict matcher.
            const FrameIdentity *ordered = lookup_.frame(presentation_cursor);
            if (allow_unstamped_order && ordered != nullptr
                && !ordered->pts && !ordered->best_effort_timestamp
                && !ordered->dts) {
                const auto target = selected_by_frame_.find(presentation_cursor);
                if (target != selected_by_frame_.end() && !delivered[target->second]) {
                    return {target->second, presentation_cursor};
                }
                return {std::nullopt, presentation_cursor};
            }
            return {};
        }
        if (!allow_unstamped_order) {
            throw std::runtime_error(
                "hardware decoder output has no timestamp or indexed identity evidence");
        }
        const auto target = selected_by_frame_.find(presentation_cursor);
        if (target != selected_by_frame_.end() && !delivered[target->second]) {
            return {target->second, presentation_cursor};
        }
        return {std::nullopt, presentation_cursor};
    }

private:
    const IndexedIdentityLookup &lookup_;
    std::span<const FrameIdentity> selected_;
    std::uint64_t minimum_decode_index_ = 0U;
    std::optional<std::uint64_t> maximum_decode_index_;
    std::unordered_map<std::uint64_t, std::size_t> selected_by_frame_;
};

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

// Variant that stops consuming the moment `done` turns true, checked before
// every receive. Persistent sessions rely on this: frames pulled after the
// last target would be discarded, and a later request continuing inside the
// same GOP could never match them again.
template <class Callback, class Done>
void receive_frames_until(AVCodecContext &codec, std::stop_token stop,
                          Callback &&callback, Done &&done) {
    FramePtr frame{av_frame_alloc()};
    if (!frame) throw std::runtime_error("av_frame_alloc failed");
    for (;;) {
        if (done()) return;
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        const int result = avcodec_receive_frame(&codec, frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
        check_ffmpeg(result, "avcodec_receive_frame");
        callback(*frame);
        av_frame_unref(frame.get());
        if (done()) return;
    }
}

// ffmpeg CLI semantics for a corrupted packet (broadcast capture, damaged
// sector): drop it and keep decoding. The packet is gone from the decoder
// on error, so the pipeline stays usable; any frame loss or reordering this
// causes is handled by the timestamp matching (or the presentation check on
// the cursor path) when later frames are delivered.
//
// Frame-threaded decoders legitimately answer EAGAIN when their internal
// packet queue is still full after a drain; the packet must be received out
// and resent. Dropping it would silently skip frames mid-GOP.
template <class Callback>
void send_packet_tolerant(AVCodecContext &codec, AVPacket *packet,
                          std::stop_token stop, DecodeTelemetry *telemetry,
                          std::string_view operation, Callback &&drain) {
    for (;;) {
        const int sent = avcodec_send_packet(&codec, packet);
        if (sent == AVERROR_INVALIDDATA) {
            if (telemetry != nullptr) ++telemetry->discarded_packets;
            return;
        }
        if (sent == AVERROR(EAGAIN)) {
            receive_frames(codec, stop, drain);
            continue;
        }
        check_ffmpeg(sent, operation);
        return;
    }
}

[[maybe_unused, nodiscard]] std::string frame_range(const AVFrame &frame) {
    switch (frame.color_range) {
    case AVCOL_RANGE_MPEG: return "limited";
    case AVCOL_RANGE_JPEG: return "full";
    default: return "unknown";
    }
}

[[nodiscard]] int sws_colorspace(AVColorSpace colorspace) {
    switch (colorspace) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    default: return SWS_CS_DEFAULT;
    }
}

void configure_scaler_colorspace(SwsContext &scaler, const AVFrame &source) {
    const int source_range = source.color_range == AVCOL_RANGE_JPEG
        || source.colorspace == AVCOL_SPC_RGB ? 1 : 0;
    const int *coefficients = sws_getCoefficients(sws_colorspace(source.colorspace));
    (void)sws_setColorspaceDetails(&scaler, coefficients, source_range,
                                   coefficients, 1, 0, 1 << 16, 1 << 16);
}

void update_identity_metadata(FrameIdentity &identity, const AVFrame &frame) {
    identity.best_effort_timestamp = timestamp_value(frame.best_effort_timestamp);
    if (frame.duration > 0) identity.packet_duration = frame.duration;
    identity.repeat_pict = frame.repeat_pict;
    identity.field_order = (frame.flags & AV_FRAME_FLAG_INTERLACED) == 0
        ? "progressive"
        : (frame.flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0 ? "tt" : "bb";
    identity.color_range = frame.color_range;
    identity.color_space = frame.colorspace;
    identity.color_primaries = frame.color_primaries;
    identity.color_transfer = frame.color_trc;
    identity.chroma_location = frame.chroma_location;
}

[[nodiscard]] bool selected(const FrameIdentity &frame, const ScanScope &scope) {
    const std::uint64_t start = scope.start_frame.value_or(0U);
    const std::uint64_t end = scope.end_frame.value_or(std::numeric_limits<std::uint64_t>::max());
    if (start > end || frame.frame_index < start || frame.frame_index > end) return false;
    switch (scope.selection) {
    case ScanSelection::all: return true;
    case ScanSelection::decoded_i_picture:
        return frame.picture_type
            ? *frame.picture_type == "I" : frame.key_frame;
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
    case DecoderOptions::Backend::videotoolbox:
#if defined(__APPLE__)
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
#if defined(__APPLE__)
    if (backend == DecoderOptions::Backend::videotoolbox) {
        return av_hwdevice_find_type_by_name("videotoolbox")
            == AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    }
#endif
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
#if defined(__APPLE__)
    if (backend == DecoderOptions::Backend::videotoolbox) {
        std::set<std::string> names;
        void *state = nullptr;
        while (const AVCodec *codec = av_codec_iterate(&state)) {
            if (av_codec_is_decoder(codec) && decoder_supports_videotoolbox(*codec)
                && videotoolbox_runtime_supports(*codec)) {
                names.emplace(codec->name);
            }
        }
        return {names.begin(), names.end()};
    }
#endif
    return {};
}

std::string quick_fingerprint(const std::string &path) {
    const std::filesystem::path native_path = path_from_utf8(path);
    const std::uint64_t size = std::filesystem::file_size(native_path);
    std::ifstream file(native_path, std::ios::binary);
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

struct DecodedFrameMetadata {
    std::optional<std::uint64_t> decode_index;
    std::optional<std::int64_t> pts;
    std::optional<std::int64_t> best_effort_timestamp;
    std::optional<std::int64_t> packet_dts;
    std::optional<std::int64_t> duration;
    std::int32_t repeat_pict = 0;
    std::string field_order = "unknown";
    std::int32_t color_range = AVCOL_RANGE_UNSPECIFIED;
    std::int32_t color_space = AVCOL_SPC_UNSPECIFIED;
    std::int32_t color_primaries = AVCOL_PRI_UNSPECIFIED;
    std::int32_t color_transfer = AVCOL_TRC_UNSPECIFIED;
    std::int32_t chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string pixel_format;
};

class IndexMetadataDecoder {
public:
    explicit IndexMetadataDecoder(const AVCodecParameters &parameters) {
        const AVCodec *decoder = avcodec_find_decoder(parameters.codec_id);
        if (decoder == nullptr) return;
        codec_.reset(avcodec_alloc_context3(decoder));
        if (!codec_ || avcodec_parameters_to_context(codec_.get(), &parameters) < 0) {
            codec_.reset();
            return;
        }
        codec_->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        codec_->thread_count = 0;
        if (avcodec_open2(codec_.get(), decoder, nullptr) < 0) codec_.reset();
        frame_.reset(av_frame_alloc());
        if (!frame_) codec_.reset();
    }

    void feed(const std::uint8_t *data, int size, const AVPacket *source,
              const FrameIdentity &identity) {
        if (!codec_) return;
        PacketPtr packet{av_packet_alloc()};
        if (!packet || data == nullptr || size <= 0
            || av_new_packet(packet.get(), size) < 0) {
            return;
        }
        std::memcpy(packet->data, data, static_cast<std::size_t>(size));
        if (source != nullptr && av_packet_copy_props(packet.get(), source) < 0) {
            return;
        }
        packet->pts = identity.pts.value_or(AV_NOPTS_VALUE);
        packet->dts = identity.dts.value_or(AV_NOPTS_VALUE);
        packet->pos = identity.file_position.value_or(-1);
        packet->duration = identity.packet_duration.value_or(0);
        av_buffer_unref(&packet->opaque_ref);
        packet->opaque_ref = av_buffer_alloc(sizeof(identity.decode_index));
        if (packet->opaque_ref == nullptr) return;
        std::memcpy(packet->opaque_ref->data, &identity.decode_index,
                    sizeof(identity.decode_index));
        int result = avcodec_send_packet(codec_.get(), packet.get());
        if (result == AVERROR(EAGAIN)) {
            drain();
            result = avcodec_send_packet(codec_.get(), packet.get());
        }
        if (result < 0 && result != AVERROR_EOF) return;
        drain();
    }

    void finish() {
        if (!codec_) return;
        if (avcodec_send_packet(codec_.get(), nullptr) >= 0) drain();
    }

    [[nodiscard]] const std::vector<DecodedFrameMetadata> &frames() const {
        return frames_;
    }

private:
    void drain() {
        for (;;) {
            const int result = avcodec_receive_frame(codec_.get(), frame_.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            if (result < 0) return;
            DecodedFrameMetadata metadata;
            if (frame_->opaque_ref != nullptr
                && frame_->opaque_ref->size >= sizeof(std::uint64_t)) {
                std::uint64_t decode_index = 0U;
                std::memcpy(&decode_index, frame_->opaque_ref->data,
                            sizeof(decode_index));
                metadata.decode_index = decode_index;
            }
            metadata.pts = timestamp_value(frame_->pts);
            metadata.best_effort_timestamp = timestamp_value(
                frame_->best_effort_timestamp);
            metadata.packet_dts = timestamp_value(frame_->pkt_dts);
            if (frame_->duration > 0) metadata.duration = frame_->duration;
            metadata.repeat_pict = frame_->repeat_pict;
            metadata.field_order = (frame_->flags & AV_FRAME_FLAG_INTERLACED) == 0
                ? "progressive"
                : (frame_->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0 ? "tt" : "bb";
            metadata.color_range = frame_->color_range;
            metadata.color_space = frame_->colorspace;
            metadata.color_primaries = frame_->color_primaries;
            metadata.color_transfer = frame_->color_trc;
            metadata.chroma_location = frame_->chroma_location;
            metadata.width = frame_->width;
            metadata.height = frame_->height;
            if (const char *name = av_get_pix_fmt_name(
                    static_cast<AVPixelFormat>(frame_->format)); name != nullptr) {
                metadata.pixel_format = name;
            }
            frames_.push_back(std::move(metadata));
            av_frame_unref(frame_.get());
        }
    }

    CodecPtr codec_;
    FramePtr frame_;
    std::vector<DecodedFrameMetadata> frames_;
};

class RapVerifier {
public:
    explicit RapVerifier(const AVCodecParameters &parameters) {
        const AVCodec *decoder = avcodec_find_decoder(parameters.codec_id);
        if (decoder == nullptr) {
            throw std::runtime_error("RAP verification requires a software decoder");
        }
        codec_.reset(avcodec_alloc_context3(decoder));
        if (!codec_) throw std::runtime_error("avcodec_alloc_context3(RAP) failed");
        check_ffmpeg(avcodec_parameters_to_context(codec_.get(), &parameters),
                     "avcodec_parameters_to_context(RAP)");
        codec_->thread_count = 1;
        check_ffmpeg(avcodec_open2(codec_.get(), decoder, nullptr),
                     "avcodec_open2(RAP)");
        frame_.reset(av_frame_alloc());
        packet_.reset(av_packet_alloc());
        if (!frame_ || !packet_) throw std::runtime_error("RAP verification allocation failed");
    }

    [[nodiscard]] bool verify(const std::uint8_t *data, int size,
                              const AVPacket *source) {
        if (data == nullptr || size <= 0) return false;
        avcodec_flush_buffers(codec_.get());
        av_packet_unref(packet_.get());
        if (av_new_packet(packet_.get(), size) < 0) return false;
        std::memcpy(packet_->data, data, static_cast<std::size_t>(size));
        if (source != nullptr) {
            packet_->pts = source->pts;
            packet_->dts = source->dts;
            packet_->pos = source->pos;
            packet_->duration = source->duration;
        }
        int sent = avcodec_send_packet(codec_.get(), packet_.get());
        if (sent < 0) return false;
        bool random_accessible = drain();
        if (!random_accessible && avcodec_send_packet(codec_.get(), nullptr) >= 0) {
            random_accessible = drain();
        }
        avcodec_flush_buffers(codec_.get());
        return random_accessible;
    }

private:
    [[nodiscard]] bool drain() {
        bool random_accessible = false;
        for (;;) {
            const int result = avcodec_receive_frame(codec_.get(), frame_.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) break;
            random_accessible = random_accessible
                || (frame_->pict_type == AV_PICTURE_TYPE_I
                    && (frame_->flags & AV_FRAME_FLAG_KEY) != 0);
            av_frame_unref(frame_.get());
        }
        return random_accessible;
    }

    CodecPtr codec_;
    FramePtr frame_;
    PacketPtr packet_;
};

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
    std::vector<const FrameIdentity *> decode_order;
    decode_order.reserve(frames.size());
    for (const FrameIdentity &frame : frames) decode_order.push_back(&frame);
    std::sort(decode_order.begin(), decode_order.end(),
              [](const FrameIdentity *left, const FrameIdentity *right) {
                  return left->decode_index < right->decode_index;
              });
    std::optional<std::int64_t> previous;
    for (const FrameIdentity *frame : decode_order) {
        if (dts && frame->vp8_invisible_frame) continue;
        const auto value = dts ? frame->dts : frame->file_position;
        if (!value) return false;
        if (previous && *value <= *previous) return false;
        previous = *value;
    }
    return previous.has_value();
}

[[nodiscard]] bool intra_only_codec(AVCodecID codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_RAWVIDEO:
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_PNG:
    case AV_CODEC_ID_FFV1:
    case AV_CODEC_ID_HUFFYUV:
    case AV_CODEC_ID_UTVIDEO:
    case AV_CODEC_ID_PRORES:
    case AV_CODEC_ID_DNXHD:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool parser_identity_is_ambiguous(
    const std::vector<FrameIdentity> &frames) {
    // A repeated presentation timestamp is safe when decode order supplies a
    // distinct DTS.  Without that alternate identity, parser-only indexing
    // cannot prove which packet a decoded frame belongs to.
    std::unordered_map<std::int64_t, std::size_t> timestamp_counts;
    std::unordered_map<std::int64_t, std::size_t> dts_counts;
    timestamp_counts.reserve(frames.size());
    bool has_missing_pts = false;
    for (const FrameIdentity &frame : frames) {
        if (frame.pts) ++timestamp_counts[*frame.pts];
        else has_missing_pts = true;
    }
    if (has_missing_pts) {
        dts_counts.reserve(frames.size());
        for (const FrameIdentity &frame : frames) {
            if (frame.dts) ++dts_counts[*frame.dts];
        }
    }
    std::unordered_map<std::int64_t,
                       std::vector<std::optional<std::int64_t>>> duplicate_groups;
    for (const FrameIdentity &frame : frames) {
        if (!frame.pts) {
            if (frame.dts && dts_counts[*frame.dts] > 1U) return true;
            continue;
        }
        if (timestamp_counts[*frame.pts] < 2U) continue;
        auto &keys = duplicate_groups[*frame.pts];
        if (std::find(keys.begin(), keys.end(), frame.dts) != keys.end()) return true;
        keys.push_back(frame.dts);
    }
    return false;
}

[[nodiscard]] MediaIndex index_media_impl(
    const std::string &path, std::uint32_t stream_index,
    const DecoderOptions *decoder_options, const IndexOptions &index_options,
    std::stop_token stop, IndexProgress progress,
    bool force_metadata_decode = false) {
    const bool strict_metadata_decode = force_metadata_decode
        || index_options.rap_verification;
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
    const std::filesystem::path native_path = path_from_utf8(path);
    result.source_size = std::filesystem::file_size(native_path);
    result.source_mtime_ns = source_mtime_ns(native_path);
    result.format_name = format->iformat != nullptr && format->iformat->name != nullptr
        ? format->iformat->name : "";
    result.format_flags = format->iformat != nullptr ? format->iformat->flags : 0;
    const std::string_view long_format_name = format->iformat != nullptr
            && format->iformat->long_name != nullptr
        ? std::string_view{format->iformat->long_name} : std::string_view{};
    result.raw_demuxer = long_format_name.starts_with("raw")
        || result.format_name == "h264" || result.format_name == "hevc"
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
    result.color_range = parameters.color_range;
    result.color_space = parameters.color_space;
    result.color_primaries = parameters.color_primaries;
    result.color_transfer = parameters.color_trc;
    result.chroma_location = parameters.chroma_location;
    {
        ExtraDataInfo extra;
        extra.codec_id = static_cast<std::uint32_t>(parameters.codec_id);
        extra.fourcc = parameters.codec_tag;
        extra.width = parameters.width;
        extra.height = parameters.height;
        extra.pixel_format = result.pixel_format;
        extra.bit_rate = static_cast<std::uint32_t>(std::max(
            parameters.bits_per_coded_sample, parameters.bits_per_raw_sample));
        if (parameters.extradata != nullptr && parameters.extradata_size > 0) {
        extra.data.assign(parameters.extradata,
                          parameters.extradata + parameters.extradata_size);
        }
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
    } else if (parameters.codec_id == AV_CODEC_ID_HEVC && !result.raw_demuxer
               && parameters.extradata_size > 0) {
        bitstream_filter_name = "hevc_mp4toannexb";
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
    std::uint32_t current_extradata_index = result.extradata.front().index;
    std::unique_ptr<IndexMetadataDecoder> metadata_decoder;
    if (strict_metadata_decode) {
        metadata_decoder = std::make_unique<IndexMetadataDecoder>(parameters);
    }
    std::unique_ptr<RapVerifier> rap_verifier;
    if (index_options.rap_verification) {
        rap_verifier = std::make_unique<RapVerifier>(parameters);
    }
    auto append_record = [&](const AVPacket *source, const std::uint8_t *data,
                             int size, bool flushed) {
        FrameIdentity identity;
        identity.decode_index = result.frames.size();
        const std::int64_t packet_pts = source != nullptr ? source->pts : AV_NOPTS_VALUE;
        const std::int64_t packet_dts = source != nullptr ? source->dts : AV_NOPTS_VALUE;
        const std::int64_t packet_pos = source != nullptr ? source->pos : -1;
        const bool demuxer_key = source != nullptr
            && (source->flags & AV_PKT_FLAG_KEY) != 0;
        identity.key_frame = demuxer_key;
        if (parser) {
            identity.pts = timestamp_value(parser->pts);
            identity.dts = timestamp_value(parser->dts);
            identity.file_position = parser->pos >= 0 ? std::optional{parser->pos}
                                                       : (packet_pos >= 0 ? std::optional{packet_pos} : std::nullopt);
            identity.picture_type = parser_picture_name(parser->pict_type);
            // Negative POC values identify open-GOP leading pictures and are
            // required when timestamps must be reconstructed.
            identity.poc = parser->output_picture_number;
            identity.repeat_pict = parser->repeat_pict;
            identity.field_order = parser_field_order(parser->field_order);
            if (identity.picture_type
                && identity.picture_type != std::optional<std::string>{"I"}) {
                identity.key_frame = false;
            } else if (parser->key_frame == 1) {
                identity.key_frame = true;
            }
        }
        if (!identity.pts) identity.pts = timestamp_value(packet_pts);
        if (!identity.dts) identity.dts = timestamp_value(packet_dts);
        if (!identity.file_position && packet_pos >= 0) identity.file_position = packet_pos;
        identity.packet_size = static_cast<std::uint32_t>(std::max(0, size));
        if (source != nullptr && source->duration > 0) {
            identity.packet_duration = source->duration;
        }
        identity.extradata_index = current_extradata_index;
        identity.color_range = result.color_range;
        identity.color_space = result.color_space;
        identity.color_primaries = result.color_primaries;
        identity.color_transfer = result.color_transfer;
        identity.chroma_location = result.chroma_location;
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
        const bool parser_i = identity.picture_type
            == std::optional<std::string>{"I"};
        if (rap_verifier) {
            const bool candidate = parser_i
                || (!parser && (demuxer_key || intra_only_codec(parameters.codec_id)));
            identity.rap = candidate
                && rap_verifier->verify(data, size, source);
        } else {
            identity.rap = identity.key_frame
                || (intra_only_codec(parameters.codec_id) && !parser);
        }
        if (identity.rap) identity.key_frame = true;
        if (flushed && identity.packet_size == 0U) identity.packet_size = 0U;
        if (metadata_decoder) metadata_decoder->feed(data, size, source, identity);
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
                extra.fourcc = parameters.codec_tag;
                extra.width = parameters.width;
                extra.height = parameters.height;
                extra.pixel_format = result.pixel_format;
                extra.bit_rate = static_cast<std::uint32_t>(std::max(
                    parameters.bits_per_coded_sample, parameters.bits_per_raw_sample));
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

    // Ordinary indexing follows L-SMASH Works: packet/parser identities are
    // sufficient for the common case and avoid a full software decode.  A
    // strict metadata pass is retained for streams whose packet identity
    // cannot be proven from parser output alone.
    const bool parser_available = parser != nullptr && parser_codec != nullptr;
    const bool requires_strict_index = !strict_metadata_decode
        && (!parser_available || result.extradata.size() > 1U
            || parser_identity_is_ambiguous(result.frames));
    if (requires_strict_index) {
        return index_media_impl(path, stream_index, decoder_options, index_options,
                                stop, std::move(progress), true);
    }

    // Join decoded presentation metadata back to packet identities. PTS is
    // the strongest key; pkt_dts disambiguates missing or duplicated PTS.
    // Ambiguous joins are deliberately left unmatched rather than assigning
    // metadata to the wrong indexed frame.
    std::vector<char> metadata_frame_used(result.frames.size(), 0);
    std::vector<std::size_t> decoded_presentation_order;
    decoded_presentation_order.reserve(result.frames.size());
    if (metadata_decoder) metadata_decoder->finish();
    if (metadata_decoder) for (const DecodedFrameMetadata &metadata : metadata_decoder->frames()) {
        std::size_t best = result.frames.size();
        int best_score = 0;
        bool ambiguous = false;
        if (metadata.decode_index
            && *metadata.decode_index < result.frames.size()
            && !metadata_frame_used[*metadata.decode_index]
            && result.frames[*metadata.decode_index].decode_index
                == *metadata.decode_index) {
            best = static_cast<std::size_t>(*metadata.decode_index);
            best_score = std::numeric_limits<int>::max();
        } else {
            for (std::size_t candidate = 0U; candidate < result.frames.size(); ++candidate) {
                if (metadata_frame_used[candidate]
                    || result.frames[candidate].vp8_invisible_frame) {
                    continue;
                }
                int score = 0;
                if (metadata.pts && result.frames[candidate].pts
                    && *metadata.pts == *result.frames[candidate].pts) score += 8;
                if (metadata.best_effort_timestamp && result.frames[candidate].pts
                    && *metadata.best_effort_timestamp == *result.frames[candidate].pts) score += 6;
                if (metadata.packet_dts && result.frames[candidate].dts
                    && *metadata.packet_dts == *result.frames[candidate].dts) score += 4;
                if (score > best_score) {
                    best = candidate;
                    best_score = score;
                    ambiguous = false;
                } else if (score > 0 && score == best_score) {
                    ambiguous = true;
                }
            }
        }
        if (best == result.frames.size() || best_score == 0 || ambiguous) continue;
        FrameIdentity &identity = result.frames[best];
        metadata_frame_used[best] = 1;
        decoded_presentation_order.push_back(best);
        identity.best_effort_timestamp = metadata.best_effort_timestamp;
        if (!identity.packet_duration) identity.packet_duration = metadata.duration;
        if (metadata.repeat_pict != 0) identity.repeat_pict = metadata.repeat_pict;
        if (metadata.field_order != "unknown") identity.field_order = metadata.field_order;
        identity.color_range = metadata.color_range;
        identity.color_space = metadata.color_space;
        identity.color_primaries = metadata.color_primaries;
        identity.color_transfer = metadata.color_transfer;
        identity.chroma_location = metadata.chroma_location;
        const auto extra = std::find_if(
            result.extradata.begin(), result.extradata.end(),
            [&](const ExtraDataInfo &entry) {
                return entry.index == identity.extradata_index;
            });
        if (extra != result.extradata.end()) {
            extra->width = metadata.width;
            extra->height = metadata.height;
            if (!metadata.pixel_format.empty()) extra->pixel_format = metadata.pixel_format;
        }
    }

    bool unique_pts = true;
    std::unordered_set<std::int64_t> seen_pts;
    seen_pts.reserve(result.frames.size());
    for (const FrameIdentity &frame : result.frames) {
        if (!frame.pts || !seen_pts.insert(*frame.pts).second) {
            unique_pts = false;
            break;
        }
    }
    std::vector<std::size_t> order(result.frames.size());
    std::iota(order.begin(), order.end(), 0U);
    const auto presentation_timestamp = [](const FrameIdentity &frame)
        -> std::optional<std::int64_t> {
        if (frame.pts) return frame.pts;
        if (frame.best_effort_timestamp) return frame.best_effort_timestamp;
        return frame.dts;
    };
    const bool all_timestamped = std::all_of(
        result.frames.begin(), result.frames.end(),
        [&](const FrameIdentity &frame) {
            return frame.vp8_invisible_frame || presentation_timestamp(frame).has_value();
        });
    const std::size_t visible_frame_count = static_cast<std::size_t>(std::count_if(
        result.frames.begin(), result.frames.end(),
        [](const FrameIdentity &frame) { return !frame.vp8_invisible_frame; }));
    if (decoded_presentation_order.size() == visible_frame_count) {
        order = std::move(decoded_presentation_order);
        // Invisible VP8 packets produce no AVFrame. Keep them adjacent to the
        // next access unit in decode order without disturbing the decoded
        // presentation order of visible frames.
        for (std::size_t decode = 0U; decode < result.frames.size(); ++decode) {
            if (!result.frames[decode].vp8_invisible_frame) continue;
            const auto insertion = std::find_if(
                order.begin(), order.end(),
                [decode](std::size_t candidate) { return candidate > decode; });
            order.insert(insertion, decode);
        }
    } else if (all_timestamped) {
        std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
            const auto left_timestamp = presentation_timestamp(result.frames[left]);
            const auto right_timestamp = presentation_timestamp(result.frames[right]);
            if (!left_timestamp) return false;
            if (!right_timestamp) return true;
            if (*left_timestamp != *right_timestamp) {
                return *left_timestamp < *right_timestamp;
            }
            return result.frames[left].decode_index < result.frames[right].decode_index;
        });
    } else {
        // Preserve the slots of completely timestamp-less frames, but still
        // put every known timestamp into presentation order. This avoids the
        // previous all-or-nothing behavior where one missing PTS discarded
        // valid timing information for the entire stream.
        std::vector<std::size_t> timestamp_slots;
        std::vector<std::size_t> timestamped_frames;
        for (std::size_t decode = 0U; decode < result.frames.size(); ++decode) {
            if (!presentation_timestamp(result.frames[decode])) continue;
            timestamp_slots.push_back(decode);
            timestamped_frames.push_back(decode);
        }
        std::stable_sort(
            timestamped_frames.begin(), timestamped_frames.end(),
            [&](std::size_t left, std::size_t right) {
                const std::int64_t left_timestamp =
                    *presentation_timestamp(result.frames[left]);
                const std::int64_t right_timestamp =
                    *presentation_timestamp(result.frames[right]);
                return left_timestamp != right_timestamp
                    ? left_timestamp < right_timestamp : left < right;
            });
        for (std::size_t slot = 0U; slot < timestamp_slots.size(); ++slot) {
            order[timestamp_slots[slot]] = timestamped_frames[slot];
        }
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
        if (const auto timestamp = presentation_timestamp(frame);
            timestamp && result.time_base_den != 0) {
            frame.timestamp_seconds = static_cast<double>(*timestamp)
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
    result.index_mode = strict_metadata_decode ? "packet_rebuilt" : "packet_fast";
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
        ensure_presentation_match(source, selected_frames[selected_index]);
        if (!scaler) {
            scaler.reset(sws_getContext(source.width, source.height,
                static_cast<AVPixelFormat>(source.format), source.width, source.height,
                AV_PIX_FMT_GRAYF32LE, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!scaler) throw std::runtime_error("sws_getContext(grayf32le) failed");
            configure_scaler_colorspace(*scaler, source);
        }
        HostFrame output;
        output.seq = selected_index;
        output.identity = selected_frames[selected_index];
        update_identity_metadata(output.identity, source);
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
            send_packet_tolerant(*opened.codec, packet.get(), stop, telemetry,
                                 "avcodec_send_packet", process);
            receive_frames(*opened.codec, stop, process);
        }
        av_packet_unref(packet.get());
    }
    send_packet_tolerant(*opened.codec, nullptr, stop, telemetry,
                         "avcodec_send_packet(flush)", process);
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

// ---------------------------------------------------------------------------
// IndexedDecodeSession
// ---------------------------------------------------------------------------
//
// Holds one open demuxer+decoder pair per indexed source and keeps it alive
// across preview requests. A request whose first target still lies ahead of
// the decoder inside the same GOP continues decoding without any seek;
// anything else pays only avformat_seek_file + avcodec_flush_buffers instead
// of a full reopen + reprobe.

struct IndexedDecodeSession::Impl {
    Impl(const std::string &path, std::shared_ptr<const MediaIndex> index,
         const DecoderOptions &options)
        : path_(path), index_(std::move(index)), options_(options) {
        if (index_ == nullptr || index_->frames.empty()) {
            throw std::invalid_argument("indexed decode session requires a media index");
        }
        identity_lookup_ = std::make_unique<IndexedIdentityLookup>(*index_);
        opened_ = open_decoder(path_, index_->stream_index, &options_, index_.get());
        packet_.reset(av_packet_alloc());
        transfer_frame_.reset(av_frame_alloc());
        if (!packet_ || !transfer_frame_) {
            throw std::runtime_error("FFmpeg frame allocation failed");
        }
    }

    DecoderOptions::Backend backend() const { return options_.backend; }

    void decode(std::span<const FrameIdentity> selected_frames, std::stop_token stop,
                const FrameConsumer &consumer, DecodeTelemetry *telemetry) {
        try {
            decode_inner(selected_frames, stop, consumer, telemetry);
        } catch (...) {
            // A cancelled or failed decode leaves the demuxer/decoder at an
            // unknown point of the GOP; the next call must reposition instead
            // of trusting the continuation cursor.
            needs_reposition_ = true;
            throw;
        }
    }

private:
    void configure_decoder(std::uint32_t extradata_index) {
        if (current_extradata_index_ == extradata_index) {
            avcodec_flush_buffers(opened_.codec.get());
            return;
        }
        const auto configuration = std::find_if(
            index_->extradata.begin(), index_->extradata.end(),
            [extradata_index](const ExtraDataInfo &entry) {
                return entry.index == extradata_index;
            });
        if (configuration == index_->extradata.end()) {
            avcodec_flush_buffers(opened_.codec.get());
            current_extradata_index_ = extradata_index;
            return;
        }
        BuiltDecoder built = build_decoder(*opened_.stream, &options_, &*configuration);
        opened_.codec = std::move(built.codec);
        opened_.decoder = built.decoder;
        current_extradata_index_ = extradata_index;
        gray_scaler_.reset();
        rgb_scaler_.reset();
        scaler_width_ = 0;
        scaler_height_ = 0;
        scaler_format_ = -1;
    }

    void reposition(std::uint64_t anchor) {
        bool positioned = false;
        if (anchor < index_->frames.size() && index_->frames[anchor].rap
            && seek_to_keyframe(*opened_.format, *index_, index_->frames[anchor], true)) {
            positioned = true;
        }
        if (!positioned) {
            // A stream cut before its first RAP (or a demuxer without a usable
            // anchor) requires decoding from the start of the file. Ordinary
            // open-GOP leading pictures are decoded from their following RAP.
            if (opened_.format->pb != nullptr
                && avio_seek(opened_.format->pb, 0, SEEK_SET) >= 0) {
                avformat_flush(opened_.format.get());
            } else {
                (void)avformat_seek_file(opened_.format.get(), -1, 0, 0, 0,
                                         AVSEEK_FLAG_BACKWARD);
            }
        }
        const std::uint32_t extradata_index = anchor < index_->frames.size()
            ? index_->frames[anchor].extradata_index : 0U;
        configure_decoder(extradata_index);
        frame_cursor_ = positioned ? anchor : 0U;
        decode_start_anchor_ = positioned ? anchor : 0U;
        run_anchor_ = anchor;
        run_valid_ = true;
        run_drained_ = false;
        needs_reposition_ = false;
        last_delivered_pts_.reset();
        last_delivered_frame_index_.reset();
    }

    void deliver(AVFrame &source, const FrameIdentity &identity, std::uint64_t seq,
                 const FrameConsumer &consumer, DecodeTelemetry *telemetry,
                 double &consumer_ms) {
        const AVFrame *cpu = &source;
        if (source.hw_frames_ctx != nullptr) {
            // Hardware decoder surface: download to the software pixel format
            // recorded in the frames context (NV12/P010/...), then convert.
            av_frame_unref(transfer_frame_.get());
            check_ffmpeg(av_hwframe_transfer_data(transfer_frame_.get(), &source, 0),
                         "av_hwframe_transfer_data(indexed)");
            check_ffmpeg(av_frame_copy_props(transfer_frame_.get(), &source),
                         "av_frame_copy_props(indexed)");
            cpu = transfer_frame_.get();
        }
        if (cpu->width != scaler_width_ || cpu->height != scaler_height_
            || cpu->format != scaler_format_) {
            gray_scaler_.reset();
            rgb_scaler_.reset();
            scaler_width_ = cpu->width;
            scaler_height_ = cpu->height;
            scaler_format_ = cpu->format;
        }
        HostFrame output;
        output.seq = seq;
        output.identity = identity;
        update_identity_metadata(output.identity, *cpu);
        output.width = cpu->width;
        output.height = cpu->height;
        const auto convert_start = Clock::now();
        if (options_.output_luma) {
            if (!gray_scaler_) {
                gray_scaler_.reset(sws_getContext(
                    cpu->width, cpu->height, static_cast<AVPixelFormat>(cpu->format),
                    cpu->width, cpu->height, AV_PIX_FMT_GRAYF32LE, SWS_BILINEAR,
                    nullptr, nullptr, nullptr));
                if (!gray_scaler_) {
                    throw std::runtime_error("sws_getContext(grayf32le) failed");
                }
            }
            configure_scaler_colorspace(*gray_scaler_, *cpu);
            output.pixels.resize(static_cast<std::size_t>(cpu->width)
                                 * static_cast<std::size_t>(cpu->height));
            std::uint8_t *planes[4] = {
                reinterpret_cast<std::uint8_t *>(output.pixels.data()),
                nullptr, nullptr, nullptr};
            int lines[4] = {cpu->width * static_cast<int>(sizeof(float)), 0, 0, 0};
            check_ffmpeg(sws_scale(gray_scaler_.get(), cpu->data, cpu->linesize, 0,
                                   cpu->height, planes, lines),
                         "sws_scale(grayf32le)");
        }
        if (options_.output_rgb) {
            std::int32_t rgb_width = cpu->width;
            std::int32_t rgb_height = cpu->height;
            if (!options_.output_luma && options_.preview_maximum_dimension > 0) {
                const double scale = std::min(
                    1.0, static_cast<double>(options_.preview_maximum_dimension)
                        / static_cast<double>(std::max(cpu->width, cpu->height)));
                rgb_width = std::max(
                    1, static_cast<std::int32_t>(std::llround(cpu->width * scale)));
                rgb_height = std::max(
                    1, static_cast<std::int32_t>(std::llround(cpu->height * scale)));
                output.width = rgb_width;
                output.height = rgb_height;
            }
            if (!rgb_scaler_) {
                rgb_scaler_.reset(sws_getContext(
                    cpu->width, cpu->height, static_cast<AVPixelFormat>(cpu->format),
                    rgb_width, rgb_height, AV_PIX_FMT_RGB24, SWS_BILINEAR,
                    nullptr, nullptr, nullptr));
                if (!rgb_scaler_) throw std::runtime_error("sws_getContext(rgb24) failed");
            }
            configure_scaler_colorspace(*rgb_scaler_, *cpu);
            output.rgb24.resize(static_cast<std::size_t>(rgb_width)
                                * static_cast<std::size_t>(rgb_height) * 3U);
            std::uint8_t *rgb_planes[4] = {output.rgb24.data(), nullptr, nullptr, nullptr};
            int rgb_lines[4] = {rgb_width * 3, 0, 0, 0};
            check_ffmpeg(sws_scale(rgb_scaler_.get(), cpu->data, cpu->linesize, 0,
                                   cpu->height, rgb_planes, rgb_lines),
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
        consumer_ms +=
            std::chrono::duration<double, std::milli>(Clock::now() - consumer_start).count();
    }

    template <class Callback, class Done>
    void send_and_receive(AVPacket *packet, std::stop_token stop, DecodeTelemetry *telemetry,
                          Callback &&process, Done &&done) {
        for (;;) {
            const int sent = avcodec_send_packet(opened_.codec.get(), packet);
            if (sent == AVERROR_INVALIDDATA) {
                if (telemetry != nullptr)
                    ++telemetry->discarded_packets;
                return;
            }
            if (sent == AVERROR(EAGAIN)) {
                receive_frames_until(*opened_.codec, stop, process, done);
                if (done()) {
                    // The request completed while making room for this packet.
                    // Do not spin if the threaded decoder still needs more
                    // output drained before it can accept the packet. Dropping
                    // it invalidates continuation only; the next request seeks.
                    needs_reposition_ = true;
                    return;
                }
                continue;
            }
            check_ffmpeg(sent, "avcodec_send_packet(indexed)");
            break;
        }
        receive_frames_until(*opened_.codec, stop, process, done);
    }

    void decode_inner(std::span<const FrameIdentity> selected_frames, std::stop_token stop,
                      const FrameConsumer &consumer, DecodeTelemetry *telemetry) {
        if (selected_frames.empty())
            return;
        const std::vector<IndexedDecodeRun> runs =
            plan_indexed_decode_runs(*index_, selected_frames);

        const auto start = Clock::now();
        double consumer_ms = 0.0;
        for (const IndexedDecodeRun &run : runs) {
            if (stop.stop_requested())
                throw std::runtime_error("cancelled");
            const std::size_t selected_begin = run.selected_begin;
            const std::size_t selected_end = run.selected_end;
            const std::uint64_t anchor = run.anchor;
            const std::uint64_t last_target = selected_frames[selected_end - 1U].frame_index;
            const bool leading_target = run.decode_anchor != anchor;
            // Presentation order can place an open-GOP leading picture before
            // the RAP that decodes it. Seek to the preceding RAP in that case;
            // seeking to `anchor` would make the leading picture unreachable.
            std::uint64_t decode_anchor = run.decode_anchor;
            constexpr std::size_t maximum_decode_attempts = 3U;
            for (std::size_t attempt = 0U;; ++attempt) {
                const std::uint64_t selected_before =
                    telemetry != nullptr ? telemetry->selected_frames : 0U;
                std::vector<HostFrame> pending_outputs;
                if (!run.streaming) pending_outputs.reserve(selected_end - selected_begin);
                const FrameConsumer buffered_output = [&](HostFrame &&frame) {
                    pending_outputs.push_back(std::move(frame));
                };
                const FrameConsumer output_consumer = run.streaming
                    ? consumer : buffered_output;

                // Reuse the in-flight GOP decode only on the first attempt and
                // when every target still lies ahead of what the decoder
                // produced.
                const bool can_continue =
                    attempt == 0U && !leading_target && !needs_reposition_ && run_valid_ &&
                    !run_drained_ && run_anchor_ == anchor &&
                    last_delivered_frame_index_.has_value()
                        && selected_frames[selected_begin].frame_index
                            > *last_delivered_frame_index_;
                if (!can_continue)
                    reposition(decode_anchor);

                // Byte position where this GOP's packets end (the next
                // random-access point in the index); reading stops there and the
                // decoder drains.
                const std::optional<std::int64_t> gop_end_position = run.end_position;
                std::vector<char> delivered(selected_end - selected_begin, 0);
                const std::uint64_t minimum_decode_index =
                    decode_start_anchor_ < index_->frames.size()
                        ? index_->frames[decode_start_anchor_].decode_index : 0U;
                const IndexedFrameMatcher matcher{
                    *identity_lookup_,
                    selected_frames.subspan(selected_begin, selected_end - selected_begin),
                    minimum_decode_index, run.end_decode_index};
                std::size_t delivered_count = 0U;
                bool finished = false;
                bool emitted_output = false;
                auto process = [&](AVFrame &source) {
                    if (telemetry)
                        ++telemetry->decoded_frames;
                    const IndexedFrameMatch matched = matcher.match(
                        source, delivered, true, frame_cursor_);
                    if (matched.frame_index) {
                        frame_cursor_ = *matched.frame_index + 1U;
                    } else {
                        ++frame_cursor_;
                    }
                    if (!matched.selected_target) {
                        const bool timestamped = source.pts != AV_NOPTS_VALUE
                            || source.best_effort_timestamp != AV_NOPTS_VALUE
                            || source.pkt_dts != AV_NOPTS_VALUE;
                        finished = !timestamped && frame_cursor_ > last_target;
                        return;
                    }
                    const std::size_t matching_target =
                        selected_begin + *matched.selected_target;
                    delivered[matching_target - selected_begin] = 1;
                    ++delivered_count;
                    emitted_output = true;
                    deliver(source, selected_frames[matching_target], matching_target,
                            output_consumer, telemetry, consumer_ms);
                    last_delivered_frame_index_ = selected_frames[matching_target].frame_index;
                    if (source.pts != AV_NOPTS_VALUE
                        && (!last_delivered_pts_ || source.pts > *last_delivered_pts_)) {
                        last_delivered_pts_ = source.pts;
                    }
                    finished = delivered_count == selected_end - selected_begin;
                };
                while (!finished && av_read_frame(opened_.format.get(), packet_.get()) >= 0) {
                    try {
                        if (stop.stop_requested())
                            throw std::runtime_error("cancelled");
                        if (packet_->stream_index == static_cast<int>(index_->stream_index)) {
                            if (gop_end_position && packet_->pos >= *gop_end_position) {
                                av_packet_unref(packet_.get());
                                break;
                            }
                            send_and_receive(packet_.get(), stop, telemetry, process,
                                             [&finished] { return finished; });
                        }
                    } catch (...) {
                        // The session survives cancellation; keep the packet blank
                        // for the next av_read_frame.
                        av_packet_unref(packet_.get());
                        throw;
                    }
                    av_packet_unref(packet_.get());
                }
                if (!finished) {
                    send_and_receive(nullptr, stop, telemetry, process,
                                     [&finished] { return finished; });
                    run_drained_ = true;
                }
                const bool segment_complete =
                    delivered_count == selected_end - selected_begin;
                if (segment_complete) {
                    if (!run.streaming) {
                        std::sort(pending_outputs.begin(), pending_outputs.end(),
                                  [](const HostFrame &left, const HostFrame &right) {
                                      return left.seq < right.seq;
                                  });
                        for (HostFrame &output : pending_outputs) {
                            const auto consumer_start = Clock::now();
                            consumer(std::move(output));
                            consumer_ms += std::chrono::duration<double, std::milli>(
                                Clock::now() - consumer_start).count();
                        }
                    }
                    break;
                }
                if (run.streaming && emitted_output) {
                    throw std::runtime_error(
                        "decoder ended before all indexed dense-run frames were produced");
                }
                if (telemetry != nullptr) {
                    telemetry->selected_frames = selected_before;
                }
                const std::optional<std::uint64_t> earlier =
                    preceding_rap(*index_, decode_anchor);
                if (attempt + 1U >= maximum_decode_attempts || !earlier) {
                    throw std::runtime_error(
                        "decoder ended before all indexed GOP frames were produced");
                }
                decode_anchor = *earlier;
                if (telemetry != nullptr)
                    ++telemetry->decode_retries;
                needs_reposition_ = true;
            }
            run_valid_ = true;
            run_anchor_ = anchor;
        }
        if (telemetry) {
            telemetry->decode_ms = std::max(
                0.0, std::chrono::duration<double, std::milli>(Clock::now() - start).count() -
                         telemetry->convert_ms - consumer_ms);
        }
    }

    std::string path_;
    std::shared_ptr<const MediaIndex> index_;
    DecoderOptions options_;
    std::unique_ptr<IndexedIdentityLookup> identity_lookup_;
    OpenedDecoder opened_;
    PacketPtr packet_;
    FramePtr transfer_frame_;
    SwsPtr gray_scaler_;
    SwsPtr rgb_scaler_;
    std::int32_t scaler_width_ = 0;
    std::int32_t scaler_height_ = 0;
    std::int32_t scaler_format_ = -1;
    // Continuation state for the GOP currently flowing through the decoder.
    std::uint64_t run_anchor_ = 0U;
    bool run_valid_ = false;
    bool run_drained_ = false;
    bool needs_reposition_ = true;
    std::uint64_t frame_cursor_ = 0U;
    std::uint64_t decode_start_anchor_ = 0U;
    std::optional<std::int64_t> last_delivered_pts_;
    std::optional<std::uint64_t> last_delivered_frame_index_;
    std::optional<std::uint32_t> current_extradata_index_;
};

IndexedDecodeSession::IndexedDecodeSession(const std::string &path,
                                           std::shared_ptr<const MediaIndex> index,
                                           const DecoderOptions &options)
    : impl_(std::make_unique<Impl>(path, std::move(index), options)) {}

IndexedDecodeSession::~IndexedDecodeSession() = default;

DecoderOptions::Backend IndexedDecodeSession::backend() const {
    return impl_->backend();
}

void IndexedDecodeSession::decode(std::span<const FrameIdentity> selected,
                                  std::stop_token stop, const FrameConsumer &consumer,
                                  DecodeTelemetry *telemetry) {
    impl_->decode(selected, stop, consumer, telemetry);
}

void decode_selected_indexed(const std::string &path, const MediaIndex &index,
                             std::span<const FrameIdentity> selected_frames,
                             const DecoderOptions &options, std::stop_token stop,
                             const FrameConsumer &consumer,
                             DecodeTelemetry *telemetry) {
    if (selected_frames.empty()) return;
    // One-shot callers get a session that is discarded after the request;
    // the worker keeps its own session alive across preview requests.
    IndexedDecodeSession session{
        path, std::shared_ptr<const MediaIndex>{&index, [](const MediaIndex *) {}}, options};
    session.decode(selected_frames, stop, consumer, telemetry);
}

std::int64_t source_mtime_unix_ns(const std::string &path) {
    return source_mtime_ns(path_from_utf8(path));
}

PreviewImage encode_preview_png(const HostFrame &source,
                                std::int32_t maximum_dimension) {
    const bool color = source.rgb24.size()
        == static_cast<std::size_t>(source.width)
            * static_cast<std::size_t>(source.height) * 3U;
    // Color frames encode from rgb24 alone; luma pixels are optional so
    // PNG-only decodes can skip producing the full-resolution float copy.
    if (source.width <= 0 || source.height <= 0
        || (!color && source.pixels.size() != static_cast<std::size_t>(source.width)
                                              * static_cast<std::size_t>(source.height))) {
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

    const AVPixelFormat source_format = color
        ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAYF32LE;
    const AVPixelFormat output_format = color ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAY8;
    struct PreviewEncoderState {
        CodecPtr codec;
        FramePtr frame;
        PacketPtr packet;
        SwsPtr scaler;
        std::int32_t width = 0;
        std::int32_t height = 0;
        AVPixelFormat output_format = AV_PIX_FMT_NONE;
        std::int32_t source_width = 0;
        std::int32_t source_height = 0;
        AVPixelFormat source_format = AV_PIX_FMT_NONE;
        std::int64_t next_pts = 0;
    };
    static thread_local PreviewEncoderState state;
    if (!state.codec || state.width != width || state.height != height
        || state.output_format != output_format) {
        const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
        if (encoder == nullptr) {
            throw std::runtime_error("FFmpeg PNG encoder is unavailable");
        }
        CodecPtr codec{avcodec_alloc_context3(encoder)};
        FramePtr frame{av_frame_alloc()};
        PacketPtr packet{av_packet_alloc()};
        if (!codec || !frame || !packet) {
            throw std::runtime_error("PNG encoder allocation failed");
        }
        codec->width = width;
        codec->height = height;
        codec->pix_fmt = output_format;
        codec->time_base = AVRational{1, 1};
        // Preview PNGs are transient cache files: level 1 deflate is several
        // times faster than the default while staying well under 1.5x the size.
        AVDictionary *encoder_options = nullptr;
        av_dict_set(&encoder_options, "compression_level", "1", 0);
        const int open_result = avcodec_open2(codec.get(), encoder, &encoder_options);
        av_dict_free(&encoder_options);
        check_ffmpeg(open_result, "avcodec_open2(png)");
        frame->format = output_format;
        frame->width = width;
        frame->height = height;
        check_ffmpeg(av_frame_get_buffer(frame.get(), 32), "av_frame_get_buffer(png)");
        state.codec = std::move(codec);
        state.frame = std::move(frame);
        state.packet = std::move(packet);
        state.scaler.reset();
        state.width = width;
        state.height = height;
        state.output_format = output_format;
        state.source_width = 0;
        state.source_height = 0;
        state.source_format = AV_PIX_FMT_NONE;
        state.next_pts = 0;
    }
    check_ffmpeg(av_frame_make_writable(state.frame.get()),
                 "av_frame_make_writable(png)");
    if (color && source.width == width && source.height == height) {
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 3U;
        for (std::int32_t row = 0; row < height; ++row) {
            std::memcpy(state.frame->data[0] + row * state.frame->linesize[0],
                        source.rgb24.data() + static_cast<std::size_t>(row) * row_bytes,
                        row_bytes);
        }
    } else {
        if (!state.scaler || state.source_width != source.width
            || state.source_height != source.height
            || state.source_format != source_format) {
            state.scaler.reset(sws_getContext(
                source.width, source.height, source_format,
                width, height, output_format, SWS_BILINEAR,
                nullptr, nullptr, nullptr));
            if (!state.scaler) {
                throw std::runtime_error("sws_getContext(preview) failed");
            }
            state.source_width = source.width;
            state.source_height = source.height;
            state.source_format = source_format;
        }
        const std::uint8_t *source_planes[4] = {
            color ? source.rgb24.data()
                  : reinterpret_cast<const std::uint8_t *>(source.pixels.data()),
            nullptr, nullptr, nullptr};
        const int source_lines[4] = {
            color ? source.width * 3
                  : source.width * static_cast<int>(sizeof(float)), 0, 0, 0};
        check_ffmpeg(sws_scale(state.scaler.get(), source_planes, source_lines, 0,
                               source.height, state.frame->data,
                               state.frame->linesize),
                     "sws_scale(preview)");
    }
    state.frame->pts = state.next_pts++;
    av_packet_unref(state.packet.get());
    check_ffmpeg(avcodec_send_frame(state.codec.get(), state.frame.get()),
                 "avcodec_send_frame(png)");
    check_ffmpeg(avcodec_receive_packet(state.codec.get(), state.packet.get()),
                 "avcodec_receive_packet(png)");
    PreviewImage result;
    result.width = width;
    result.height = height;
    result.png.assign(state.packet->data,
                      state.packet->data + state.packet->size);
    return result;
}

template <class Deliver>
void decode_selected_hardware_indexed(
    const std::string &path, const MediaIndex &index,
    std::span<const FrameIdentity> selected_frames,
    const DecoderOptions &options, std::stop_token stop,
    std::string_view backend_name, Deliver &&deliver,
    DecodeTelemetry *telemetry) {
    if (selected_frames.empty()) return;
    const auto start = Clock::now();
    const std::size_t requested_sessions = std::clamp<std::size_t>(
        options.hardware_decode_sessions, 1U, 4U);
    const std::vector<IndexedDecodeRun> runs = plan_indexed_decode_runs(
        index, selected_frames, requested_sessions > 1U);
    const IndexedIdentityLookup identity_lookup{index};
    // The default remains one persistent hardware decoder. The experimental
    // multi-session path partitions whole RAP-aligned runs so no decoder
    // depends on reference state owned by another session.
    const std::size_t worker_count = std::min(requested_sessions, runs.size());
    std::vector<DecodeTelemetry> worker_telemetry(worker_count);
    std::vector<double> worker_consumer_ms(worker_count, 0.0);
    std::mutex failure_mutex;
    std::atomic_bool failed{false};
    std::exception_ptr failure;

    const auto decode_slice = [&](std::size_t worker_index) {
        try {
            const std::size_t run_begin = worker_index * runs.size() / worker_count;
            const std::size_t run_end = (worker_index + 1U) * runs.size() / worker_count;
            const ExtraDataInfo *initial_configuration = indexed_decoder_configuration(
                index, runs[run_begin].decode_anchor);
            OpenedDecoder opened = open_decoder(
                path, index.stream_index, &options, &index, initial_configuration);
            PacketPtr packet{av_packet_alloc()};
            if (!packet) throw std::runtime_error("FFmpeg packet allocation failed");
            std::optional<std::uint32_t> current_extradata_index =
                runs[run_begin].decode_anchor < index.frames.size()
                    ? std::optional{index.frames[runs[run_begin].decode_anchor]
                                        .extradata_index}
                    : std::nullopt;
            DecodeTelemetry &local = worker_telemetry[worker_index];
            double &consumer_ms = worker_consumer_ms[worker_index];
            constexpr std::size_t maximum_decode_attempts = 3U;

            const auto configure_decoder = [&](std::uint64_t decode_anchor,
                                               bool force_flush) {
                const std::optional<std::uint32_t> required_extradata =
                    decode_anchor < index.frames.size()
                        ? std::optional{index.frames[decode_anchor].extradata_index}
                        : std::nullopt;
                if (current_extradata_index == required_extradata) {
                    if (force_flush) avcodec_flush_buffers(opened.codec.get());
                    return;
                }
                BuiltDecoder built = build_decoder(
                    *opened.stream, &options,
                    indexed_decoder_configuration(index, decode_anchor));
                opened.codec = std::move(built.codec);
                opened.decoder = built.decoder;
                current_extradata_index = required_extradata;
            };

            for (std::size_t run_index = run_begin; run_index < run_end; ++run_index) {
                if (stop.stop_requested()) throw std::runtime_error("cancelled");
                if (failed.load(std::memory_order_relaxed)) return;
                const IndexedDecodeRun &run = runs[run_index];
                std::uint64_t decode_anchor = run.decode_anchor;
                for (std::size_t attempt = 0U;; ++attempt) {
                    // Every normal run starts at an indexed RAP. Feeding the next
                    // RAP resets codec reference state without avcodec_flush_buffers(),
                    // which would tear down and recreate FFmpeg's H.264/HEVC
                    // hardware context for every selected I-picture. A retry can
                    // start before the requested RAP, so reset explicitly there.
                    configure_decoder(decode_anchor, attempt != 0U);
                    std::uint64_t presentation_cursor = 0U;
                    if (decode_anchor < index.frames.size()
                        && seek_to_keyframe(*opened.format, index,
                                            index.frames[decode_anchor], true)) {
                        presentation_cursor = decode_anchor;
                    } else if (opened.format->pb != nullptr
                               && avio_seek(opened.format->pb, 0, SEEK_SET) >= 0) {
                        avformat_flush(opened.format.get());
                    } else {
                        (void)avformat_seek_file(opened.format.get(), -1, 0, 0, 0,
                                                 AVSEEK_FLAG_BACKWARD);
                    }
                    av_packet_unref(packet.get());
                    std::vector<char> delivered_flags(
                        run.selected_end - run.selected_begin, 0);
                    const std::uint64_t minimum_decode_index =
                        presentation_cursor < index.frames.size()
                            ? index.frames[presentation_cursor].decode_index : 0U;
                    const IndexedFrameMatcher matcher{
                        identity_lookup,
                        selected_frames.subspan(
                            run.selected_begin, run.selected_end - run.selected_begin),
                        minimum_decode_index, run.end_decode_index};
                    std::size_t delivered_count = 0U;
                    auto process = [&](AVFrame &source) {
                        ++local.decoded_frames;
                        const IndexedFrameMatch matched = matcher.match(
                            source, delivered_flags, false, presentation_cursor);
                        if (matched.frame_index) {
                            presentation_cursor = *matched.frame_index + 1U;
                        } else {
                            ++presentation_cursor;
                        }
                        if (!matched.selected_target) return;
                        delivered_flags[*matched.selected_target] = 1;
                        ++delivered_count;
                        const std::size_t target =
                            run.selected_begin + *matched.selected_target;
                        const auto consumer_start = Clock::now();
                        deliver(source, selected_frames[target], target);
                        consumer_ms += std::chrono::duration<double, std::milli>(
                            Clock::now() - consumer_start).count();
                        ++local.selected_frames;
                    };
                    while (delivered_count != delivered_flags.size()
                           && av_read_frame(opened.format.get(), packet.get()) >= 0) {
                        if (stop.stop_requested()) {
                            av_packet_unref(packet.get());
                            throw std::runtime_error("cancelled");
                        }
                        if (failed.load(std::memory_order_relaxed)) {
                            av_packet_unref(packet.get());
                            return;
                        }
                        if (packet->stream_index == static_cast<int>(index.stream_index)) {
                            if (run.end_position && packet->pos >= *run.end_position) {
                                av_packet_unref(packet.get());
                                break;
                            }
                            send_packet_tolerant(
                                *opened.codec, packet.get(), stop, &local,
                                std::string{"avcodec_send_packet("}
                                    + std::string{backend_name} + ")",
                                process);
                            receive_frames(*opened.codec, stop, process);
                        }
                        av_packet_unref(packet.get());
                    }
                    if (delivered_count != delivered_flags.size()) {
                        send_packet_tolerant(
                            *opened.codec, nullptr, stop, &local,
                            std::string{"avcodec_send_packet("}
                                + std::string{backend_name} + " flush)",
                            process);
                        receive_frames(*opened.codec, stop, process);
                    }
                    if (delivered_count == delivered_flags.size()) break;
                    // A retry is only safe before this run emitted a surface;
                    // otherwise the analysis pipeline would receive duplicates.
                    if (delivered_count != 0U) {
                        throw std::runtime_error(
                            std::string{backend_name}
                            + " decoder lost indexed frame identity after a completed prefix");
                    }
                    const std::optional<std::uint64_t> earlier =
                        preceding_rap(index, decode_anchor);
                    if (attempt + 1U >= maximum_decode_attempts || !earlier) {
                        throw std::runtime_error(
                            std::string{backend_name}
                            + " decoder ended before all indexed run frames were produced");
                    }
                    decode_anchor = *earlier;
                    ++local.decode_retries;
                }
            }
        } catch (...) {
            const std::scoped_lock lock(failure_mutex);
            if (!failure) failure = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0U; worker < worker_count; ++worker) {
        workers.emplace_back(decode_slice, worker);
    }
    for (std::thread &worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);

    if (telemetry) {
        telemetry->decode_sessions = worker_count;
        for (const DecodeTelemetry &local : worker_telemetry) {
            telemetry->decoded_frames += local.decoded_frames;
            telemetry->selected_frames += local.selected_frames;
            telemetry->decode_retries += local.decode_retries;
            telemetry->discarded_packets += local.discarded_packets;
        }
        const double longest_consumer_ms = *std::max_element(
            worker_consumer_ms.begin(), worker_consumer_ms.end());
        telemetry->decode_ms += std::max(
            0.0, std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                - longest_consumer_ms);
    }
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
    decode_selected_hardware_indexed(
        path, index, selected_frames, options, stop, "CUDA",
        [&](AVFrame &source, const FrameIdentity &identity, std::size_t seq) {
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
        output.seq = seq;
        output.identity = identity;
        update_identity_metadata(output.identity, source);
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
        consumer(std::move(output));
        }, telemetry);
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
    decode_selected_hardware_indexed(
        path, index, selected_frames, options, stop, "Vulkan",
        [&](AVFrame &source, const FrameIdentity &identity, std::size_t seq) {
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
        output.seq = seq;
        output.identity = identity;
        update_identity_metadata(output.identity, source);
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
        consumer(std::move(output));
        }, telemetry);
#endif
}

void decode_selected_metal(const std::string &path, const MediaIndex &index,
                           std::span<const FrameIdentity> selected_frames,
                           const DecoderOptions &options, std::stop_token stop,
                           const MetalFrameConsumer &consumer,
                           DecodeTelemetry *telemetry) {
#if !defined(__APPLE__)
    (void)path; (void)index; (void)selected_frames; (void)options; (void)stop;
    (void)consumer; (void)telemetry;
    throw std::runtime_error("VideoToolbox hardware decode is only available on macOS");
#else
    if (options.backend != DecoderOptions::Backend::videotoolbox) {
        throw std::invalid_argument("decode_selected_metal requires the VideoToolbox backend");
    }
    decode_selected_hardware_indexed(
        path, index, selected_frames, options, stop, "VideoToolbox",
        [&](AVFrame &source, const FrameIdentity &identity, std::size_t seq) {
            if (source.format != AV_PIX_FMT_VIDEOTOOLBOX) {
                throw std::runtime_error("metal_zero_copy_unsupported: VideoToolbox returned a non-hardware frame");
            }
            // FFmpeg stores the retained CVPixelBufferRef in data[3] for
            // AV_PIX_FMT_VIDEOTOOLBOX; data[0..2] are not image planes.
            CVPixelBufferRef pixel_buffer =
                reinterpret_cast<CVPixelBufferRef>(source.data[3]);
            if (pixel_buffer == nullptr || !CVPixelBufferIsPlanar(pixel_buffer)) {
                throw std::runtime_error("metal_zero_copy_unsupported: VideoToolbox frame has no CVPixelBuffer");
            }
            const OSType format = CVPixelBufferGetPixelFormatType(pixel_buffer);
            const char *surface = nullptr;
            switch (format) {
            case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: surface = "420v"; break;
            case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: surface = "420f"; break;
            case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange: surface = "x420"; break;
            case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: surface = "xf20"; break;
            default: throw std::runtime_error("metal_zero_copy_unsupported: unsupported VideoToolbox surface format");
            }
            MetalFrame output;
            output.seq = seq;
            output.identity = identity;
            update_identity_metadata(output.identity, source);
            output.width = source.width;
            output.height = source.height;
            output.pixel_buffer = reinterpret_cast<std::uintptr_t>(pixel_buffer);
            output.bit_depth = (format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange) ? 10 : 8;
            output.range = frame_range(source);
            output.surface_format = surface;
            output.lease = retain_frame(source, "av_frame_clone(VideoToolbox)");
            consumer(std::move(output));
        }, telemetry);
#endif
}

} // namespace getnative::media
