#include "worker.hpp"

#include "backend_policy.hpp"
#include "capabilities.hpp"
#include "json.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/cpu_features.hpp"
#include "getnative/crop_geometry.hpp"
#include "getnative/candidate_grid.hpp"
#include "getnative/cuda_analysis.hpp"
#include "getnative/metal_analysis.hpp"
#include "getnative/profile.hpp"
#include "getnative/plan_store.hpp"
#include "getnative/utf8_path.hpp"
#include "getnative/vulkan_analysis.hpp"
#if defined(GETNATIVE_HAS_MEDIA)
#include "getnative/media_decode.hpp"
#include "verify_resume.hpp"
#endif

#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace getnative::cli {
namespace {

constexpr std::int64_t kProtocolVersion = 1;
constexpr std::size_t kPlanChunkSize = 64;
constexpr std::size_t kCandidateChunkSize = 32;
constexpr std::size_t kMediaVerifyMinimumConcurrency = 1U;
constexpr std::size_t kMediaVerifyMaximumConcurrency = 16U;
constexpr std::size_t kMediaVerifyDefaultConcurrency = 8U;
// GPU analysis engines independently cap execution slots at 8. Media-verify
// frame concurrency is a different limit and must not be forwarded here.
constexpr std::size_t kAnalysisEngineExecutionSlots = 8U;

std::int64_t timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class WorkerError : public std::runtime_error {
public:
    WorkerError(std::string code, std::string message, bool retryable = false)
        : std::runtime_error(std::move(message)),
          code_(std::move(code)),
          retryable_(retryable) {}
    [[nodiscard]] const std::string &code() const noexcept { return code_; }
    [[nodiscard]] bool retryable() const noexcept { return retryable_; }

private:
    std::string code_;
    bool retryable_;
};

// ---------------------------------------------------------------------------
// Request parsing helpers
// ---------------------------------------------------------------------------

std::string require_string(const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    if (!value || value->type != JsonValue::Type::string) {
        throw WorkerError("bad_request", "missing or invalid string field: " + std::string{key});
    }
    return value->string_value;
}

const JsonValue &require_member(const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    if (!value) {
        throw WorkerError("bad_request", "missing field: " + std::string{key});
    }
    return *value;
}

double require_number(const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    if (!value || value->type != JsonValue::Type::number
        || !std::isfinite(value->number_value)) {
        throw WorkerError("bad_request", "missing or invalid number field: " + std::string{key});
    }
    return value->number_value;
}

std::int32_t require_int(const JsonValue &object, std::string_view key) {
    const double value = require_number(object, key);
    if (std::trunc(value) != value || value < static_cast<double>(INT32_MIN)
        || value > static_cast<double>(INT32_MAX)) {
        throw WorkerError("bad_request", "field must be a 32-bit integer: " + std::string{key});
    }
    return static_cast<std::int32_t>(value);
}

std::int64_t require_int64(const JsonValue &object, std::string_view key) {
    const double value = require_number(object, key);
    if (std::trunc(value) != value
        || value < -9007199254740992.0 || value > 9007199254740992.0) {
        throw WorkerError("bad_request", "field must be an exact integer: " + std::string{key});
    }
    return static_cast<std::int64_t>(value);
}

std::int32_t optional_int(const JsonValue &object, std::string_view key, std::int32_t fallback) {
    const JsonValue *value = object.find(key);
    if (!value || value->is_null()) return fallback;
    return require_int(object, key);
}

[[maybe_unused]] std::optional<std::string> optional_string(
    const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    if (!value || value->is_null()) return std::nullopt;
    if (value->type != JsonValue::Type::string) {
        throw WorkerError("bad_request", "invalid string field: " + std::string{key});
    }
    return value->string_value;
}

std::optional<Geometry> optional_geometry(const JsonValue &object,
                                          std::string_view key = "geometry") {
    const JsonValue *value = object.find(key);
    if (!value || value->is_null()) return std::nullopt;
    if (value->type != JsonValue::Type::object) {
        throw WorkerError("bad_request", "geometry must be an object");
    }
    const std::int32_t width = require_int(*value, "width");
    const std::int32_t height = require_int(*value, "height");
    const bool has_src_fields = value->find("src_left") || value->find("src_top")
        || value->find("src_width") || value->find("src_height");
    if (!has_src_fields) return std::nullopt; // v1 dimension-only geometry
    if (!value->find("src_left") || !value->find("src_top")
        || !value->find("src_width") || !value->find("src_height")) {
        throw WorkerError("bad_request", "fractional geometry requires all src fields");
    }
    const double src_left = require_number(*value, "src_left");
    const double src_top = require_number(*value, "src_top");
    const double src_width = require_number(*value, "src_width");
    const double src_height = require_number(*value, "src_height");
    if (width < 2 || height < 2 || width > 32768 || height > 32768
        || src_width <= 0.0 || src_height <= 0.0 || src_left < 0.0 || src_top < 0.0
        || src_left + src_width > static_cast<double>(width) + 1e-9
        || src_top + src_height > static_cast<double>(height) + 1e-9) {
        throw WorkerError("bad_request", "geometry source rectangle exceeds canvas");
    }
    return Geometry{
        static_cast<std::int64_t>(width), static_cast<std::int64_t>(height),
        src_left, src_top, src_width, src_height,
    };
}

// ---------------------------------------------------------------------------
// Analyze request model
// ---------------------------------------------------------------------------

enum class AxisMode : std::uint8_t { height_only, width_only, height_plus_width };
enum class BackendChoice : std::uint8_t { cpu, cuda, vulkan, metal, automatic };

[[nodiscard]] constexpr const char *backend_choice_name(
    BackendChoice backend) noexcept {
    switch (backend) {
    case BackendChoice::cpu: return "cpu";
    case BackendChoice::cuda: return "cuda";
    case BackendChoice::vulkan: return "vulkan";
    case BackendChoice::metal: return "metal";
    case BackendChoice::automatic: return "auto";
    }
    return "cpu";
}

struct FrameAsset {
    std::string path;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct AnalyzeJobSpec {
    std::string request_id;
    std::string job_id;
    FrameAsset frame;
    AxisMode axis_mode = AxisMode::height_only;
    BackendChoice backend = BackendChoice::cpu;
    Filter filter{};
    std::vector<std::string> candidates;
    // mode "kernel" (protocol v1.1): one fixed geometry, many kernels.
    // `candidates` then holds exactly one decimal (the fixed axis value) and
    // `kernel_filters` carries the ordered kernel list.
    bool kernel_mode = false;
    std::vector<Filter> kernel_filters;
    MetricSpec metric{};
    std::size_t worker_count = 0;
    CompatibilityProfile profile = CompatibilityProfile::muf_d278cd3;
    EndpointRule endpoint_rule = EndpointRule::inclusive;
    std::optional<std::int64_t> base_height;
    std::optional<std::int64_t> base_width;
    std::optional<CandidateRangeSpec> grid;
    std::optional<Geometry> geometry;
    bool profile_geometry = false;
};

// Verification (protocol v1.1): one locked recipe, many streamed frames.
struct VerifyJobSpec {
    std::string request_id;
    std::string job_id;
    std::int32_t width = 0;
    std::int32_t height = 0;
    AxisMode axis_mode = AxisMode::height_only;
    Filter filter{};
    std::string candidate;
    std::optional<Geometry> geometry;
    MetricSpec metric{};
    std::size_t worker_count = 0;
    std::size_t concurrency = kMediaVerifyDefaultConcurrency;
    std::size_t requested_concurrency = kMediaVerifyDefaultConcurrency;
    std::int64_t expected_frames = -1;
    BackendChoice requested_backend = BackendChoice::cpu;
    BackendChoice backend = BackendChoice::cpu;
    std::string selected_device;
    std::string selected_device_uuid;
#if defined(GETNATIVE_HAS_MEDIA)
    struct MediaInput {
        std::string path;
        std::string fingerprint;
        std::string cache_directory;
        std::uint32_t stream_index = 0U;
        media::ScanScope scope;
    };
    std::optional<MediaInput> media;
#endif
    struct Fallback {
        std::string code;
        std::string from;
        std::string to;
        std::string reason;
        std::uint64_t frame_seq = 0U;
    };
    std::vector<Fallback> fallback_chain;
};

class MappedRing;

struct VerifyFrameItem {
    std::uint64_t seq = 0;
    FrameAsset asset;
    std::shared_ptr<const MappedRing> ring;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
};

struct VerifyFrameResult {
    std::uint64_t seq = 0;
    std::optional<double> error;
};

#if defined(GETNATIVE_HAS_MEDIA)
template <class Frame>
class MediaVerifyPipeline {
public:
    using Analyzer = std::function<double(const Frame &)>;
    using Collector = std::function<void(
        std::uint64_t seq, const media::FrameIdentity &, double,
        std::uint64_t completed)>;

    MediaVerifyPipeline(std::size_t concurrency, std::stop_token stop,
                        Analyzer analyzer, Collector collector,
                        std::function<void()> stop_decoder)
        : concurrency_(concurrency), stop_(stop), analyzer_(std::move(analyzer)),
          collector_(std::move(collector)),
          stop_decoder_(std::move(stop_decoder)) {}

    MediaVerifyPipeline(const MediaVerifyPipeline &) = delete;
    MediaVerifyPipeline &operator=(const MediaVerifyPipeline &) = delete;

    template <class Decoder>
    void run(Decoder &&decoder) {
        std::stop_callback cancelled{stop_, [this] { condition_.notify_all(); }};
        workers_.reserve(concurrency_);
        for (std::size_t index = 0U; index < concurrency_; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }

        std::exception_ptr decode_failure;
        try {
            decoder([this](Frame frame) { push(std::move(frame)); });
        } catch (...) {
            decode_failure = std::current_exception();
        }
        {
            const std::scoped_lock lock(mutex_);
            decoding_done_ = true;
            if (stop_.stop_requested()) {
                abort_ = true;
                inflight_ -= queue_.size();
                queue_.clear();
            }
        }
        condition_.notify_all();
        for (std::thread &worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();

        if (failure_) std::rethrow_exception(failure_);
        if (stop_.stop_requested()) {
            throw WorkerError("cancelled", "media verification cancelled");
        }
        if (decode_failure) std::rethrow_exception(decode_failure);
    }

    [[nodiscard]] std::uint64_t completed() const noexcept {
        return completed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t max_inflight() const noexcept {
        return max_inflight_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double queue_wait_ms() const noexcept {
        return queue_wait_ms_.load(std::memory_order_relaxed);
    }

private:
    std::size_t concurrency_;
    std::stop_token stop_;
    Analyzer analyzer_;
    Collector collector_;
    std::function<void()> stop_decoder_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::mutex collector_mutex_;
    std::deque<Frame> queue_;
    std::vector<std::thread> workers_;
    std::exception_ptr failure_;
    std::size_t inflight_ = 0U;
    bool decoding_done_ = false;
    bool abort_ = false;
    std::atomic<std::uint64_t> completed_{0U};
    std::atomic<std::size_t> max_inflight_{0U};
    std::atomic<double> queue_wait_ms_{0.0};

    void push(Frame frame) {
        const auto wait_start = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] {
            return abort_ || failure_ || stop_.stop_requested()
                || inflight_ < concurrency_;
        });
        queue_wait_ms_.fetch_add(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wait_start).count(),
            std::memory_order_relaxed);
        if (failure_) std::rethrow_exception(failure_);
        if (abort_ || stop_.stop_requested()) {
            throw WorkerError("cancelled", "media verification cancelled");
        }
        queue_.push_back(std::move(frame));
        ++inflight_;
        max_inflight_.store(
            std::max(max_inflight_.load(std::memory_order_relaxed), inflight_),
            std::memory_order_relaxed);
        lock.unlock();
        condition_.notify_one();
    }

    void fail(std::exception_ptr error) noexcept {
        {
            const std::scoped_lock lock(mutex_);
            if (!failure_) failure_ = std::move(error);
            abort_ = true;
            inflight_ -= queue_.size();
            queue_.clear();
        }
        if (stop_decoder_) stop_decoder_();
        condition_.notify_all();
    }

    void worker_loop() noexcept {
        for (;;) {
            std::optional<Frame> frame;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return abort_ || stop_.stop_requested() || !queue_.empty()
                        || decoding_done_;
                });
                if (abort_ || stop_.stop_requested()) return;
                if (queue_.empty()) {
                    if (decoding_done_) return;
                    continue;
                }
                frame.emplace(std::move(queue_.front()));
                queue_.pop_front();
            }

            try {
                const double error = analyzer_(*frame);
                const std::uint64_t seq = frame->seq;
                const media::FrameIdentity identity = frame->identity;
                const std::uint64_t done =
                    completed_.fetch_add(1U, std::memory_order_relaxed) + 1U;
                frame.reset();
                {
                    const std::scoped_lock lock(mutex_);
                    --inflight_;
                }
                condition_.notify_all();
                // Report completion order, not presentation order. Concurrent
                // decode/analysis otherwise holds seq>0 in pending until seq 0
                // finishes, so the UI sees no verify progress or speed.
                const std::scoped_lock collector_lock(collector_mutex_);
                collector_(seq, identity, error, done);
            } catch (...) {
                fail(std::current_exception());
                return;
            }
        }
    }
};
#endif

AxisMode parse_axis_mode(const std::string &value) {
    if (value == "h_only") return AxisMode::height_only;
    if (value == "w_only") return AxisMode::width_only;
    if (value == "h_plus_w") return AxisMode::height_plus_width;
    throw WorkerError("bad_request", "unknown axis_mode: " + value);
}

Filter parse_filter(const JsonValue &kernel) {
    const std::string id = require_string(kernel, "id");
    double blur = 1.0;
    if (const JsonValue *blur_value = kernel.find("blur");
        blur_value != nullptr && !blur_value->is_null()) {
        blur = require_number(kernel, "blur");
        if (!std::isfinite(blur) || blur <= 0.0) {
            throw WorkerError("bad_request", "blur must be finite and greater than zero");
        }
    }
    if (id == "bilinear") return Filter::bilinear(blur);
    if (id == "spline16") return Filter::spline16(blur);
    if (id == "spline36") return Filter::spline36(blur);
    if (id == "spline64") return Filter::spline64(blur);
    if (id == "bicubic") {
        const JsonValue *b = kernel.find("b");
        const JsonValue *c = kernel.find("c");
        if (!b || b->is_null() || !c || c->is_null()) {
            throw WorkerError("bad_request", "bicubic requires explicit b and c");
        }
        const double b_value = require_number(kernel, "b");
        const double c_value = require_number(kernel, "c");
        if (!std::isfinite(b_value) || !std::isfinite(c_value)) {
            throw WorkerError("bad_request", "bicubic parameters must be finite");
        }
        return Filter::bicubic(b_value, c_value, blur);
    }
    if (id == "lanczos") {
        const JsonValue *taps = kernel.find("taps");
        if (!taps || taps->is_null()) {
            throw WorkerError("bad_request", "lanczos requires explicit taps");
        }
        const std::int32_t value = require_int(kernel, "taps");
        if (value < 1 || value > 15) {
            throw WorkerError("bad_request", "lanczos taps must be within 1..15");
        }
        return Filter::lanczos(value, blur);
    }
    throw WorkerError("bad_request", "unknown kernel id: " + id);
}

// Canonical echo of a parsed kernel spec: bicubic always
// carries b and c, lanczos always carries taps.
JsonValue filter_to_json(const Filter &filter) {
    std::vector<std::pair<std::string, JsonValue>> members;
    switch (filter.type) {
    case KernelType::bilinear:
        members.emplace_back("id", JsonValue::string("bilinear"));
        break;
    case KernelType::bicubic:
        members.emplace_back("id", JsonValue::string("bicubic"));
        members.emplace_back("b", JsonValue::number(filter.b));
        members.emplace_back("c", JsonValue::number(filter.c));
        break;
    case KernelType::lanczos:
        members.emplace_back("id", JsonValue::string("lanczos"));
        members.emplace_back("taps", JsonValue::integer(filter.taps));
        break;
    case KernelType::spline16:
        members.emplace_back("id", JsonValue::string("spline16"));
        break;
    case KernelType::spline36:
        members.emplace_back("id", JsonValue::string("spline36"));
        break;
    case KernelType::spline64:
        members.emplace_back("id", JsonValue::string("spline64"));
        break;
    }
    return JsonValue::object(std::move(members));
}

MetricSpec parse_metric(const JsonValue &metric) {
    MetricSpec result{};
    result.crop_left = optional_int(metric, "crop_left", result.crop_left);
    result.crop_right = optional_int(metric, "crop_right", result.crop_right);
    result.crop_top = optional_int(metric, "crop_top", result.crop_top);
    result.crop_bottom = optional_int(metric, "crop_bottom", result.crop_bottom);
    if (metric.find("threshold")) {
        const double value = require_number(metric, "threshold");
        if (value < 0.0) throw WorkerError("bad_request", "threshold must be non-negative");
        result.threshold = static_cast<float>(value);
    }
    const double p_value = metric.find("p_norm") ? require_number(metric, "p_norm") : 1.0;
    if (p_value < 1.0 || p_value > 4294967295.0 || std::trunc(p_value) != p_value) {
        throw WorkerError("bad_request", "p_norm must be an integer in 1..4294967295");
    }
    result.norm = static_cast<std::uint32_t>(p_value);
    if (result.crop_left < 0 || result.crop_right < 0 || result.crop_top < 0
        || result.crop_bottom < 0) {
        throw WorkerError("bad_request", "crop values must be non-negative");
    }
    return result;
}

std::optional<std::int64_t> optional_decimal_integer(
    const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    if (!value || value->is_null()) return std::nullopt;
    if (value->type != JsonValue::Type::string || value->string_value.empty()) {
        throw WorkerError("bad_request", std::string{key} + " must be a positive integer string");
    }
    std::int64_t result = 0;
    const auto *begin = value->string_value.data();
    const auto *end = begin + value->string_value.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result <= 0) {
        throw WorkerError("bad_request", std::string{key} + " must be a positive integer string");
    }
    return result;
}

EndpointRule parse_endpoint_rule(const JsonValue &object) {
    const JsonValue *value = object.find("endpoint_rule");
    if (!value || value->is_null()) return EndpointRule::inclusive;
    const std::string text = require_string(object, "endpoint_rule");
    if (text == "inclusive") return EndpointRule::inclusive;
    if (text == "exclusive_stop") return EndpointRule::exclusive_stop;
    throw WorkerError("bad_request", "unknown endpoint_rule: " + text);
}

std::optional<CandidateRangeSpec> parse_candidate_grid(const JsonValue &command) {
    const JsonValue *value = command.find("grid");
    if (!value || value->is_null()) return std::nullopt;
    if (value->type != JsonValue::Type::object) {
        throw WorkerError("bad_request", "grid must be an object");
    }
    CandidateRangeSpec grid;
    grid.start = require_string(*value, "start");
    grid.stop = require_string(*value, "stop");
    grid.step = require_string(*value, "step");
    grid.endpoint = parse_endpoint_rule(command);
    grid.maximum_count = 100000U;
    return grid;
}

FrameAsset parse_frame_asset(const JsonValue &asset) {
    FrameAsset result;
    result.path = require_string(asset, "path");
    if (result.path.empty()) {
        throw WorkerError("bad_request", "frame_asset.path must not be empty");
    }
    const std::string format = require_string(asset, "format");
    if (format != "f32le") {
        throw WorkerError("unsupported", "frame asset format must be f32le in protocol v1");
    }
    result.width = require_int(asset, "width");
    result.height = require_int(asset, "height");
    if (result.width < 2 || result.height < 2) {
        throw WorkerError("bad_request", "frame dimensions must be at least 2");
    }
    return result;
}

AnalyzeJobSpec parse_analyze(const JsonValue &command, std::string job_id) {
    AnalyzeJobSpec spec;
    spec.request_id = require_string(command, "request_id");
    spec.job_id = std::move(job_id);
    const std::string mode = require_string(command, "mode");
    spec.kernel_mode = mode == "kernel";
    if (mode != "height" && !spec.kernel_mode) {
        throw WorkerError("unsupported",
                          "mode must be height or kernel in protocol v1.1, got: " + mode);
    }
    const std::string backend = require_string(command, "backend");
    if (backend == "cpu") {
        spec.backend = BackendChoice::cpu;
    } else if (backend == "cuda") {
        spec.backend = BackendChoice::cuda;
    } else if (backend == "vulkan") {
        spec.backend = BackendChoice::vulkan;
    } else if (backend == "metal") {
        spec.backend = BackendChoice::metal;
    } else if (backend == "auto") {
        spec.backend = BackendChoice::automatic;
    } else {
        throw WorkerError("unsupported", "unknown backend: " + backend);
    }
    spec.frame = parse_frame_asset(require_member(command, "frame_asset"));
    spec.axis_mode = parse_axis_mode(require_string(command, "axis_mode"));
    spec.metric = parse_metric(require_member(command, "metric"));
    if (const JsonValue *profile_value = command.find("profile_id"); profile_value) {
        const auto parsed = parse_profile(require_string(command, "profile_id"));
        if (!parsed) throw WorkerError("bad_request", "unknown profile_id");
        spec.profile = *parsed;
        spec.profile_geometry = true;
    }
    spec.endpoint_rule = parse_endpoint_rule(command);
    spec.base_height = optional_decimal_integer(command, "base_height");
    spec.base_width = optional_decimal_integer(command, "base_width");
    spec.geometry = optional_geometry(command);
    spec.grid = parse_candidate_grid(command);
    if (backend == "cuda"
        && (spec.metric.norm < cuda_minimum_p_norm
            || spec.metric.norm > cuda_maximum_p_norm)) {
        throw WorkerError("unsupported", "CUDA backend only supports p_norm in 1..4");
    }
    if (backend == "vulkan"
        && (spec.metric.norm < vulkan_minimum_p_norm
            || spec.metric.norm > vulkan_maximum_p_norm)) {
        throw WorkerError("unsupported", "Vulkan backend only supports p_norm in 1..4");
    }
    if (backend == "metal"
        && (spec.metric.norm < metal_minimum_p_norm
            || spec.metric.norm > metal_maximum_p_norm)) {
        throw WorkerError("unsupported", "Metal backend only supports p_norm in 1..4");
    }

    if (spec.kernel_mode) {
        // Kernel scan: one fixed axis value + an ordered kernel list. The
        // fixed value travels in the single `candidate` field (same decimal
        // semantics as verify_begin); `kernel` must not accompany `kernels`.
        if (command.find("kernel")) {
            throw WorkerError("bad_request",
                              "kernel mode takes kernels, not kernel");
        }
        const JsonValue *candidate = command.find("candidate");
        if (candidate == nullptr) {
            throw WorkerError("bad_request", "kernel mode requires candidate");
        }
        std::string decimal;
        if (candidate->type == JsonValue::Type::string) {
            decimal = candidate->string_value;
        } else if (candidate->type == JsonValue::Type::number) {
            decimal = candidate->raw_number;
        } else {
            throw WorkerError("bad_request",
                              "candidate must be a decimal string or number");
        }
        double value = 0.0;
        try {
            const JsonValue parsed = parse_json(decimal);
            if (parsed.type != JsonValue::Type::number) {
                throw std::runtime_error("not a number");
            }
            value = parsed.number_value;
        } catch (const std::exception &) {
            throw WorkerError("bad_request", "invalid candidate decimal: " + decimal);
        }
        if (!std::isfinite(value) || value < 2.0) {
            throw WorkerError("bad_request", "candidate must be finite and >= 2");
        }
        spec.candidates.push_back(std::move(decimal));

        const JsonValue &kernels = require_member(command, "kernels");
        if (kernels.type != JsonValue::Type::array || kernels.items.empty()) {
            throw WorkerError("bad_request", "kernels must be a non-empty array");
        }
        if (kernels.items.size() > 4096U) {
            throw WorkerError("bad_request", "kernels exceeds the 4096-entry cap");
        }
        spec.kernel_filters.reserve(kernels.items.size());
        for (const JsonValue &kernel : kernels.items) {
            if (kernel.type != JsonValue::Type::object) {
                throw WorkerError("bad_request", "kernels entries must be objects");
            }
            spec.kernel_filters.push_back(parse_filter(kernel));
        }
        if (command.find("worker_count")) {
            const double workers = require_number(command, "worker_count");
            if (workers < 0.0 || std::trunc(workers) != workers) {
                throw WorkerError("bad_request",
                                  "worker_count must be a non-negative integer");
            }
            spec.worker_count = static_cast<std::size_t>(workers);
        }
        return spec;
    }

    spec.filter = parse_filter(require_member(command, "kernel"));

    const JsonValue &candidates = require_member(command, "candidates");
    if (candidates.type != JsonValue::Type::array || candidates.items.empty()) {
        throw WorkerError("bad_request", "candidates must be a non-empty array");
    }
    spec.candidates.reserve(candidates.items.size());
    for (const JsonValue &candidate : candidates.items) {
        std::string decimal;
        if (candidate.type == JsonValue::Type::string) {
            decimal = candidate.string_value;
        } else if (candidate.type == JsonValue::Type::number) {
            decimal = candidate.raw_number;
        } else {
            throw WorkerError("bad_request", "candidates must be decimal strings or numbers");
        }
        double value = 0.0;
        try {
            const JsonValue parsed = parse_json(decimal);
            if (parsed.type != JsonValue::Type::number) throw std::runtime_error("not a number");
            value = parsed.number_value;
        } catch (const std::exception &) {
            throw WorkerError("bad_request", "invalid candidate decimal: " + decimal);
        }
        if (!std::isfinite(value) || value < 2.0) {
            throw WorkerError("bad_request", "candidate values must be finite and >= 2");
        }
        spec.candidates.push_back(std::move(decimal));
    }
    if (spec.grid) {
        const auto generated = generate_candidate_range(*spec.grid, profile(spec.profile).default_grid);
        if (generated.size() != spec.candidates.size()) {
            throw WorkerError("bad_request", "grid candidate count does not match candidates");
        }
    }
    if (command.find("worker_count")) {
        const double value = require_number(command, "worker_count");
        if (value < 0.0 || std::trunc(value) != value) {
            throw WorkerError("bad_request", "worker_count must be a non-negative integer");
        }
        spec.worker_count = static_cast<std::size_t>(value);
    }
    return spec;
}

// ---------------------------------------------------------------------------
// Verify request model (protocol v1.1)
// ---------------------------------------------------------------------------

constexpr std::size_t kVerifyDefaultWorkers = 16U;
constexpr std::size_t kVerifyResultBatchSize = 64U;
constexpr std::int64_t kVerifyMaxFrames = 1000000;

VerifyJobSpec parse_verify_begin(const JsonValue &command, std::string job_id,
                                 bool media_mode = false) {
    VerifyJobSpec spec;
    spec.request_id = require_string(command, "request_id");
    spec.job_id = std::move(job_id);

    const JsonValue &geometry = require_member(command, "geometry");
    spec.width = require_int(geometry, "width");
    spec.height = require_int(geometry, "height");
    if (spec.width < 2 || spec.height < 2
        || spec.width > 32768 || spec.height > 32768) {
        throw WorkerError("bad_request", "verify geometry must be within 2..32768");
    }

    const std::string backend = require_string(command, "backend");
    if (backend == "cpu") {
        spec.requested_backend = BackendChoice::cpu;
    } else if (backend == "auto") {
        spec.requested_backend = BackendChoice::automatic;
    } else if (media_mode && backend == "cuda") {
        spec.requested_backend = BackendChoice::cuda;
    } else if (media_mode && backend == "vulkan") {
        spec.requested_backend = BackendChoice::vulkan;
    } else if (media_mode && backend == "metal") {
        spec.requested_backend = BackendChoice::metal;
    } else if (backend == "cuda" || backend == "vulkan" || backend == "metal") {
        throw WorkerError(
            "unsupported",
            "verify mode is CPU-only in protocol v1.1");
    } else {
        throw WorkerError("unsupported", "unknown backend: " + backend);
    }
    spec.backend = media_mode ? spec.requested_backend : BackendChoice::cpu;

    spec.axis_mode = parse_axis_mode(require_string(command, "axis_mode"));
    spec.geometry = optional_geometry(command, "resolved_geometry");
    spec.filter = parse_filter(require_member(command, "kernel"));
    spec.metric = parse_metric(require_member(command, "metric"));
    if (media_mode && spec.requested_backend == BackendChoice::cuda
        && (spec.metric.norm < cuda_minimum_p_norm
            || spec.metric.norm > cuda_maximum_p_norm)) {
        throw WorkerError("unsupported", "CUDA verify only supports p_norm in 1..4");
    }
    if (media_mode && spec.requested_backend == BackendChoice::vulkan
        && (spec.metric.norm < vulkan_minimum_p_norm
            || spec.metric.norm > vulkan_maximum_p_norm)) {
        throw WorkerError("unsupported", "Vulkan verify only supports p_norm in 1..4");
    }
    if (media_mode && spec.requested_backend == BackendChoice::metal
        && (spec.metric.norm < metal_minimum_p_norm
            || spec.metric.norm > metal_maximum_p_norm)) {
        throw WorkerError("unsupported", "Metal verify only supports p_norm in 1..4");
    }

    const JsonValue &candidate = require_member(command, "candidate");
    std::string decimal;
    if (candidate.type == JsonValue::Type::string) {
        decimal = candidate.string_value;
    } else if (candidate.type == JsonValue::Type::number) {
        decimal = candidate.raw_number;
    } else {
        throw WorkerError("bad_request", "candidate must be a decimal string or number");
    }
    double value = 0.0;
    try {
        const JsonValue parsed = parse_json(decimal);
        if (parsed.type != JsonValue::Type::number) throw std::runtime_error("not a number");
        value = parsed.number_value;
    } catch (const std::exception &) {
        throw WorkerError("bad_request", "invalid candidate decimal: " + decimal);
    }
    if (!std::isfinite(value) || value < 2.0) {
        throw WorkerError("bad_request", "candidate must be finite and >= 2");
    }
    const std::int32_t primary =
        spec.axis_mode == AxisMode::width_only ? spec.width : spec.height;
    if (value >= static_cast<double>(primary)) {
        throw WorkerError("bad_request", "candidate must be below the source axis length");
    }
    spec.candidate = std::move(decimal);

    if (!media_mode && command.find("worker_count")) {
        const double workers = require_number(command, "worker_count");
        if (workers < 0.0 || std::trunc(workers) != workers) {
            throw WorkerError("bad_request", "worker_count must be a non-negative integer");
        }
        spec.worker_count = static_cast<std::size_t>(workers);
    }
    if (media_mode && command.find("concurrency")) {
        const double concurrency = require_number(command, "concurrency");
        if (std::trunc(concurrency) != concurrency
            || concurrency < static_cast<double>(kMediaVerifyMinimumConcurrency)
            || concurrency > static_cast<double>(kMediaVerifyMaximumConcurrency)) {
            throw WorkerError(
                "bad_request", "concurrency must be an integer within 1..16");
        }
        spec.concurrency = static_cast<std::size_t>(concurrency);
        spec.requested_concurrency = spec.concurrency;
    }
    if (command.find("expected_frames")) {
        spec.expected_frames = require_int64(command, "expected_frames");
        if (spec.expected_frames < 1 || spec.expected_frames > kVerifyMaxFrames) {
            throw WorkerError("bad_request", "expected_frames must be within 1..1000000");
        }
    }
#if defined(GETNATIVE_HAS_MEDIA)
    if (media_mode) {
        const JsonValue &media_input = require_member(command, "media");
        VerifyJobSpec::MediaInput input;
        input.path = require_string(media_input, "path");
        if (input.path.empty()) throw WorkerError("bad_request", "media path must not be empty");
        input.fingerprint = optional_string(media_input, "fingerprint").value_or("");
        input.cache_directory = optional_string(media_input, "cache_directory")
            .value_or(path_to_utf8(std::filesystem::temp_directory_path()
                                   / "getnative-media-cache"));
        const std::int32_t stream_index = require_int(media_input, "stream_index");
        if (stream_index < 0) {
            throw WorkerError("bad_request", "media stream_index must be non-negative");
        }
        input.stream_index = static_cast<std::uint32_t>(stream_index);
        const JsonValue &scope = require_member(command, "scan_scope");
        const std::string selection = require_string(scope, "selection");
        if (selection == "all") {
            input.scope.selection = media::ScanSelection::all;
        } else if (selection == "every_n") {
            input.scope.selection = media::ScanSelection::every_n;
            const std::int64_t every_n = require_int64(scope, "every_n");
            if (every_n < 1) {
                throw WorkerError("bad_request", "every_n selection requires every_n >= 1");
            }
            input.scope.every_n = static_cast<std::uint64_t>(every_n);
        } else if (selection == "decoded_i_picture") {
            input.scope.selection = media::ScanSelection::decoded_i_picture;
        } else {
            throw WorkerError("bad_request", "unknown scan selection: " + selection);
        }
        if (scope.find("start_frame") && !scope.find("start_frame")->is_null()) {
            const std::int64_t start = require_int64(scope, "start_frame");
            if (start < 0) throw WorkerError("bad_request", "start_frame must be non-negative");
            input.scope.start_frame = static_cast<std::uint64_t>(start);
        }
        if (scope.find("end_frame") && !scope.find("end_frame")->is_null()) {
            const std::int64_t end = require_int64(scope, "end_frame");
            if (end < 0) throw WorkerError("bad_request", "end_frame must be non-negative");
            input.scope.end_frame = static_cast<std::uint64_t>(end);
        }
        if (input.scope.start_frame && input.scope.end_frame
            && *input.scope.start_frame > *input.scope.end_frame) {
            throw WorkerError("bad_request", "scan range start must be <= end");
        }
        spec.media = std::move(input);
    }
#else
    (void)media_mode;
#endif
    return spec;
}

void cap_media_verify_concurrency(VerifyJobSpec &spec) {
    if (spec.requested_concurrency == 0U) {
        spec.requested_concurrency = spec.concurrency;
    }
    const bool gpu = spec.backend == BackendChoice::cuda
        || spec.backend == BackendChoice::vulkan
        || spec.backend == BackendChoice::metal;
    if (!gpu || spec.concurrency <= kAnalysisEngineExecutionSlots) return;
    spec.concurrency = kAnalysisEngineExecutionSlots;
}

std::size_t effective_verify_workers(const VerifyJobSpec &spec) {
    if (spec.worker_count != 0U) return spec.worker_count;
    const std::size_t hardware =
        static_cast<std::size_t>(std::max(1U, std::thread::hardware_concurrency()));
    return std::min(kVerifyDefaultWorkers, hardware);
}

// ---------------------------------------------------------------------------
// Frame asset loading
// ---------------------------------------------------------------------------

std::filesystem::path executable_directory() {
    std::filesystem::path executable;
#if defined(_WIN32)
    std::vector<wchar_t> buffer(512U);
    while (buffer.size() <= 32768U) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) break;
        if (static_cast<std::size_t>(length) < buffer.size()) {
            executable = std::filesystem::path{
                std::wstring_view{buffer.data(), static_cast<std::size_t>(length)}};
            break;
        }
        buffer.resize(buffer.size() * 2U);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0U;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size != 0U) {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            executable = std::filesystem::path{buffer.data()};
        }
    }
#else
    std::vector<char> buffer(512U);
    while (buffer.size() <= 1024U * 1024U) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) break;
        if (static_cast<std::size_t>(length) < buffer.size()) {
            executable.assign(
                std::string{buffer.data(), static_cast<std::size_t>(length)});
            break;
        }
        buffer.resize(buffer.size() * 2U);
    }
#endif
    if (!executable.empty()) {
        std::error_code error;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(executable, error);
        if (!error) executable = resolved;
        if (executable.has_parent_path()) return executable.parent_path();
    }

    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    return error ? std::filesystem::path{"."} : current;
}

std::filesystem::path default_plan_store_dir() {
#if defined(__linux__)
    if (const char *xdg_cache = std::getenv("XDG_CACHE_HOME")) {
        const std::filesystem::path base{xdg_cache};
        if (!base.empty() && base.is_absolute()) {
            return base / "io.getnative.vf" / "axis-plans";
        }
    }
    if (const char *home = std::getenv("HOME")) {
        const std::filesystem::path base{home};
        if (!base.empty() && base.is_absolute()) {
            return base / ".cache" / "io.getnative.vf" / "axis-plans";
        }
    }
#endif
    return executable_directory();
}

// Linux uses the per-user XDG cache so installed and AppImage builds always
// have a stable writable store. Other platforms retain the portable default.
// An explicit directory overrides either location; GETNATIVE_PLAN_CACHE=off
// disables persistence while leaving L1 enabled.
std::optional<std::filesystem::path> resolve_plan_store_dir() {
    const char *toggle = std::getenv("GETNATIVE_PLAN_CACHE");
    if (toggle != nullptr && std::string_view{toggle} == "off") return std::nullopt;
    if (const auto explicit_dir = path_from_environment("GETNATIVE_PLAN_CACHE_DIR")) {
        if (!explicit_dir->empty()) return explicit_dir;
    }
    return default_plan_store_dir();
}

void load_frame_into(const FrameAsset &asset, float *destination) {
    const std::uint64_t elements =
        static_cast<std::uint64_t>(asset.width) * static_cast<std::uint64_t>(asset.height);
    const std::uint64_t bytes = elements * sizeof(float);
    std::ifstream input(path_from_utf8(asset.path), std::ios::binary | std::ios::ate);
    if (!input) {
        throw WorkerError("frame_asset_error", "cannot open frame asset: " + asset.path, true);
    }
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) != bytes) {
        throw WorkerError(
            "frame_asset_error",
            "frame asset size mismatch: expected " + std::to_string(bytes)
                + " bytes, found " + std::to_string(static_cast<long long>(size)));
    }
    input.seekg(0);
    input.read(reinterpret_cast<char *>(destination), static_cast<std::streamsize>(bytes));
    if (!input) {
        throw WorkerError("frame_asset_error", "failed while reading frame asset: " + asset.path, true);
    }
}

// Single-use frame access for verify workers. Verification is DRAM-bound
// (docs/performance/e2-verification-pipeline-*): the read(2) path copies
// every frame page-cache → user buffer, adding ~16.6 MB of traffic per
// 1080p frame on top of analysis. Mapping the asset read-only lets analysis
// consume the page cache directly. Producers publish assets atomically
// (tmp+rename), so an open mapping can never observe a partially written
// frame. Windows keeps the read() path until a MapViewOfFile branch lands.
#if !defined(_WIN32)
class MappedFrame {
public:
    explicit MappedFrame(const FrameAsset &asset) {
        const std::uint64_t elements =
            static_cast<std::uint64_t>(asset.width) * static_cast<std::uint64_t>(asset.height);
        bytes_ = elements * sizeof(float);
        const int fd = ::open(asset.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw WorkerError("frame_asset_error", "cannot open frame asset: " + asset.path, true);
        }
        struct stat info {};
        if (::fstat(fd, &info) != 0 || static_cast<std::uint64_t>(info.st_size) != bytes_) {
            ::close(fd);
            throw WorkerError(
                "frame_asset_error",
                "frame asset size mismatch: expected " + std::to_string(bytes_)
                    + " bytes, found "
                    + (info.st_size >= 0 ? std::to_string(static_cast<long long>(info.st_size))
                                         : std::string{"<stat failed>"}));
        }
        int mmap_flags = MAP_PRIVATE;
#if defined(__linux__)
        mmap_flags |= MAP_POPULATE;
#endif
        void *mapped = ::mmap(nullptr, bytes_, PROT_READ, mmap_flags, fd, 0);
        ::close(fd);
        if (mapped == MAP_FAILED) {
            throw WorkerError("frame_asset_error", "cannot map frame asset: " + asset.path, true);
        }
        data_ = static_cast<const float *>(mapped);
    }
    MappedFrame(const MappedFrame &) = delete;
    MappedFrame &operator=(const MappedFrame &) = delete;
    ~MappedFrame() {
        if (data_ != nullptr) {
            ::munmap(const_cast<float *>(data_), bytes_);
        }
    }

    [[nodiscard]] const float *data() const noexcept { return data_; }

private:
    const float *data_ = nullptr;
    std::uint64_t bytes_ = 0;
};
#endif

class MappedRing {
public:
    MappedRing(const std::string &path, std::uint32_t slot_count,
               std::uint64_t frame_bytes)
        : slot_count_(slot_count), frame_bytes_(frame_bytes) {
        if (slot_count_ == 0U || frame_bytes_ == 0U
            || frame_bytes_ > std::numeric_limits<std::uint64_t>::max() / slot_count_) {
            throw WorkerError("bad_request", "invalid verify ring dimensions");
        }
        bytes_ = frame_bytes_ * slot_count_;
#if defined(_WIN32)
        const std::filesystem::path native_path = path_from_utf8(path);
        file_ = ::CreateFileW(native_path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw WorkerError("frame_asset_error", "cannot open verify ring: " + path, true);
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file_, &size)
            || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != bytes_) {
            ::CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw WorkerError("frame_asset_error", "verify ring size mismatch");
        }
        mapping_ = ::CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            ::CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw WorkerError("frame_asset_error", "cannot create verify ring mapping", true);
        }
        data_ = static_cast<const std::byte *>(
            ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(bytes_)));
        if (data_ == nullptr) {
            ::CloseHandle(mapping_);
            ::CloseHandle(file_);
            mapping_ = nullptr;
            file_ = INVALID_HANDLE_VALUE;
            throw WorkerError("frame_asset_error", "cannot map verify ring", true);
        }
#else
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw WorkerError("frame_asset_error", "cannot open verify ring: " + path, true);
        }
        struct stat info {};
        if (::fstat(fd, &info) != 0 || info.st_size < 0
            || static_cast<std::uint64_t>(info.st_size) != bytes_) {
            ::close(fd);
            throw WorkerError("frame_asset_error", "verify ring size mismatch");
        }
        void *mapped = ::mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mapped == MAP_FAILED) {
            throw WorkerError("frame_asset_error", "cannot map verify ring: " + path, true);
        }
        data_ = static_cast<const std::byte *>(mapped);
#endif
    }

    MappedRing(const MappedRing &) = delete;
    MappedRing &operator=(const MappedRing &) = delete;
    ~MappedRing() {
#if defined(_WIN32)
        if (data_ != nullptr) ::UnmapViewOfFile(data_);
        if (mapping_ != nullptr) ::CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
#else
        if (data_ != nullptr) ::munmap(const_cast<std::byte *>(data_), bytes_);
#endif
    }

    [[nodiscard]] const float *slot_data(std::uint32_t slot) const {
        if (slot >= slot_count_) {
            throw WorkerError("bad_request", "verify ring slot is out of range");
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(slot) * frame_bytes_;
        return reinterpret_cast<const float *>(data_ + offset);
    }
    [[nodiscard]] std::uint32_t slot_count() const noexcept { return slot_count_; }
    [[nodiscard]] std::uint64_t frame_bytes() const noexcept { return frame_bytes_; }

private:
    const std::byte *data_ = nullptr;
    std::uint32_t slot_count_ = 0;
    std::uint64_t frame_bytes_ = 0;
    std::uint64_t bytes_ = 0;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#endif
};

std::vector<float> load_frame_asset(const FrameAsset &asset) {
    const std::uint64_t elements =
        static_cast<std::uint64_t>(asset.width) * static_cast<std::uint64_t>(asset.height);
    std::vector<float> frame(static_cast<std::size_t>(elements));
    load_frame_into(asset, frame.data());
    return frame;
}

// ---------------------------------------------------------------------------
// Worker session
// ---------------------------------------------------------------------------

enum class JobKind : std::uint8_t { analyze, verify };

struct Job {
    explicit Job(AnalyzeJobSpec job_spec)
        : kind(JobKind::analyze), spec(std::move(job_spec)) {}
    explicit Job(VerifyJobSpec verify_spec)
        : kind(JobKind::verify), verify(std::move(verify_spec)) {}

    [[nodiscard]] const std::string &id() const {
        return kind == JobKind::analyze ? spec.job_id : verify.job_id;
    }
    [[nodiscard]] const std::string &request_id() const {
        return kind == JobKind::analyze ? spec.request_id : verify.request_id;
    }
    [[nodiscard]] const char *mode() const {
        return kind == JobKind::analyze ? "height" : "verify";
    }
    void request_cancel() {
        cancel_requested.store(true);
        stop_source.request_stop();
        verify_condition.notify_all();
    }

    JobKind kind;
    AnalyzeJobSpec spec;
    VerifyJobSpec verify;
    std::atomic<bool> cancel_requested{false};
    std::stop_source stop_source;
    bool started = false;
    std::chrono::steady_clock::time_point queued_at =
        std::chrono::steady_clock::now();

    // Verify frame stream. The reader thread appends inbox items and the
    // end marker; the executor's analysis workers consume them. One mutex
    // guards every field below; one condition variable wakes both the
    // analysis workers (inbox growth / end / cancel) and the executor's
    // result drain (outbox growth / completion / cancel).
    std::mutex verify_mutex;
    std::condition_variable verify_condition;
    std::deque<VerifyFrameItem> verify_inbox;
    std::vector<VerifyFrameResult> verify_outbox;
    std::uint64_t verify_received = 0;
    std::uint64_t verify_completed = 0;
    std::uint64_t verify_failed = 0;
    bool verify_stream_ended = false;
    std::uint64_t verify_declared_total = 0;
    std::optional<std::chrono::steady_clock::time_point> verify_first_frame_at;
    std::string verify_cancel_detail;
    std::shared_ptr<const MappedRing> verify_ring;
    std::vector<std::uint64_t> verify_slot_generations;
};

#if defined(GETNATIVE_HAS_MEDIA)
enum class MediaJobKind : std::uint8_t {
    index,
    frame_window,
    preview,
    asset_batch,
};

struct MediaAssetSpec {
    std::string item_id;
    std::uint64_t frame_index = 0U;
    std::string format = "f32le";
    std::int32_t maximum_dimension = 320;
};

struct MediaJob {
    std::string request_id;
    std::vector<std::string> subscribers;
    std::string job_id;
    MediaJobKind kind = MediaJobKind::index;
    std::string path;
    std::optional<std::string> fingerprint;
    std::optional<std::uint32_t> stream_index;
    std::string cache_directory;
    std::string decoder = "auto";
    std::string target = "frame";
    std::optional<std::uint64_t> frame_index;
    std::optional<double> timestamp_seconds;
    std::uint32_t window_radius = 12U;
    std::int32_t maximum_dimension = 1280;
    std::int32_t expected_width = 0;
    std::int32_t expected_height = 0;
    std::vector<MediaAssetSpec> assets;
    bool accepting_subscribers = true;
    std::atomic<bool> cancel_requested{false};
    std::stop_source stop_source;

    void request_cancel() {
        cancel_requested.store(true, std::memory_order_relaxed);
        stop_source.request_stop();
    }
};
#endif

// Session frame cache. Frame vectors are never resized after load, so the
// ConstImageView pointer handed to backends is stable across jobs — the
// CUDA backend's source-residency cache keys on that pointer.
class FrameCache {
public:
    struct Entry {
        std::vector<float> pixels;
        std::int32_t width = 0;
        std::int32_t height = 0;
    };

    const Entry &get(const FrameAsset &asset) {
        const std::string key = frame_key(asset);
        if (auto found = entries_.find(key); found != entries_.end()) {
            touch(key);
            return found->second;
        }
        Entry entry;
        entry.pixels = load_frame_asset(asset);
        entry.width = asset.width;
        entry.height = asset.height;
        entries_.emplace(key, std::move(entry));
        lru_.push_front(key);
        while (lru_.size() > kMaximumEntries) {
            entries_.erase(lru_.back());
            lru_.pop_back();
        }
        return entries_.at(key);
    }

    [[nodiscard]] bool contains(const FrameAsset &asset) const {
        return entries_.contains(frame_key(asset));
    }

private:
    static constexpr std::size_t kMaximumEntries = 8U;

    static std::string frame_key(const FrameAsset &asset) {
        return asset.path + "#" + std::to_string(asset.width) + "x"
            + std::to_string(asset.height);
    }

    void touch(const std::string &key) {
        lru_.remove(key);
        lru_.push_front(key);
    }

    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;
};

class CpuAnalysisExecutor {
public:
    struct Execution {
        std::vector<CandidateResult> results;
        std::size_t completed = 0;
        std::size_t completed_prefix = 0;
        bool cancelled = false;
    };

    CpuAnalysisExecutor() = default;
    CpuAnalysisExecutor(const CpuAnalysisExecutor &) = delete;
    CpuAnalysisExecutor &operator=(const CpuAnalysisExecutor &) = delete;

    ~CpuAnalysisExecutor() {
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread &worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    Execution analyze(
        ConstImageView source, std::span<const CandidateAnalysis> candidates,
        const MetricSpec &metric, std::size_t worker_count,
        std::stop_token stop_token,
        std::function<void(std::size_t)> progress) {
        const std::scoped_lock submit_lock(submit_mutex_);
        Execution execution;
        execution.results.resize(candidates.size());
        if (candidates.empty()) return execution;
        worker_count = std::max<std::size_t>(1U, std::min(worker_count, candidates.size()));
        ensure_workers(worker_count);
        std::vector<std::atomic<bool>> completed(candidates.size());
        for (auto &flag : completed) flag.store(false, std::memory_order_relaxed);
        Task task{
            source, candidates, metric, worker_count, stop_token, std::move(progress),
            &execution.results, &completed,
        };
        {
            const std::scoped_lock lock(mutex_);
            task_ = &task;
            active_workers_ = workers_.size();
            ++generation_;
        }
        condition_.notify_all();
        {
            std::unique_lock lock(mutex_);
            completion_.wait(lock, [&] { return active_workers_ == 0U; });
            task_ = nullptr;
        }
        if (task.failure) std::rethrow_exception(task.failure);
        execution.completed = task.completed.load(std::memory_order_relaxed);
        while (execution.completed_prefix < candidates.size()
               && completed[execution.completed_prefix].load(std::memory_order_acquire)) {
            ++execution.completed_prefix;
        }
        execution.cancelled = stop_token.stop_requested();
        return execution;
    }

    [[nodiscard]] std::size_t capacity() const {
        const std::scoped_lock lock(mutex_);
        return workers_.size();
    }

private:
    struct Task {
        Task(ConstImageView source_value,
             std::span<const CandidateAnalysis> candidates_value,
             const MetricSpec &metric_value, std::size_t worker_count_value,
             std::stop_token stop_token_value,
             std::function<void(std::size_t)> progress_value,
             std::vector<CandidateResult> *results_value,
             std::vector<std::atomic<bool>> *completed_flags_value)
            : source(source_value), candidates(candidates_value), metric(metric_value),
              worker_count(worker_count_value), stop_token(stop_token_value),
              progress(std::move(progress_value)), results(results_value),
              completed_flags(completed_flags_value) {}

        ConstImageView source;
        std::span<const CandidateAnalysis> candidates;
        const MetricSpec &metric;
        std::size_t worker_count;
        std::stop_token stop_token;
        std::function<void(std::size_t)> progress;
        std::vector<CandidateResult> *results;
        std::vector<std::atomic<bool>> *completed_flags;
        std::atomic_size_t cursor{0U};
        std::atomic_size_t completed{0U};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        std::mutex progress_mutex;
        std::size_t last_reported = 0U;
    };

    mutable std::mutex mutex_;
    std::mutex submit_mutex_;
    std::condition_variable condition_;
    std::condition_variable completion_;
    std::vector<std::thread> workers_;
    Task *task_ = nullptr;
    std::uint64_t generation_ = 0U;
    std::size_t active_workers_ = 0U;
    bool stopping_ = false;

    void ensure_workers(std::size_t count) {
        while (workers_.size() < count) {
            const std::size_t index = workers_.size();
            workers_.emplace_back([this, index] { worker_loop(index); });
        }
    }

    void worker_loop(std::size_t worker_index) {
        CpuWorkspace workspace;
        std::uint64_t observed_generation = 0U;
        while (true) {
            Task *task = nullptr;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return stopping_ || generation_ != observed_generation;
                });
                if (stopping_) return;
                observed_generation = generation_;
                task = task_;
            }
            if (task != nullptr && worker_index < task->worker_count) {
                run_task(*task, workspace);
            }
            {
                const std::scoped_lock lock(mutex_);
                if (active_workers_ > 0U && --active_workers_ == 0U) {
                    completion_.notify_one();
                }
            }
        }
    }

    static void run_task(Task &task, CpuWorkspace &workspace) {
        while (!task.stop_token.stop_requested()) {
            const std::size_t index = task.cursor.fetch_add(1U, std::memory_order_relaxed);
            if (index >= task.candidates.size()) return;
            try {
                const CandidateAnalysis &candidate = task.candidates[index];
                double error = 0.0;
                if (candidate.axes == AnalysisAxes::both) {
                    if (!candidate.horizontal || !candidate.vertical) {
                        throw std::invalid_argument("two-axis candidate contains a null plan");
                    }
                    error = analyze_candidate_f32(
                        task.source, *candidate.horizontal, *candidate.vertical,
                        task.metric, workspace);
                } else {
                    const auto &plan = candidate.axes == AnalysisAxes::horizontal
                        ? candidate.horizontal : candidate.vertical;
                    if (!plan) {
                        throw std::invalid_argument("single-axis candidate contains a null plan");
                    }
                    error = analyze_axis_candidate_f32(
                        task.source, *plan, candidate.axes, task.metric, workspace);
                }
                (*task.results)[index] = {candidate.id, error};
                (*task.completed_flags)[index].store(true, std::memory_order_release);
                const std::size_t done =
                    task.completed.fetch_add(1U, std::memory_order_relaxed) + 1U;
                if (done % kCandidateChunkSize == 0U || done == task.candidates.size()) {
                    const std::scoped_lock lock(task.progress_mutex);
                    if (done > task.last_reported) {
                        task.last_reported = done;
                        task.progress(done);
                    }
                }
            } catch (...) {
                const std::scoped_lock lock(task.failure_mutex);
                if (!task.failure) task.failure = std::current_exception();
                task.cursor.store(task.candidates.size(), std::memory_order_relaxed);
                return;
            }
        }
    }
};

class WorkerSession {
public:
    WorkerSession(std::ostream &output, std::ostream &log)
        : output_(output), log_(log), plan_cache_{} {
        executor_ = std::thread([this] { execute_loop(); });
#if defined(GETNATIVE_HAS_MEDIA)
        media_executor_ = std::thread([this] { media_execute_loop(); });
#endif
        store_writer_ = std::thread([this] { store_write_loop(); });
    }

    ~WorkerSession() {
        request_stop();
        if (executor_.joinable()) executor_.join();
#if defined(GETNATIVE_HAS_MEDIA)
        if (media_executor_.joinable()) media_executor_.join();
#endif
        {
            const std::scoped_lock lock(store_mutex_);
            store_stop_ = true;
        }
        store_condition_.notify_all();
        if (store_writer_.joinable()) store_writer_.join();
    }

    void request_stop() {
        std::deque<std::shared_ptr<Job>> dropped;
#if defined(GETNATIVE_HAS_MEDIA)
        std::deque<std::shared_ptr<MediaJob>> media_dropped;
#endif
        {
            const std::scoped_lock lock(mutex_);
            if (stopping_) {
                dropped.clear();
            } else {
                stopping_ = true;
                dropped = std::move(queue_);
                queue_.clear();
            }
            if (running_) running_->request_cancel();
        }
        condition_.notify_all();
#if defined(GETNATIVE_HAS_MEDIA)
        {
            const std::scoped_lock lock(media_mutex_);
            media_stopping_ = true;
            media_dropped = std::move(media_queue_);
            media_queue_.clear();
            if (media_running_) media_running_->request_cancel();
        }
        media_condition_.notify_all();
#endif
        for (const auto &job : dropped) {
            emit_cancelled(*job, false, "shutdown");
        }
#if defined(GETNATIVE_HAS_MEDIA)
        for (const auto &job : media_dropped) {
            emit_cancelled(job->request_id, job->job_id, "media", false, "shutdown");
        }
#endif
    }

    void hello(const JsonValue &command) {
        const std::string request_id = require_string(command, "request_id");
        if (greeted_) {
            throw WorkerError("protocol_error", "hello was already received");
        }
        greeted_ = true;
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("hello_ok")},
            {"request_id", JsonValue::string(request_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"engine_version", JsonValue::string("0.2.3")},
            {"commands", JsonValue::object({
                {"analyze", JsonValue::boolean(true)},
                {"cancel", JsonValue::boolean(true)},
                {"verify_begin", JsonValue::boolean(true)},
#if defined(GETNATIVE_HAS_MEDIA)
                {"verify_media_begin", JsonValue::boolean(true)},
                {"media_index_begin", JsonValue::boolean(true)},
                {"media_frame_window", JsonValue::boolean(true)},
                {"media_preview_begin", JsonValue::boolean(true)},
                {"media_asset_batch_begin", JsonValue::boolean(true)},
#else
                {"verify_media_begin", JsonValue::boolean(false)},
                {"media_index_begin", JsonValue::boolean(false)},
                {"media_frame_window", JsonValue::boolean(false)},
                {"media_preview_begin", JsonValue::boolean(false)},
                {"media_asset_batch_begin", JsonValue::boolean(false)},
#endif
                {"verify_frame", JsonValue::boolean(true)},
                {"verify_ring_attach", JsonValue::boolean(true)},
                {"verify_end", JsonValue::boolean(true)},
            })},
            {"media_verify_concurrency", JsonValue::object({
                {"min", JsonValue::integer(kMediaVerifyMinimumConcurrency)},
                {"max", JsonValue::integer(kMediaVerifyMaximumConcurrency)},
                {"default", JsonValue::integer(kMediaVerifyDefaultConcurrency)},
                {"gpu_max", JsonValue::integer(kAnalysisEngineExecutionSlots)},
            })},
        }));
    }

    void capabilities(const JsonValue &command) {
        require_greeting();
        const std::string request_id = require_string(command, "request_id");
        std::ostringstream payload;
#if defined(GETNATIVE_HAS_CUDA)
        const CudaAnalysisEngine *resident_cuda = nullptr;
        std::string resident_cuda_error;
        try {
            resident_cuda = &resident_cuda_engine();
        } catch (const std::exception &error) {
            resident_cuda_error = error.what();
        }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        const VulkanAnalysisEngine *resident_vulkan = nullptr;
        std::string resident_vulkan_error;
        try {
            resident_vulkan = &resident_vulkan_engine();
        } catch (const std::exception &error) {
            resident_vulkan_error = error.what();
        }
#endif
#if defined(GETNATIVE_HAS_CUDA) || defined(GETNATIVE_HAS_VULKAN) || defined(GETNATIVE_HAS_METAL)
        write_capabilities(payload, true
#if defined(GETNATIVE_HAS_CUDA)
                           , resident_cuda, resident_cuda_error
#endif
#if defined(GETNATIVE_HAS_VULKAN)
                           , resident_vulkan, resident_vulkan_error
#endif
                           );
#else
        write_capabilities(payload, true);
#endif
        JsonValue parsed = parse_json(payload.str());
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("capabilities")},
            {"request_id", JsonValue::string(request_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"payload", std::move(parsed)},
        }));
    }

    void analyze(const JsonValue &command) {
        require_greeting();
        const std::string job_id = "job-" + std::to_string(next_job_++);
        AnalyzeJobSpec spec = parse_analyze(command, job_id);
        std::string selected_device;
        if (spec.backend == BackendChoice::automatic) {
            AutomaticBackend selected = AutomaticBackend::cpu;
#if defined(GETNATIVE_HAS_CUDA)
            if (choose_automatic_backend(spec.metric.norm, true, false, false)
                == AutomaticBackend::cuda) {
                try {
                    CudaAnalysisEngine &cuda = resident_cuda_engine();
                    selected = AutomaticBackend::cuda;
                    selected_device = cuda.device_info().name;
                } catch (const std::exception &error) {
                    log_ << "worker: auto CUDA initialization failed: "
                         << error.what() << '\n';
                }
            }
#endif
            if (selected == AutomaticBackend::cpu) {
#if defined(GETNATIVE_HAS_VULKAN)
                if (choose_automatic_backend(spec.metric.norm, false, true, true)
                    == AutomaticBackend::vulkan) {
                    try {
                        VulkanAnalysisEngine &vulkan = resident_vulkan_engine();
                        const bool discrete = vulkan.device_info().device_type
                            == VulkanDeviceType::discrete_gpu;
                        selected = choose_automatic_backend(
                            spec.metric.norm, false, true, discrete);
                        if (selected == AutomaticBackend::vulkan) {
                            selected_device = vulkan.device_info().name;
                        } else {
                            log_ << "worker: auto Vulkan device is not discrete; "
                                    "using CPU\n";
                        }
                    } catch (const std::exception &error) {
                        log_ << "worker: auto Vulkan initialization failed; using CPU: "
                             << error.what() << '\n';
                    }
                }
#endif
            }
            spec.backend = selected == AutomaticBackend::cuda
                ? BackendChoice::cuda
                : selected == AutomaticBackend::vulkan
                    ? BackendChoice::vulkan : BackendChoice::cpu;
#if defined(__APPLE__) && defined(GETNATIVE_HAS_METAL)
            if (spec.backend == BackendChoice::cpu
                && spec.metric.norm >= metal_minimum_p_norm
                && spec.metric.norm <= metal_maximum_p_norm) {
                try {
                    MetalAnalysisEngine &metal = resident_metal_engine();
                    spec.backend = BackendChoice::metal;
                    selected_device = metal.device_info().name;
                } catch (const std::exception &error) {
                    log_ << "worker: auto Metal initialization failed; using CPU: "
                         << error.what() << '\n';
                }
            }
#endif
        } else if (spec.backend == BackendChoice::cuda) {
#if defined(GETNATIVE_HAS_CUDA)
            try {
                selected_device = resident_cuda_engine().device_info().name;
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"CUDA backend is not available: "} + error.what());
            }
#else
            throw WorkerError("unsupported", "CUDA backend was not compiled");
#endif
        } else if (spec.backend == BackendChoice::vulkan) {
#if defined(GETNATIVE_HAS_VULKAN)
            try {
                selected_device = resident_vulkan_engine().device_info().name;
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"Vulkan backend is not available: "} + error.what());
            }
#else
            throw WorkerError("unsupported", "Vulkan backend was not compiled");
#endif
        } else if (spec.backend == BackendChoice::metal) {
#if defined(GETNATIVE_HAS_METAL)
            try {
                selected_device = resident_metal_engine().device_info().name;
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"Metal backend is not available: "} + error.what());
            }
#else
            throw WorkerError("unsupported", "Metal backend was not compiled");
#endif
        }
        auto job = std::make_shared<Job>(std::move(spec));
        // Emit accepted BEFORE the job becomes visible to the executor:
        // event order on the wire must place accepted ahead of any job
        // event (a warm plan cache makes plan progress otherwise racy).
        std::vector<std::pair<std::string, JsonValue>> accepted = {
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("accepted")},
            {"request_id", JsonValue::string(job->spec.request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string(job->spec.kernel_mode ? "kernel" : "height")},
            {"backend", JsonValue::string(backend_choice_name(job->spec.backend))},
        };
        if (!selected_device.empty()) {
            accepted.emplace_back("device", JsonValue::string(selected_device));
        }
        emit(JsonValue::object(std::move(accepted)));
        {
            const std::scoped_lock lock(mutex_);
            queue_.push_back(job);
        }
        condition_.notify_one();
    }

    void verify_begin(const JsonValue &command) {
        require_greeting();
        const std::string job_id = "job-" + std::to_string(next_job_++);
        VerifyJobSpec spec = parse_verify_begin(command, job_id);
        const std::size_t workers = effective_verify_workers(spec);
        auto job = std::make_shared<Job>(std::move(spec));
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("accepted")},
            {"request_id", JsonValue::string(job->verify.request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("verify")},
            {"backend", JsonValue::string("cpu")},
            {"worker_count", JsonValue::integer(static_cast<std::int64_t>(workers))},
            {"suggested_in_flight", JsonValue::integer(
                static_cast<std::int64_t>(workers * 2U))},
        }));
        {
            const std::scoped_lock lock(mutex_);
            queue_.push_back(job);
        }
        condition_.notify_one();
    }

#if defined(GETNATIVE_HAS_MEDIA)
    void select_media_verify_backend(VerifyJobSpec &spec) {
        const auto fallback = [&](std::string from, std::string to, std::string reason) {
            spec.fallback_chain.push_back({
                "compute_backend_fallback", std::move(from), std::move(to),
                std::move(reason), 0U});
        };
#if defined(__APPLE__) && defined(GETNATIVE_HAS_METAL)
        constexpr std::string_view after_vulkan = "metal";
#else
        constexpr std::string_view after_vulkan = "cpu";
#endif
        if (spec.requested_backend == BackendChoice::automatic) {
            bool selected = false;
            std::string previous = "cuda";
#if defined(__APPLE__) && defined(GETNATIVE_HAS_METAL)
            bool metal_attempted = false;
            if (spec.metric.norm >= metal_minimum_p_norm
                && spec.metric.norm <= metal_maximum_p_norm) {
                metal_attempted = true;
                try {
                    MetalAnalysisEngine &metal = resident_metal_engine();
                    if (!media::backend_runtime_available(
                            media::DecoderOptions::Backend::videotoolbox)) {
                        throw std::runtime_error(
                            "FFmpeg VideoToolbox hardware decode is unavailable");
                    }
                    spec.backend = BackendChoice::metal;
                    spec.selected_device = metal.device_info().name;
                    selected = true;
                } catch (const std::exception &error) {
                    fallback("metal", "cuda", error.what());
                }
            }
#endif
            if (!selected && spec.metric.norm >= cuda_minimum_p_norm
                && spec.metric.norm <= cuda_maximum_p_norm) {
#if defined(GETNATIVE_HAS_CUDA)
                try {
                    CudaAnalysisEngine &cuda = resident_cuda_engine();
                    spec.backend = BackendChoice::cuda;
                    spec.selected_device = cuda.device_info().name;
                    spec.selected_device_uuid = cuda.device_info().uuid;
                    selected = true;
                } catch (const std::exception &error) {
                    fallback("cuda", "vulkan", error.what());
                }
#else
                fallback("cuda", "vulkan", "CUDA backend was not compiled");
#endif
            } else if (!selected) {
                fallback("cuda", "vulkan", "requested p_norm is unsupported by CUDA");
            }
            if (!selected) {
                previous = "vulkan";
                if (spec.metric.norm >= vulkan_minimum_p_norm
                    && spec.metric.norm <= vulkan_maximum_p_norm) {
#if defined(GETNATIVE_HAS_VULKAN)
                    try {
                        VulkanAnalysisEngine &vulkan = resident_vulkan_engine();
                        if (vulkan.device_info().device_type == VulkanDeviceType::discrete_gpu) {
                            spec.backend = BackendChoice::vulkan;
                            spec.selected_device = vulkan.device_info().name;
                            spec.selected_device_uuid = vulkan.device_info().uuid;
                            selected = true;
                        } else {
                            fallback("vulkan", std::string{after_vulkan},
                                     "automatic Vulkan requires a discrete GPU");
                        }
                    } catch (const std::exception &error) {
                        fallback("vulkan", std::string{after_vulkan}, error.what());
                    }
#else
                    fallback("vulkan", std::string{after_vulkan},
                             "Vulkan backend was not compiled");
#endif
                } else {
                fallback("vulkan", std::string{after_vulkan},
                         "requested p_norm is unsupported by Vulkan");
                }
            }
            if (!selected) {
#if defined(__APPLE__) && defined(GETNATIVE_HAS_METAL)
                previous = "metal";
                if (!metal_attempted && spec.metric.norm >= metal_minimum_p_norm
                    && spec.metric.norm <= metal_maximum_p_norm) {
                    try {
                        MetalAnalysisEngine &metal = resident_metal_engine();
                        if (!media::backend_runtime_available(
                                media::DecoderOptions::Backend::videotoolbox)) {
                            throw std::runtime_error(
                                "FFmpeg VideoToolbox hardware decode is unavailable");
                        }
                        spec.backend = BackendChoice::metal;
                        spec.selected_device = metal.device_info().name;
                        selected = true;
                    } catch (const std::exception &error) {
                        fallback("metal", "cpu", error.what());
                    }
                } else if (!metal_attempted) {
                    fallback("metal", "cpu", "requested p_norm is unsupported by Metal");
                }
#endif
            }
            if (!selected) {
                spec.backend = BackendChoice::cpu;
                (void)previous;
            }
        } else if (spec.requested_backend == BackendChoice::cuda) {
#if defined(GETNATIVE_HAS_CUDA)
            try {
                CudaAnalysisEngine &cuda = resident_cuda_engine();
                spec.backend = BackendChoice::cuda;
                spec.selected_device = cuda.device_info().name;
                spec.selected_device_uuid = cuda.device_info().uuid;
            } catch (const std::exception &error) {
                throw WorkerError("unsupported",
                    std::string{"CUDA backend is not available: "} + error.what());
            }
#else
            throw WorkerError("unsupported", "CUDA backend was not compiled");
#endif
        } else if (spec.requested_backend == BackendChoice::vulkan) {
#if defined(GETNATIVE_HAS_VULKAN)
            try {
                VulkanAnalysisEngine &vulkan = resident_vulkan_engine();
                spec.backend = BackendChoice::vulkan;
                spec.selected_device = vulkan.device_info().name;
                spec.selected_device_uuid = vulkan.device_info().uuid;
            } catch (const std::exception &error) {
                throw WorkerError("unsupported",
                    std::string{"Vulkan backend is not available: "} + error.what());
            }
#else
            throw WorkerError("unsupported", "Vulkan backend was not compiled");
#endif
        } else if (spec.requested_backend == BackendChoice::metal) {
#if defined(__APPLE__) && defined(GETNATIVE_HAS_METAL)
            try {
                MetalAnalysisEngine &metal = resident_metal_engine();
                if (!media::backend_runtime_available(
                        media::DecoderOptions::Backend::videotoolbox)) {
                    throw std::runtime_error(
                        "FFmpeg VideoToolbox hardware decode is unavailable");
                }
                spec.backend = BackendChoice::metal;
                spec.selected_device = metal.device_info().name;
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"Metal Verify is not available: "} + error.what());
            }
#else
            throw WorkerError("unsupported", "Metal Verify was not compiled");
#endif
        } else {
            spec.backend = BackendChoice::cpu;
        }

        if (spec.backend == BackendChoice::cuda) {
            if (!media::backend_runtime_available(
                    media::DecoderOptions::Backend::cuda)) {
                spec.fallback_chain.push_back({
                    "hardware_decode_fallback", "nvdec", "software",
                    "FFmpeg CUDA hardware decode is unavailable", 0U});
            }
        } else if (spec.backend == BackendChoice::vulkan) {
            if (media::preferred_host_hwdec()
                == media::DecoderOptions::Backend::software) {
                spec.fallback_chain.push_back({
                    "hardware_decode_fallback", "host_hwdec", "software",
                    "VAAPI/D3D11VA/NVDEC hardware decode is unavailable", 0U});
            }
        }
    }

    void verify_media_begin(const JsonValue &command) {
        require_greeting();
        const std::string job_id = "job-" + std::to_string(next_job_++);
        VerifyJobSpec spec = parse_verify_begin(command, job_id, true);
        select_media_verify_backend(spec);
        cap_media_verify_concurrency(spec);
        auto job = std::make_shared<Job>(std::move(spec));
        std::vector<std::pair<std::string, JsonValue>> accepted = {
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("accepted")},
            {"request_id", JsonValue::string(job->verify.request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("verify")},
            {"backend", JsonValue::string(backend_choice_name(job->verify.backend))},
            {"concurrency", JsonValue::integer(
                static_cast<std::int64_t>(job->verify.concurrency))},
            {"suggested_in_flight", JsonValue::integer(
                static_cast<std::int64_t>(job->verify.concurrency))},
        };
        if (!job->verify.selected_device.empty()) {
            accepted.emplace_back("device", JsonValue::string(job->verify.selected_device));
        }
        emit(JsonValue::object(std::move(accepted)));
        {
            const std::scoped_lock lock(mutex_);
            queue_.push_back(job);
        }
        condition_.notify_one();
    }

    void media_begin(const JsonValue &command, MediaJobKind kind) {
        require_greeting();
        auto job = std::make_shared<MediaJob>();
        job->request_id = require_string(command, "request_id");
        job->subscribers.push_back(job->request_id);
        job->job_id = "job-" + std::to_string(next_job_++);
        job->kind = kind;
        job->path = require_string(command, "path");
        if (job->path.empty()
            || !std::filesystem::is_regular_file(path_from_utf8(job->path))) {
            throw WorkerError("media_read_error", "media path does not name a regular file");
        }
        job->fingerprint = optional_string(command, "fingerprint");
        job->cache_directory = optional_string(command, "cache_directory")
            .value_or(path_to_utf8(std::filesystem::temp_directory_path()
                                   / "getnative-media-cache"));
        job->decoder = optional_string(command, "decoder").value_or("auto");
        if (!matches_media_decoder(job->decoder)) {
            throw WorkerError("bad_request", "decoder must be auto, software, nvdec, or vulkan_video");
        }
        if (const JsonValue *stream = command.find("stream_index");
            stream != nullptr && !stream->is_null()) {
            const std::int32_t value = require_int(command, "stream_index");
            if (value < 0) throw WorkerError("bad_request", "stream_index must be non-negative");
            job->stream_index = static_cast<std::uint32_t>(value);
        }
        if (kind == MediaJobKind::frame_window || kind == MediaJobKind::preview) {
            job->target = optional_string(command, "target").value_or("frame");
            if (const JsonValue *frame = command.find("frame_index");
                frame != nullptr && !frame->is_null()) {
                const std::int64_t value = require_int64(command, "frame_index");
                if (value < 0) throw WorkerError("bad_request", "frame_index must be non-negative");
                job->frame_index = static_cast<std::uint64_t>(value);
            }
            if (const JsonValue *timestamp = command.find("timestamp_seconds");
                timestamp != nullptr && !timestamp->is_null()) {
                const double value = require_number(command, "timestamp_seconds");
                if (value < 0.0) {
                    throw WorkerError("bad_request", "timestamp_seconds must be non-negative");
                }
                job->timestamp_seconds = value;
            }
            if (!matches_media_target(job->target)) {
                throw WorkerError("bad_request", "unknown media frame target: " + job->target);
            }
        }
        if (kind == MediaJobKind::frame_window || kind == MediaJobKind::preview) {
            // Preview replies embed the window, so it parses a radius too;
            // its default is 0 (clients that want the rail pass one).
            const std::int32_t radius = optional_int(
                command, "window_radius", kind == MediaJobKind::preview ? 0 : 12);
            if (radius < 0 || radius > 1000) {
                throw WorkerError("bad_request", "window_radius must be within 0..1000");
            }
            job->window_radius = static_cast<std::uint32_t>(radius);
        }
        if (kind == MediaJobKind::preview) {
            job->maximum_dimension = optional_int(command, "maximum_dimension", 1280);
            if (job->maximum_dimension < 16 || job->maximum_dimension > 8192) {
                throw WorkerError("bad_request", "maximum_dimension must be within 16..8192");
            }
        }
        if (kind == MediaJobKind::asset_batch) {
            job->expected_width = optional_int(command, "width", 0);
            job->expected_height = optional_int(command, "height", 0);
            const JsonValue &assets = require_member(command, "assets");
            if (assets.type != JsonValue::Type::array || assets.items.empty()
                || assets.items.size() > 26U) {
                throw WorkerError("bad_request", "assets must contain 1..26 entries");
            }
            for (const JsonValue &item : assets.items) {
                if (item.type != JsonValue::Type::object) {
                    throw WorkerError("bad_request", "asset entries must be objects");
                }
                MediaAssetSpec asset;
                asset.item_id = require_string(item, "item_id");
                const std::int64_t frame = require_int64(item, "frame_index");
                if (asset.item_id.empty() || frame < 0) {
                    throw WorkerError("bad_request", "asset item_id and frame_index are invalid");
                }
                asset.frame_index = static_cast<std::uint64_t>(frame);
                asset.format = optional_string(item, "format").value_or("f32le");
                asset.maximum_dimension = optional_int(item, "maximum_dimension", 320);
                if (asset.format != "f32le" && asset.format != "png") {
                    throw WorkerError("bad_request", "asset format must be f32le or png");
                }
                if (asset.maximum_dimension < 16 || asset.maximum_dimension > 8192) {
                    throw WorkerError("bad_request", "asset maximum_dimension must be within 16..8192");
                }
                job->assets.push_back(std::move(asset));
            }
            std::sort(job->assets.begin(), job->assets.end(),
                      [](const MediaAssetSpec &left, const MediaAssetSpec &right) {
                          return left.frame_index < right.frame_index;
                      });
            std::unordered_set<std::string> item_ids;
            for (const MediaAssetSpec &asset : job->assets) {
                if (!item_ids.insert(asset.item_id).second) {
                    throw WorkerError("bad_request", "asset item_id values must be unique");
                }
            }
        }

        const bool png_preview = kind == MediaJobKind::preview
            || (kind == MediaJobKind::asset_batch
                && std::any_of(job->assets.begin(), job->assets.end(),
                               [](const MediaAssetSpec &asset) {
                                   return asset.format == "png";
                               }));
        if (png_preview) {
            if (job->decoder == "nvdec" || job->decoder == "vulkan_video") {
                throw WorkerError(
                    "unsupported",
                    "video previews support only the software decoder");
            }
            job->decoder = "software";
        }

        if (kind == MediaJobKind::index) {
            const std::scoped_lock lock(media_mutex_);
            const auto matches = [&](const std::shared_ptr<MediaJob> &candidate) {
                return candidate->kind == MediaJobKind::index
                    && candidate->accepting_subscribers
                    && candidate->path == job->path
                    && candidate->fingerprint == job->fingerprint
                    && candidate->stream_index == job->stream_index;
            };
            std::shared_ptr<MediaJob> existing;
            if (media_running_ && matches(media_running_)) existing = media_running_;
            if (!existing) {
                const auto found = std::find_if(media_queue_.begin(), media_queue_.end(), matches);
                if (found != media_queue_.end()) existing = *found;
            }
            if (existing) {
                existing->subscribers.push_back(job->request_id);
                emit_media_accepted(job->request_id, existing->job_id, kind);
                return;
            }
        }

        emit_media_accepted(job->request_id, job->job_id, kind);
        {
            const std::scoped_lock lock(media_mutex_);
            media_queue_.push_back(job);
        }
        media_condition_.notify_one();
    }
#endif

    void verify_frame(const JsonValue &command) {
        require_greeting();
        const std::string request_id = require_string(command, "request_id");
        const std::string job_id = require_string(command, "job_id");
        const std::int64_t seq_value = require_int64(command, "seq");
        if (seq_value < 0) {
            throw WorkerError("bad_request", "verify_frame seq must be non-negative");
        }
        const std::shared_ptr<Job> job = find_verify_job(job_id);
        {
            const std::scoped_lock stream_lock(job->verify_mutex);
            if (job->verify_stream_ended) {
                throw WorkerError("bad_request", "verify stream already ended");
            }
            if (static_cast<std::uint64_t>(seq_value) != job->verify_received) {
                throw WorkerError(
                    "bad_request",
                    "verify_frame seq must be contiguous: expected "
                        + std::to_string(job->verify_received)
                        + ", got " + std::to_string(seq_value));
            }
            VerifyFrameItem item;
            item.seq = static_cast<std::uint64_t>(seq_value);
            const JsonValue *slot_value = command.find("slot");
            if (slot_value && !slot_value->is_null()) {
                if (!job->verify_ring) {
                    throw WorkerError("bad_request", "verify ring is not attached");
                }
                const std::int32_t slot = require_int(command, "slot");
                const std::int64_t generation = require_int64(command, "generation");
                if (slot < 0
                    || static_cast<std::uint32_t>(slot) >= job->verify_ring->slot_count()) {
                    throw WorkerError("bad_request", "verify ring slot is out of range");
                }
                if (generation <= 0) {
                    throw WorkerError("bad_request", "verify ring generation must be positive");
                }
                const std::size_t slot_index = static_cast<std::size_t>(slot);
                if (static_cast<std::uint64_t>(generation)
                    <= job->verify_slot_generations[slot_index]) {
                    throw WorkerError("bad_request", "stale verify ring generation");
                }
                job->verify_slot_generations[slot_index] =
                    static_cast<std::uint64_t>(generation);
                item.ring = job->verify_ring;
                item.slot = static_cast<std::uint32_t>(slot);
                item.generation = static_cast<std::uint64_t>(generation);
            } else {
                FrameAsset asset = parse_frame_asset(require_member(command, "frame_asset"));
                if (asset.width != job->verify.width || asset.height != job->verify.height) {
                    throw WorkerError(
                        "bad_request",
                        "verify frame asset geometry does not match the recipe");
                }
                item.asset = std::move(asset);
            }
            job->verify_inbox.push_back(std::move(item));
            if (!job->verify_first_frame_at) {
                job->verify_first_frame_at = std::chrono::steady_clock::now();
            }
            ++job->verify_received;
        }
        job->verify_condition.notify_all();
    }

    void verify_ring_attach(const JsonValue &command) {
        require_greeting();
        require_string(command, "request_id");
        const std::string job_id = require_string(command, "job_id");
        const std::string path = require_string(command, "path");
        const std::int32_t slot_count = require_int(command, "slot_count");
        const std::int64_t frame_bytes = require_int64(command, "frame_bytes");
        if (slot_count < 1 || slot_count > 64 || frame_bytes <= 0) {
            throw WorkerError("bad_request", "invalid verify ring attachment");
        }
        const std::shared_ptr<Job> job = find_verify_job(job_id);
        const std::uint64_t expected_bytes =
            static_cast<std::uint64_t>(job->verify.width)
            * static_cast<std::uint64_t>(job->verify.height) * sizeof(float);
        if (static_cast<std::uint64_t>(frame_bytes) != expected_bytes) {
            throw WorkerError("bad_request", "verify ring frame size does not match geometry");
        }
        auto ring = std::make_shared<MappedRing>(
            path, static_cast<std::uint32_t>(slot_count),
            static_cast<std::uint64_t>(frame_bytes));
        {
            const std::scoped_lock stream_lock(job->verify_mutex);
            if (job->verify_ring || job->verify_received != 0U || job->verify_stream_ended) {
                throw WorkerError("bad_request", "verify ring must attach once before frames");
            }
            job->verify_ring = std::move(ring);
            job->verify_slot_generations.assign(
                static_cast<std::size_t>(slot_count), 0U);
        }
    }

    void verify_end(const JsonValue &command) {
        require_greeting();
        require_string(command, "request_id");
        const std::string job_id = require_string(command, "job_id");
        const std::int64_t total = require_int64(command, "total");
        if (total < 0) {
            throw WorkerError("bad_request", "verify_end total must be non-negative");
        }
        const std::shared_ptr<Job> job = find_verify_job(job_id);
        bool mismatch = false;
        {
            const std::scoped_lock stream_lock(job->verify_mutex);
            if (job->verify_stream_ended) {
                throw WorkerError("bad_request", "verify stream already ended");
            }
            job->verify_stream_ended = true;
            job->verify_declared_total = static_cast<std::uint64_t>(total);
            mismatch = job->verify_declared_total != job->verify_received;
            if (mismatch) {
                job->verify_cancel_detail = "verify_total_mismatch";
            }
        }
        job->verify_condition.notify_all();
        if (mismatch) {
            job->request_cancel();
            throw WorkerError(
                "bad_request",
                "verify_end total does not match the streamed frame count");
        }
    }

    void cancel(const JsonValue &command) {
        require_greeting();
        const std::string request_id = require_string(command, "request_id");
        const std::string job_id = require_string(command, "job_id");
        bool found = false;
        std::shared_ptr<Job> queued;
#if defined(GETNATIVE_HAS_MEDIA)
        std::shared_ptr<MediaJob> media_queued;
#endif
        {
            const std::scoped_lock lock(mutex_);
            if (running_ && running_->id() == job_id) {
                running_->request_cancel();
                found = true;
            } else {
                for (auto iterator = queue_.begin(); iterator != queue_.end(); ++iterator) {
                    if ((*iterator)->id() == job_id) {
                        queued = std::move(*iterator);
                        queue_.erase(iterator);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (queued) {
            emit_cancelled(*queued, false, "cancelled_before_start");
        }
#if defined(GETNATIVE_HAS_MEDIA)
        if (!found) {
            const std::scoped_lock lock(media_mutex_);
            if (media_running_ && media_running_->job_id == job_id) {
                media_running_->request_cancel();
                found = true;
            } else {
                for (auto iterator = media_queue_.begin();
                     iterator != media_queue_.end(); ++iterator) {
                    if ((*iterator)->job_id == job_id) {
                        media_queued = std::move(*iterator);
                        media_queue_.erase(iterator);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (media_queued) {
            for (const std::string &subscriber : media_queued->subscribers) {
                emit_cancelled(subscriber, media_queued->job_id, "media", false,
                               "cancelled_before_start");
            }
        }
#endif
        if (!found) {
            emit(JsonValue::object({
                {"protocol_version", JsonValue::integer(kProtocolVersion)},
                {"type", JsonValue::string("cancelled")},
                {"request_id", JsonValue::string(request_id)},
                {"job_id", JsonValue::string(job_id)},
                {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                {"partial", JsonValue::boolean(false)},
                {"detail", JsonValue::string("not_running")},
            }));
        }
    }

    void shutdown(const JsonValue &command) {
        const std::string request_id = require_string(command, "request_id");
        std::deque<std::shared_ptr<Job>> dropped;
        {
            const std::scoped_lock lock(mutex_);
            stopping_ = true;
            dropped = std::move(queue_);
            queue_.clear();
            if (running_) running_->request_cancel();
        }
        condition_.notify_all();
#if defined(GETNATIVE_HAS_MEDIA)
        std::deque<std::shared_ptr<MediaJob>> media_dropped;
        {
            const std::scoped_lock lock(media_mutex_);
            media_stopping_ = true;
            media_dropped = std::move(media_queue_);
            media_queue_.clear();
            if (media_running_) media_running_->request_cancel();
        }
        media_condition_.notify_all();
#endif
        for (const auto &job : dropped) {
            emit_cancelled(*job, false, "shutdown");
        }
#if defined(GETNATIVE_HAS_MEDIA)
        for (const auto &job : media_dropped) {
            for (const std::string &subscriber : job->subscribers) {
                emit_cancelled(subscriber, job->job_id, "media", false, "shutdown");
            }
        }
#endif
        if (executor_.joinable()) executor_.join();
#if defined(GETNATIVE_HAS_MEDIA)
        if (media_executor_.joinable()) media_executor_.join();
#endif
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("shutdown")},
            {"request_id", JsonValue::string(request_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
        }));
        shutdown_acknowledged_ = true;
    }

    [[nodiscard]] bool shutdown_acknowledged() const noexcept {
        return shutdown_acknowledged_;
    }

    static void emit_static_error(std::ostream &output, const std::string &request_id,
                                  const WorkerError &error) {
        output << JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("error")},
            {"request_id", JsonValue::string(request_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"code", JsonValue::string(error.code())},
            {"message", JsonValue::string(error.what())},
            {"retryable", JsonValue::boolean(error.retryable())},
        }).dump() << '\n';
        output.flush();
    }

private:
    std::ostream &output_;
    std::ostream &log_;
    AxisPlanCache plan_cache_;
    FrameCache frame_cache_;
    CpuAnalysisExecutor cpu_executor_;
#if defined(GETNATIVE_HAS_CUDA)
    std::optional<CudaAnalysisEngine> cuda_engine_;
    std::mutex cuda_init_mutex_;
#endif
#if defined(GETNATIVE_HAS_VULKAN)
    std::optional<VulkanAnalysisEngine> vulkan_engine_;
    std::mutex vulkan_init_mutex_;
#endif
#if defined(GETNATIVE_HAS_METAL)
    std::optional<MetalAnalysisEngine> metal_engine_;
    std::mutex metal_init_mutex_;
#endif
    // L2 cold plan store (E4): lazily opened on the first plan-bearing job;
    // failures disable it for the session (cache degradation, never a job
    // failure). Publishing is write-behind on store_writer_.
    std::optional<PlanStore> plan_store_;
    bool plan_store_attempted_ = false;
    struct PendingGridPublish {
        std::uint64_t grid_hash = 0;
        std::vector<AxisPlanRequest> requests;
        std::vector<std::shared_ptr<const AxisPlan>> plans;
    };
    std::mutex store_mutex_;
    std::condition_variable store_condition_;
    std::deque<PendingGridPublish> store_queue_;
    bool store_stop_ = false;
    std::thread store_writer_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::shared_ptr<Job> running_;
    bool stopping_ = false;
    bool greeted_ = false;
    bool shutdown_acknowledged_ = false;
    std::uint64_t next_job_ = 1;
    std::thread executor_;
    std::mutex emit_mutex_;
#if defined(GETNATIVE_HAS_MEDIA)
    std::mutex media_mutex_;
    std::condition_variable media_condition_;
    std::deque<std::shared_ptr<MediaJob>> media_queue_;
    std::shared_ptr<MediaJob> media_running_;
    bool media_stopping_ = false;
    std::thread media_executor_;
    // Reuse across media jobs. Both are only ever touched on the media
    // executor thread, so they need no locking. The index cache avoids a
    // re-probe + index re-parse per request; the decode session keeps the
    // demuxer/decoder open so repeated seeks only pay a flush.
    std::string media_index_cache_key_;
    std::shared_ptr<const media::IndexedMedia> media_index_cache_;
    std::chrono::steady_clock::time_point media_index_cache_validated_at_{};
    std::string media_session_key_;
    std::unique_ptr<media::IndexedDecodeSession> media_session_;
    // path#stream#backend keys of hardware sessions that already failed to
    // open or decode, so later requests fall back to software immediately.
#endif

    void require_greeting() {
        if (!greeted_) {
            throw WorkerError("protocol_error", "hello must be the first command");
        }
    }

#if defined(GETNATIVE_HAS_MEDIA)
    static bool matches_media_decoder(std::string_view value) {
        return value == "auto" || value == "software" || value == "nvdec"
            || value == "vaapi" || value == "d3d11va" || value == "vulkan_video";
    }

    static bool matches_media_target(std::string_view value) {
        return value == "frame" || value == "timestamp"
            || value == "previous_keyframe" || value == "next_keyframe";
    }

    static const char *media_mode(MediaJobKind kind) {
        switch (kind) {
        case MediaJobKind::index: return "media_index";
        case MediaJobKind::frame_window: return "media_frame_window";
        case MediaJobKind::preview: return "media_preview";
        case MediaJobKind::asset_batch: return "media_asset_batch";
        }
        return "media";
    }

    void emit_media_accepted(const std::string &request_id,
                             const std::string &job_id,
                             MediaJobKind kind) {
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("accepted")},
            {"request_id", JsonValue::string(request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string(media_mode(kind))},
            {"backend", JsonValue::string("media")},
        }));
    }
#endif

    void emit(const JsonValue &event) {
        const std::scoped_lock lock(emit_mutex_);
        output_ << event.dump() << '\n';
        output_.flush();
    }

    void emit_error(const std::string &request_id, const WorkerError &error) {
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("error")},
            {"request_id", JsonValue::string(request_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"code", JsonValue::string(error.code())},
            {"message", JsonValue::string(error.what())},
            {"retryable", JsonValue::boolean(error.retryable())},
        }));
    }

    void emit_progress(const AnalyzeJobSpec &spec, std::string_view phase,
                       std::size_t completed, std::size_t total) {
        emit_progress(spec.request_id, spec.job_id, "height", phase,
                      completed, total, JsonValue::array());
    }

    void emit_progress(const std::string &request_id, const std::string &job_id,
                       const char *mode, std::string_view phase,
                       std::uint64_t completed, std::uint64_t total,
                       JsonValue results, JsonValue coverage = JsonValue{}) {
        std::vector<std::pair<std::string, JsonValue>> members = {
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("progress")},
            {"request_id", JsonValue::string(request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string(mode)},
            {"phase", JsonValue::string(std::string{phase})},
            {"completed", JsonValue::integer(static_cast<std::int64_t>(completed))},
            {"total", JsonValue::integer(static_cast<std::int64_t>(total))},
        };
        if (results.type == JsonValue::Type::array && !results.items.empty()) {
            members.emplace_back("results", std::move(results));
        }
        if (coverage.type == JsonValue::Type::object) {
            members.emplace_back("coverage", std::move(coverage));
        }
        emit(JsonValue::object(std::move(members)));
    }

    void emit_cancelled(const AnalyzeJobSpec &spec, bool partial,
                        std::string_view detail) {
        emit_cancelled(spec.request_id, spec.job_id, "height", partial, detail);
    }

    void emit_cancelled(const Job &job, bool partial, std::string_view detail) {
        emit_cancelled(job.request_id(), job.id(), job.mode(), partial, detail);
    }

    void emit_cancelled(const std::string &request_id, const std::string &job_id,
                        const char *mode, bool partial, std::string_view detail) {
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("cancelled")},
            {"request_id", JsonValue::string(request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string(mode)},
            {"partial", JsonValue::boolean(partial)},
            {"detail", JsonValue::string(std::string{detail})},
        }));
    }

    static void check_cancelled(const Job &job) {
        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            throw WorkerError("cancelled", "job was cancelled");
        }
    }

#if defined(GETNATIVE_HAS_CUDA)
    CudaAnalysisEngine &resident_cuda_engine() {
        const std::scoped_lock lock(cuda_init_mutex_);
        if (!cuda_engine_) {
            log_ << "worker: initializing CUDA analysis engine...\n";
            CudaAnalysisOptions options;
            options.execution_slots = kAnalysisEngineExecutionSlots;
            cuda_engine_.emplace(std::move(options));
            log_ << "worker: CUDA analysis engine initialized on "
                 << cuda_engine_->device_info().name << '\n';
        }
        return *cuda_engine_;
    }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
    VulkanAnalysisEngine &resident_vulkan_engine() {
        const std::scoped_lock lock(vulkan_init_mutex_);
        if (!vulkan_engine_) {
            log_ << "worker: initializing Vulkan analysis engine...\n";
            VulkanAnalysisOptions options;
            options.execution_slots = kAnalysisEngineExecutionSlots;
            vulkan_engine_.emplace(std::move(options));
            log_ << "worker: Vulkan analysis engine initialized on "
                 << vulkan_engine_->device_info().name << '\n';
        }
        return *vulkan_engine_;
    }
#endif
#if defined(GETNATIVE_HAS_METAL)
    MetalAnalysisEngine &resident_metal_engine() {
        const std::scoped_lock lock(metal_init_mutex_);
        if (!metal_engine_) {
            log_ << "worker: initializing Metal analysis engine...\n";
            MetalAnalysisOptions options;
            options.execution_slots = kAnalysisEngineExecutionSlots;
            metal_engine_.emplace(std::move(options));
            log_ << "worker: Metal analysis engine initialized on "
                 << metal_engine_->device_info().name << '\n';
        }
        return *metal_engine_;
    }
#endif

    std::shared_ptr<Job> find_verify_job(const std::string &job_id) {
        const std::scoped_lock lock(mutex_);
        if (running_ && running_->id() == job_id && running_->kind == JobKind::verify) {
            return running_;
        }
        for (const auto &job : queue_) {
            if (job->id() == job_id && job->kind == JobKind::verify) {
                return job;
            }
        }
        throw WorkerError("bad_request", "no active verify job with id: " + job_id);
    }

    // -----------------------------------------------------------------------
    // Cold plan store (E4)
    // -----------------------------------------------------------------------

    PlanStore *plan_store_or_null() {
        if (plan_store_attempted_) {
            return plan_store_ ? &*plan_store_ : nullptr;
        }
        plan_store_attempted_ = true;
        const std::optional<std::filesystem::path> directory = resolve_plan_store_dir();
        if (!directory) return nullptr;
        try {
            plan_store_.emplace(*directory);
            log_ << "worker: plan store at " << path_to_utf8(*directory) << '\n';
        } catch (const std::exception &error) {
            log_ << "worker: plan store disabled: " << error.what() << '\n';
        }
        return plan_store_ ? &*plan_store_ : nullptr;
    }

    // Sparse store reads (E4): resolve one job plan-chunk through the pack's
    // key-hash index, decompressing only the touched zstd chunks. Per-chunk
    // reads keep the L1 working set at chunk size, so over-capacity grids
    // cannot thrash the byte cap the way whole-pack preheat would.
    std::size_t fetch_from_store(std::uint64_t grid_hash,
                                 std::span<const AxisPlanRequest> chunk_requests,
                                 double *fetch_ms_out) {
        PlanStore *store = plan_store_or_null();
        if (store == nullptr || chunk_requests.empty()) return 0U;
        const auto start = std::chrono::steady_clock::now();
        std::optional<std::vector<std::shared_ptr<const AxisPlan>>> stored;
        try {
            stored = store->read_plans(grid_hash, chunk_requests);
        } catch (const std::exception &error) {
            log_ << "worker: plan store read failed: " << error.what() << '\n';
        }
        const double read_ms = elapsed_ms(start);
        *fetch_ms_out += read_ms;
        if (!stored) return 0U;
        const auto publish_start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < chunk_requests.size(); ++index) {
            plan_cache_.publish(chunk_requests[index], (*stored)[index]);
        }
        if (std::getenv("GETNATIVE_STORE_TRACE") != nullptr) {
            log_ << "worker: store fetch read=" << read_ms << "ms publish="
                 << elapsed_ms(publish_start) << "ms plans=" << chunk_requests.size()
                 << '\n';
        }
        return chunk_requests.size();
    }

    // Write-behind publish: the store writer thread owns the actual encode +
    // compress + rename, so job latency never waits on zstd.
    void queue_store_publish(std::span<const AxisPlanRequest> requests,
                             std::span<const std::shared_ptr<const AxisPlan>> plans,
                             std::size_t physical_build_count) {
        if (plan_store_or_null() == nullptr || requests.empty()
            || physical_build_count == 0) {
            return;
        }
        {
            const std::scoped_lock lock(store_mutex_);
            store_queue_.push_back({
                PlanStore::grid_hash(requests),
                {requests.begin(), requests.end()},
                {plans.begin(), plans.end()},
            });
        }
        store_condition_.notify_one();
    }

    // Deduplicate parallel request/plan vectors for grid publishing (the
    // pack index rejects duplicate keys). Field-wise identity is exact here:
    // duplicates within one job arise from identical candidate strings.
    static std::pair<std::vector<AxisPlanRequest>,
                     std::vector<std::shared_ptr<const AxisPlan>>>
    dedupe_requests(const std::vector<AxisPlanRequest> &requests,
                    const std::vector<std::shared_ptr<const AxisPlan>> &plans) {
        const auto less = [&](std::size_t lhs, std::size_t rhs) {
            const AxisPlanRequest &a = requests[lhs];
            const AxisPlanRequest &b = requests[rhs];
            const auto tuple_a = std::tie(
                a.source_size, a.destination_size, a.active_length, a.shift,
                a.filter.type, a.filter.b, a.filter.c, a.filter.taps, a.border);
            const auto tuple_b = std::tie(
                b.source_size, b.destination_size, b.active_length, b.shift,
                b.filter.type, b.filter.b, b.filter.c, b.filter.taps, b.border);
            return tuple_a < tuple_b;
        };
        std::vector<std::size_t> order(requests.size());
        for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
        std::sort(order.begin(), order.end(), less);
        std::vector<AxisPlanRequest> unique_requests;
        std::vector<std::shared_ptr<const AxisPlan>> unique_plans;
        for (const std::size_t index : order) {
            if (!unique_requests.empty()
                && equal_requests(unique_requests.back(), requests[index])) {
                continue;
            }
            unique_requests.push_back(requests[index]);
            unique_plans.push_back(plans[index]);
        }
        return {std::move(unique_requests), std::move(unique_plans)};
    }

    static bool equal_requests(const AxisPlanRequest &a, const AxisPlanRequest &b) {
        return a.source_size == b.source_size
            && a.destination_size == b.destination_size
            && a.active_length == b.active_length && a.shift == b.shift
            && a.filter.type == b.filter.type && a.filter.b == b.filter.b
            && a.filter.c == b.filter.c && a.filter.taps == b.filter.taps
            && a.border == b.border;
    }

    void store_write_loop() {
        while (true) {
            PendingGridPublish pending;
            {
                std::unique_lock lock(store_mutex_);
                store_condition_.wait(lock, [&] {
                    return store_stop_ || !store_queue_.empty();
                });
                if (store_stop_) return;
                pending = std::move(store_queue_.front());
                store_queue_.pop_front();
            }
            try {
                PlanStore *store = plan_store_or_null();
                if (store != nullptr) {
                    store->publish_grid(pending.grid_hash, pending.requests,
                                        pending.plans);
                }
            } catch (const std::exception &error) {
                log_ << "worker: plan store publish failed: " << error.what() << '\n';
            }
        }
    }

    void execute_loop() {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                job = queue_.front();
                queue_.pop_front();
                running_ = job;
                job->started = true;
            }
            try {
                run_job(*job);
            } catch (const WorkerError &error) {
                try {
                    if (error.code() == "cancelled") {
                        // Candidate-phase cancellation returns normally with
                        // partial results; a thrown cancellation means the job
                        // stopped before any results were produced.
                        emit_cancelled(*job, false, "cancelled");
                    } else {
                        emit_error(job->request_id(), error);
                    }
                } catch (...) {
                }
            } catch (const std::exception &error) {
                try {
                    emit_error(job->request_id(),
                               WorkerError("internal", error.what()));
                } catch (...) {
                }
            }
            const std::scoped_lock lock(mutex_);
            running_.reset();
        }
    }

#if defined(GETNATIVE_HAS_MEDIA)
    static JsonValue frame_identity_json(const media::FrameIdentity &frame) {
        return JsonValue::object({
            {"frame_index", JsonValue::integer(static_cast<std::int64_t>(frame.frame_index))},
            {"pts", frame.pts ? JsonValue::integer(*frame.pts) : JsonValue{}},
            {"best_effort_timestamp", frame.best_effort_timestamp
                ? JsonValue::integer(*frame.best_effort_timestamp) : JsonValue{}},
            {"timestamp_seconds", frame.timestamp_seconds
                ? JsonValue::number(*frame.timestamp_seconds) : JsonValue{}},
            {"key_frame", JsonValue::boolean(frame.key_frame)},
            {"picture_type", frame.picture_type
                ? JsonValue::string(*frame.picture_type) : JsonValue{}},
            {"keyframe_anchor", JsonValue::integer(
                static_cast<std::int64_t>(frame.keyframe_anchor))},
        });
    }

    static std::uint64_t resolve_media_target(const media::MediaIndex &index,
                                              const MediaJob &job) {
        if (index.frames.empty()) throw WorkerError("media_index_error", "media index is empty");
        const std::uint64_t last = index.frames.back().frame_index;
        if (job.target == "timestamp") {
            if (!job.timestamp_seconds) {
                throw WorkerError("bad_request", "timestamp target requires timestamp_seconds");
            }
            const media::FrameIdentity *best = nullptr;
            double best_distance = std::numeric_limits<double>::infinity();
            for (const auto &frame : index.frames) {
                if (!frame.timestamp_seconds) continue;
                const double distance = std::abs(*frame.timestamp_seconds - *job.timestamp_seconds);
                if (distance < best_distance) {
                    best = &frame;
                    best_distance = distance;
                }
            }
            if (best == nullptr) {
                throw WorkerError("media_index_error",
                                  "indexed frames do not contain usable timestamps");
            }
            return best->frame_index;
        }
        const std::uint64_t target = std::min(job.frame_index.value_or(0U), last);
        if (job.target == "previous_keyframe") {
            for (auto iterator = index.frames.rbegin(); iterator != index.frames.rend(); ++iterator) {
                // Strictly before: when the current frame is itself a keyframe
                // an inclusive bound would resolve to it and the seek no-ops.
                if (iterator->key_frame && iterator->frame_index < target) {
                    return iterator->frame_index;
                }
            }
            return 0U;
        }
        if (job.target == "next_keyframe") {
            for (const auto &frame : index.frames) {
                if (frame.key_frame && frame.frame_index > target) return frame.frame_index;
            }
            return last;
        }
        return target;
    }

    media::IndexedMedia ensure_media_index(MediaJob &job) {
        auto build = [&](const media::DecoderOptions &options) {
            return media::ensure_index(
                job.path, job.stream_index, job.cache_directory, options,
                job.stop_source.get_token(), [&](std::uint64_t decoded) {
                    if (decoded % 64U != 0U) return;
                    for (const std::string &request_id : media_subscriber_snapshot(job)) {
                        emit_progress(request_id, job.job_id, media_mode(job.kind),
                                      "index", decoded, 0U, JsonValue::array());
                    }
                });
        };
        media::DecoderOptions software;
        media::IndexedMedia indexed = build(software);
        validate_media_fingerprint(job, indexed.index);
        return indexed;
    }

    // Media-index cache. Seeking the same file issues several media jobs per
    // second; without this each one re-probes the container and re-parses the
    // whole index file. Rate-limited size/mtime checks guard against the source
    // changing without turning every NAS seek into a network metadata round
    // trip. Only called on the media executor thread.
    //
    // Explicit index jobs always observe the on-disk index: their contract
    // (rebuilt flag, corrupt-index recovery) describes the file itself.
    const media::IndexedMedia &ensure_media_index_cached(MediaJob &job) {
        const std::string key = job.path + "#"
            + (job.stream_index ? std::to_string(*job.stream_index)
                                : std::string{"auto"});
        const auto now = std::chrono::steady_clock::now();
        constexpr auto signature_ttl = std::chrono::seconds{5};
        if (job.kind != MediaJobKind::index && media_index_cache_
            && media_index_cache_key_ == key) {
            validate_media_fingerprint(job, media_index_cache_->index);
            // SMB metadata queries can block for hundreds of milliseconds (or
            // until a reconnect timeout). One validation covers a burst of
            // scrub/preview requests; explicit index jobs still bypass this.
            if (now - media_index_cache_validated_at_ < signature_ttl) {
                return *media_index_cache_;
            }
            if (media_index_cache_->index.source_size
                    == std::filesystem::file_size(path_from_utf8(job.path))
                && media_index_cache_->index.source_mtime_ns
                    == media::source_mtime_unix_ns(job.path)) {
                media_index_cache_validated_at_ = now;
                return *media_index_cache_;
            }
        }
        media::IndexedMedia indexed = ensure_media_index(job);
        media_index_cache_ =
            std::make_shared<const media::IndexedMedia>(std::move(indexed));
        media_index_cache_key_ = key;
        media_index_cache_validated_at_ = now;
        // A (re)built index invalidates the decode session's index reference.
        media_session_.reset();
        media_session_key_.clear();
        return *media_index_cache_;
    }

    static void validate_media_fingerprint(const MediaJob &job,
                                           const media::MediaIndex &index) {
        if (job.fingerprint && *job.fingerprint != index.fingerprint) {
            throw WorkerError(
                "media_fingerprint_error",
                "the source fingerprint changed after the media request was submitted");
        }
    }

    // The frame-window payload surrounding a target frame; shared by the
    // frame_window job and the preview job (which embeds it so the browser
    // rail does not need a second round trip per seek).
    static JsonValue media_frame_window_payload(const media::MediaIndex &index,
                                                std::uint64_t target,
                                                std::uint32_t radius) {
        std::vector<media::FrameIdentity> frames =
            media::frame_window(index, target, radius);
        std::vector<JsonValue> values;
        values.reserve(frames.size());
        for (const auto &frame : frames) values.push_back(frame_identity_json(frame));
        JsonValue previous;
        JsonValue next;
        for (const auto &frame : index.frames) {
            if (!frame.key_frame) continue;
            if (frame.frame_index < target) previous = frame_identity_json(frame);
            if (frame.frame_index > target && next.is_null()) next = frame_identity_json(frame);
        }
        return JsonValue::object({
            {"selected", frame_identity_json(index.frames[target])},
            {"frames", JsonValue::array(std::move(values))},
            {"total_frames", JsonValue::integer(
                static_cast<std::int64_t>(index.frames.size()))},
            {"previous_keyframe", std::move(previous)},
            {"next_keyframe", std::move(next)},
            {"indexed_complete", JsonValue::boolean(true)},
        });
    }

    // Returns the persistent software decode session for this source/mode,
    // creating or replacing it when the key changed. Preview frame identity
    // must use the same software reference semantics as the media index.
    media::IndexedDecodeSession &media_session_for(MediaJob &job, bool needs_luma,
                                                   bool needs_rgb,
                                                   std::int32_t preview_dimension,
                                                   std::string &backend_used) {
        const media::MediaIndex &index = media_index_cache_->index;
        const std::string base_key = media_index_cache_key_ + "#"
            + std::to_string(index.stream_index)
            + (needs_luma ? "#luma" : "") + (needs_rgb ? "#rgb" : "")
            + "#d" + std::to_string(preview_dimension);
        const std::string key = base_key + "#software";
        if (media_session_ && media_session_key_ == key) {
            backend_used = "software";
            return *media_session_;
        }
        media::DecoderOptions options;
        options.output_luma = needs_luma;
        options.output_rgb = needs_rgb;
        options.preview_maximum_dimension = preview_dimension;
        // Aliasing shared_ptr: the session keeps the cached index (and
        // therefore the cache entry) alive.
        std::shared_ptr<const media::MediaIndex> index_ref(
            media_index_cache_, &media_index_cache_->index);
        media_session_ = std::make_unique<media::IndexedDecodeSession>(
            job.path, std::move(index_ref), options);
        media_session_key_ = key;
        backend_used = "software";
        return *media_session_;
    }

    static std::string media_cache_stem(const media::MediaIndex &index,
                                        std::uint64_t frame_index,
                                        std::string_view suffix) {
        std::string fingerprint = index.fingerprint;
        std::replace_if(fingerprint.begin(), fingerprint.end(),
                        [](unsigned char value) { return !std::isalnum(value); }, '_');
        if (fingerprint.size() > 96U) fingerprint.resize(96U);
        return fingerprint + "-s" + std::to_string(index.stream_index)
            + "-f" + std::to_string(frame_index) + "-v"
            + std::to_string(media::MediaIndex::format_version) + std::string{suffix};
    }

    static constexpr std::uint32_t media_preview_pipeline_version = 2U;

    static std::optional<std::pair<std::int32_t, std::int32_t>>
    media_png_dimensions(const std::filesystem::path &path) {
        std::array<std::uint8_t, 24> header{};
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char *>(header.data()),
                        static_cast<std::streamsize>(header.size()))) {
            return std::nullopt;
        }
        constexpr std::array<std::uint8_t, 8> signature{
            0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
        if (!std::equal(signature.begin(), signature.end(), header.begin())
            || header[12] != 'I' || header[13] != 'H'
            || header[14] != 'D' || header[15] != 'R') {
            return std::nullopt;
        }
        const auto read_be32 = [&](std::size_t offset) {
            return (static_cast<std::uint32_t>(header[offset]) << 24U)
                | (static_cast<std::uint32_t>(header[offset + 1U]) << 16U)
                | (static_cast<std::uint32_t>(header[offset + 2U]) << 8U)
                | static_cast<std::uint32_t>(header[offset + 3U]);
        };
        const std::uint32_t width = read_be32(16U);
        const std::uint32_t height = read_be32(20U);
        if (width == 0U || height == 0U
            || width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        return std::pair{static_cast<std::int32_t>(width),
                         static_cast<std::int32_t>(height)};
    }

    static void write_media_asset(const std::filesystem::path &path,
                                  std::span<const std::uint8_t> bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("failed to create media cache file");
            output.write(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) throw std::runtime_error("failed to write media cache file");
            output.close();
            std::filesystem::rename(temporary, path);
        } catch (...) {
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    static JsonValue media_index_payload(const media::IndexedMedia &indexed) {
        const media::MediaIndex &index = indexed.index;
        const double duration = index.duration_ticks > 0
            && index.time_base_den != 0
            ? static_cast<double>(index.duration_ticks)
                * static_cast<double>(index.time_base_num)
                / static_cast<double>(index.time_base_den)
            : 0.0;
        return JsonValue::object({
            {"kind", JsonValue::string("video")},
            {"state", JsonValue::string("ready")},
            {"fingerprint", JsonValue::string(index.fingerprint)},
            {"size_bytes", JsonValue::integer(static_cast<std::int64_t>(index.source_size))},
            {"stream_index", JsonValue::integer(index.stream_index)},
            {"codec", JsonValue::string(index.codec)},
            {"width", JsonValue::integer(index.width)},
            {"height", JsonValue::integer(index.height)},
            {"duration_seconds", duration > 0.0 ? JsonValue::number(duration) : JsonValue{}},
            {"frame_count", JsonValue::integer(
                static_cast<std::int64_t>(index.frames.size()))},
            {"time_base_num", JsonValue::integer(index.time_base_num)},
            {"time_base_den", JsonValue::integer(index.time_base_den)},
            {"decoder", JsonValue::string(index.decoder)},
            {"index_mode", JsonValue::string(index.index_mode)},
            {"seek_method", JsonValue::string(index.seek_method)},
            {"selective_decodes", JsonValue::integer(
                static_cast<std::int64_t>(index.selective_decodes))},
            {"packet_count", JsonValue::integer(
                static_cast<std::int64_t>(index.packet_count))},
            {"index_ms", JsonValue::number(index.index_ms)},
            {"index_path", JsonValue::string(indexed.index_path)},
            {"index_version", JsonValue::integer(media::MediaIndex::format_version)},
            {"rebuilt", JsonValue::boolean(indexed.rebuilt)},
        });
    }

    void emit_media_result(MediaJob &job, const JsonValue &payload) {
        for (const std::string &request_id : finish_media_subscribers(job)) {
            emit(JsonValue::object({
                {"protocol_version", JsonValue::integer(kProtocolVersion)},
                {"type", JsonValue::string("result")},
                {"request_id", JsonValue::string(request_id)},
                {"job_id", JsonValue::string(job.job_id)},
                {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                {"mode", JsonValue::string(media_mode(job.kind))},
                {"payload", payload},
            }));
        }
    }

    std::vector<std::string> media_subscriber_snapshot(const MediaJob &job) {
        const std::scoped_lock lock(media_mutex_);
        return job.subscribers;
    }

    std::vector<std::string> finish_media_subscribers(MediaJob &job) {
        const std::scoped_lock lock(media_mutex_);
        job.accepting_subscribers = false;
        return job.subscribers;
    }

    void run_media_job(MediaJob &job) {
        const media::IndexedMedia &indexed = ensure_media_index_cached(job);
        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            throw WorkerError("cancelled", "media job was cancelled");
        }
        if (job.kind == MediaJobKind::index) {
            for (const std::string &request_id : media_subscriber_snapshot(job)) {
                emit_progress(request_id, job.job_id, media_mode(job.kind), "index",
                              indexed.index.frames.size(), indexed.index.frames.size(),
                              JsonValue::array());
            }
            emit_media_result(job, media_index_payload(indexed));
            return;
        }

        if (job.kind == MediaJobKind::frame_window) {
            const std::uint64_t target = resolve_media_target(indexed.index, job);
            JsonValue payload = media_frame_window_payload(
                indexed.index, target, job.window_radius);
            payload.members.emplace_back("index_path", JsonValue::string(indexed.index_path));
            emit_media_result(job, std::move(payload));
            return;
        }

        std::vector<MediaAssetSpec> assets = job.assets;
        std::uint64_t preview_target = 0U;
        if (job.kind == MediaJobKind::preview) {
            preview_target = resolve_media_target(indexed.index, job);
            assets = {{"preview", preview_target, "png", job.maximum_dimension}};
        }
        for (const MediaAssetSpec &asset : assets) {
            if (asset.frame_index >= indexed.index.frames.size()) {
                throw WorkerError("bad_request", "requested frame is outside the media index");
            }
        }
        const std::filesystem::path output_directory =
            path_from_utf8(job.cache_directory) / "media-assets";

        struct PreparedAsset {
            std::filesystem::path output_path;
            std::int32_t width = 0;
            std::int32_t height = 0;
            bool needs_decode = false;
        };
        std::vector<PreparedAsset> prepared;
        prepared.reserve(assets.size());
        const std::int32_t source_width = indexed.index.width;
        const std::int32_t source_height = indexed.index.height;
        if (job.kind == MediaJobKind::asset_batch
            && ((job.expected_width != 0 && job.expected_width != source_width)
                || (job.expected_height != 0 && job.expected_height != source_height))) {
            throw WorkerError(
                "media_decode_error",
                "indexed frame geometry does not match the requested asset geometry");
        }
        for (const MediaAssetSpec &asset : assets) {
            PreparedAsset item;
            if (asset.format == "png") {
                item.output_path = output_directory / media_cache_stem(
                    indexed.index, asset.frame_index,
                    "-pv" + std::to_string(media_preview_pipeline_version)
                        + "-d" + std::to_string(asset.maximum_dimension) + ".png");
                const double scale = std::min(
                    1.0, static_cast<double>(asset.maximum_dimension)
                        / static_cast<double>(std::max(source_width, source_height)));
                item.width = std::max(
                    1, static_cast<std::int32_t>(std::llround(source_width * scale)));
                item.height = std::max(
                    1, static_cast<std::int32_t>(std::llround(source_height * scale)));
            } else {
                item.width = job.expected_width != 0 ? job.expected_width : source_width;
                item.height = job.expected_height != 0 ? job.expected_height : source_height;
                item.output_path = output_directory / media_cache_stem(
                    indexed.index, asset.frame_index,
                    "-" + std::to_string(item.width) + "x"
                        + std::to_string(item.height) + ".f32le");
            }
            item.needs_decode = !std::filesystem::is_regular_file(item.output_path);
            if (!item.needs_decode && asset.format == "png") {
                if (const auto dimensions = media_png_dimensions(item.output_path)) {
                    item.width = dimensions->first;
                    item.height = dimensions->second;
                }
            }
            prepared.push_back(std::move(item));
        }

        std::vector<media::FrameIdentity> selected;
        for (std::size_t i = 0; i < assets.size(); ++i) {
            if (!prepared[i].needs_decode) continue;
            if (selected.empty() || selected.back().frame_index != assets[i].frame_index) {
                selected.push_back(indexed.index.frames[assets[i].frame_index]);
            }
        }
        std::sort(selected.begin(), selected.end(),
                  [](const media::FrameIdentity &left, const media::FrameIdentity &right) {
                      return left.frame_index < right.frame_index;
                  });
        selected.erase(std::unique(selected.begin(), selected.end(),
                                   [](const media::FrameIdentity &left,
                                      const media::FrameIdentity &right) {
                                       return left.frame_index == right.frame_index;
                                   }),
                       selected.end());
        media::DecodeTelemetry telemetry;
        // Luma only when an f32le asset needs it; pure-PNG requests skip the
        // full-resolution float conversion entirely.
        bool needs_rgb = false;
        bool needs_luma = false;
        std::int32_t preview_dimension = 0;
        for (std::size_t i = 0; i < assets.size(); ++i) {
            if (!prepared[i].needs_decode) continue;
            if (assets[i].format == "png") {
                needs_rgb = true;
                preview_dimension = std::max(
                    preview_dimension, assets[i].maximum_dimension);
            }
            else needs_luma = true;
        }
        std::vector<bool> produced(assets.size(), false);
        std::string backend_used = "software";
        if (!selected.empty()) {
            auto consume = [&](const media::HostFrame &frame) {
                for (std::size_t i = 0; i < assets.size(); ++i) {
                    const MediaAssetSpec &asset = assets[i];
                    PreparedAsset &item = prepared[i];
                    if (asset.frame_index != frame.identity.frame_index) continue;
                    if (asset.format == "png") {
                        item.width = frame.width;
                        item.height = frame.height;
                        const double scale = std::min(
                            1.0, static_cast<double>(asset.maximum_dimension)
                                / static_cast<double>(std::max(frame.width, frame.height)));
                        item.width = std::max(
                            1, static_cast<std::int32_t>(std::llround(frame.width * scale)));
                        item.height = std::max(
                            1, static_cast<std::int32_t>(std::llround(frame.height * scale)));
                        if (!std::filesystem::is_regular_file(item.output_path)) {
                            media::PreviewImage preview = media::encode_preview_png(
                                frame, asset.maximum_dimension);
                            item.width = preview.width;
                            item.height = preview.height;
                            write_media_asset(item.output_path, preview.png);
                        }
                    } else {
                        if ((job.expected_width != 0 && job.expected_width != frame.width)
                            || (job.expected_height != 0 && job.expected_height != frame.height)) {
                            throw WorkerError(
                                "media_decode_error",
                                "decoded frame geometry does not match the requested asset geometry");
                        }
                        item.width = frame.width;
                        item.height = frame.height;
                        if (!std::filesystem::is_regular_file(item.output_path)) {
                            const std::span<const std::uint8_t> bytes{
                                reinterpret_cast<const std::uint8_t *>(frame.pixels.data()),
                                frame.pixels.size() * sizeof(float)};
                            write_media_asset(item.output_path, bytes);
                        }
                    }
                    produced[i] = true;
                    const std::size_t completed = static_cast<std::size_t>(
                        std::count(produced.begin(), produced.end(), true));
                    for (const std::string &request_id : media_subscriber_snapshot(job)) {
                        emit_progress(request_id, job.job_id, media_mode(job.kind),
                                      "decode", completed, assets.size(), JsonValue::array());
                    }
                }
            };
            std::string session_backend;
            media::IndexedDecodeSession *session =
                &media_session_for(job, needs_luma, needs_rgb,
                                   needs_luma ? 0 : preview_dimension,
                                   session_backend);
            session->decode(selected, job.stop_source.get_token(), consume, &telemetry);
            backend_used = session_backend;
        }
        std::vector<JsonValue> output_items;
        output_items.reserve(assets.size());
        for (std::size_t i = 0; i < assets.size(); ++i) {
            const MediaAssetSpec &asset = assets[i];
            const PreparedAsset &item = prepared[i];
            output_items.push_back(JsonValue::object({
                {"item_id", JsonValue::string(asset.item_id)},
                {"frame_index", JsonValue::integer(
                    static_cast<std::int64_t>(asset.frame_index))},
                {"path", JsonValue::string(path_to_utf8(item.output_path))},
                {"format", JsonValue::string(asset.format)},
                {"width", JsonValue::integer(item.width)},
                {"height", JsonValue::integer(item.height)},
                {"from_cache", JsonValue::boolean(!item.needs_decode)},
            }));
        }
        JsonValue payload = JsonValue::object({
            {"assets", JsonValue::array(std::move(output_items))},
            {"decoded_frames", JsonValue::integer(
                static_cast<std::int64_t>(telemetry.decoded_frames))},
            {"decode_retries", JsonValue::integer(
                static_cast<std::int64_t>(telemetry.decode_retries))},
            {"discarded_packets", JsonValue::integer(
                static_cast<std::int64_t>(telemetry.discarded_packets))},
            {"decoder", JsonValue::string(backend_used)},
            {"index_path", JsonValue::string(indexed.index_path)},
            {"index_version", JsonValue::integer(media::MediaIndex::format_version)},
        });
        if (job.kind == MediaJobKind::preview) {
            const JsonValue *items = payload.find("assets");
            JsonValue first = items != nullptr && !items->items.empty()
                ? items->items.front() : JsonValue{};
            payload = JsonValue::object({
                {"asset", std::move(first)},
                {"decoded_frames", JsonValue::integer(
                    static_cast<std::int64_t>(telemetry.decoded_frames))},
                {"decode_retries", JsonValue::integer(
                    static_cast<std::int64_t>(telemetry.decode_retries))},
                {"discarded_packets", JsonValue::integer(
                    static_cast<std::int64_t>(telemetry.discarded_packets))},
                {"decoder", JsonValue::string(backend_used)},
                // One round trip per seek: the browser rail's frame window
                // rides along with the preview image.
                {"window", media_frame_window_payload(
                    indexed.index, preview_target, job.window_radius)},
                {"index_path", JsonValue::string(indexed.index_path)},
                {"index_version", JsonValue::integer(media::MediaIndex::format_version)},
            });
        }
        emit_media_result(job, payload);
    }

    void media_execute_loop() {
        while (true) {
            std::shared_ptr<MediaJob> job;
            {
                std::unique_lock lock(media_mutex_);
                media_condition_.wait(lock, [&] {
                    return media_stopping_ || !media_queue_.empty();
                });
                if (media_stopping_) return;
                job = media_queue_.front();
                media_queue_.pop_front();
                media_running_ = job;
            }
            try {
                // A job cancelled while queued must not pay for an index load
                // or decoder open before the cancellation is honored.
                if (job->cancel_requested.load(std::memory_order_relaxed)) {
                    throw WorkerError("cancelled", "media job was cancelled");
                }
                run_media_job(*job);
            } catch (const WorkerError &error) {
                for (const std::string &request_id : finish_media_subscribers(*job)) {
                    if (error.code() == "cancelled") {
                        emit_cancelled(request_id, job->job_id, media_mode(job->kind),
                                       false, "cancelled");
                    } else {
                        emit(JsonValue::object({
                            {"protocol_version", JsonValue::integer(kProtocolVersion)},
                            {"type", JsonValue::string("error")},
                            {"request_id", JsonValue::string(request_id)},
                            {"job_id", JsonValue::string(job->job_id)},
                            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                            {"code", JsonValue::string(error.code())},
                            {"message", JsonValue::string(error.what())},
                            {"retryable", JsonValue::boolean(error.retryable())},
                        }));
                    }
                }
            } catch (const std::exception &error) {
                const bool cancelled = job->cancel_requested.load(std::memory_order_relaxed)
                    || std::string_view{error.what()} == "cancelled";
                for (const std::string &request_id : finish_media_subscribers(*job)) {
                    if (cancelled) {
                        emit_cancelled(request_id, job->job_id, media_mode(job->kind),
                                       false, "cancelled");
                    } else {
                        const char *code = job->kind == MediaJobKind::index
                            || job->kind == MediaJobKind::frame_window
                            ? "media_index_error" : "media_decode_error";
                        emit(JsonValue::object({
                            {"protocol_version", JsonValue::integer(kProtocolVersion)},
                            {"type", JsonValue::string("error")},
                            {"request_id", JsonValue::string(request_id)},
                            {"job_id", JsonValue::string(job->job_id)},
                            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                            {"code", JsonValue::string(code)},
                            {"message", JsonValue::string(error.what())},
                            {"retryable", JsonValue::boolean(false)},
                        }));
                    }
                }
            }
            const std::scoped_lock lock(media_mutex_);
            media_running_.reset();
        }
    }
#endif

    // -----------------------------------------------------------------------
    // Job execution
    // -----------------------------------------------------------------------

    void run_job(Job &job) {
        if (job.kind == JobKind::verify) {
#if defined(GETNATIVE_HAS_MEDIA)
            if (job.verify.media) {
                run_media_verify_job(job);
                return;
            }
#endif
            run_verify_job(job);
            return;
        }
        run_analyze_job(job);
    }

    void run_analyze_job(Job &job) {
        const AnalyzeJobSpec &spec = job.spec;
        const double worker_queue_ms = elapsed_ms(job.queued_at);
        const auto job_start = std::chrono::steady_clock::now();

        const bool asset_cache_hit = frame_cache_.contains(spec.frame);
        const auto asset_start = std::chrono::steady_clock::now();
        const FrameCache::Entry &frame_entry = frame_cache_.get(spec.frame);
        const double asset_wait_ms = elapsed_ms(asset_start);
        ConstImageView source{
            frame_entry.pixels.data(), frame_entry.width, frame_entry.height,
            frame_entry.width};

        const std::int32_t primary_size =
            spec.axis_mode == AxisMode::width_only ? source.width : source.height;

        // Candidate axis requests (primary axis follows the candidate grid;
        // the secondary axis in h_plus_w mode is derived from the aspect
        // ratio, matching the standard integer-width rule). Kernel mode maps
        // each kernel filter onto the single fixed axis value instead.
        std::vector<double> values;
        values.reserve(spec.candidates.size());
        const auto generated = spec.grid
            ? generate_candidate_range(*spec.grid, profile(spec.profile).default_grid)
            : std::vector<Candidate>{};
        for (std::size_t index = 0; index < spec.candidates.size(); ++index) {
            const std::string &decimal = spec.candidates[index];
            const double value = spec.grid ? generated[index].value : parse_json(decimal).number_value;
            if (value >= static_cast<double>(primary_size)) {
                throw WorkerError(
                    "bad_request",
                    "candidate " + decimal + " must be below the source axis length");
            }
            values.push_back(value);
        }

        const std::size_t result_count = spec.kernel_mode
            ? spec.kernel_filters.size() : values.size();
        std::vector<AxisPlanRequest> requests;
        requests.reserve(spec.axis_mode == AxisMode::height_plus_width
                             ? result_count * 2U
                             : result_count);
        const auto geometry_for = [&](double value) {
            try {
                if (spec.kernel_mode && spec.geometry) {
                    return *spec.geometry;
                }
                if (!spec.profile_geometry) {
                    const double active_width = spec.axis_mode == AxisMode::width_only
                        ? value
                        : spec.axis_mode == AxisMode::height_plus_width
                            ? static_cast<double>(source.width) * value / static_cast<double>(source.height)
                            : static_cast<double>(source.width);
                    const double active_height = spec.axis_mode == AxisMode::width_only
                        ? static_cast<double>(source.height) : value;
                    return Geometry{
                        python_int(active_width), python_int(active_height), 0.0, 0.0,
                        active_width, active_height,
                    };
                }
                const GeometryAxisMode axis_mode = spec.axis_mode == AxisMode::height_only
                    ? GeometryAxisMode::height_only
                    : spec.axis_mode == AxisMode::width_only
                        ? GeometryAxisMode::width_only
                        : GeometryAxisMode::height_plus_width;
                return resolve_candidate_geometry(
                    source.width, source.height, axis_mode, value,
                    spec.base_height, spec.base_width);
            } catch (const std::exception &error) {
                throw WorkerError("bad_request", std::string{"invalid geometry: "} + error.what());
            }
        };
        const auto make_request = [&](double value, const Filter &filter, const Geometry &geometry,
                                      bool horizontal) {
            AxisPlanRequest request;
            request.source_size = horizontal ? source.width : source.height;
            const std::int64_t destination_size = horizontal ? geometry.width : geometry.height;
            request.active_length = horizontal ? geometry.src_width : geometry.src_height;
            request.shift = horizontal ? geometry.src_left : geometry.src_top;
            request.filter = filter;
            request.border = BorderMode::mirror;
            if (destination_size < 2
                || destination_size > std::numeric_limits<std::int32_t>::max()
                || destination_size >= request.source_size
                || !std::isfinite(request.active_length) || !std::isfinite(request.shift)
                || request.active_length <= 0.0) {
                throw WorkerError("bad_request", "candidate geometry is outside the source bounds");
            }
            request.destination_size = static_cast<std::int32_t>(destination_size);
            (void)value;
            return request;
        };
        if (spec.kernel_mode) {
            const Geometry geometry = geometry_for(values.front());
            for (const Filter &filter : spec.kernel_filters) {
                requests.push_back(make_request(values.front(), filter, geometry,
                                                spec.axis_mode == AxisMode::width_only));
            }
        } else {
            for (const double value : values) {
                const Geometry geometry = geometry_for(value);
                requests.push_back(make_request(value, spec.filter, geometry,
                                                spec.axis_mode == AxisMode::width_only));
            }
        }
        if (spec.axis_mode == AxisMode::height_plus_width) {
            // The geometry resolver has already applied base parity. Both axes
            // use the same immutable request shape on CPU and CUDA.
            const auto append_secondary = [&](double value, const Geometry &geometry,
                                              const Filter &filter) {
                requests.push_back(make_request(value, filter, geometry,
                                                spec.axis_mode == AxisMode::height_plus_width));
            };
            if (spec.kernel_mode) {
                const Geometry geometry = geometry_for(values.front());
                for (const Filter &filter : spec.kernel_filters) {
                    append_secondary(values.front(), geometry, filter);
                }
            } else {
                for (const double value : values) {
                    const Geometry geometry = geometry_for(value);
                    append_secondary(value, geometry, spec.filter);
                }
            }
        }

        // Plan phase: one parallel prefetch of the whole grid from the cold
        // store, then per-chunk top-up reads for any entries the L1 byte cap
        // evicted (the sparse path that keeps over-capacity grids from
        // thrashing), then the L1 batch (which builds whatever remains).
        const auto plan_start = std::chrono::steady_clock::now();
        double store_fetch_ms = 0.0;
        std::size_t store_hits = 0;
        const std::uint64_t grid_hash =
            plan_store_or_null() != nullptr ? PlanStore::grid_hash(requests) : 0U;
        if (grid_hash != 0U) {
            // Publish in reverse so the earliest-consumed plans are the
            // newest LRU entries; over-capacity grids then evict the
            // latest-consumed plans instead of the not-yet-used ones.
            const std::vector<AxisPlanRequest> reversed(requests.rbegin(),
                                                        requests.rend());
            store_hits += fetch_from_store(
                grid_hash, {reversed.data(), reversed.size()}, &store_fetch_ms);
        }
        std::vector<std::shared_ptr<const AxisPlan>> plans;
        plans.reserve(requests.size());
        std::size_t cache_hits = 0;
        std::size_t builds = 0;
        for (std::size_t begin = 0; begin < requests.size(); begin += kPlanChunkSize) {
            check_cancelled(job);
            const std::size_t end = std::min(begin + kPlanChunkSize, requests.size());
            const std::span<const AxisPlanRequest> chunk{requests.data() + begin,
                                                         end - begin};
            if (grid_hash != 0U) {
                const auto found = plan_cache_.lookup_batch(chunk);
                std::vector<AxisPlanRequest> evicted;
                for (std::size_t index = 0; index < chunk.size(); ++index) {
                    if (!found[index]) evicted.push_back(chunk[index]);
                }
                if (!evicted.empty()) {
                    store_hits += fetch_from_store(grid_hash, evicted, &store_fetch_ms);
                }
            }
            AxisPlanCacheBatchResult batch = plan_cache_.get_or_build_batch(chunk);
            cache_hits += batch.ready_hit_count;
            builds += batch.physical_build_count;
            plans.insert(plans.end(),
                         std::make_move_iterator(batch.plans.begin()),
                         std::make_move_iterator(batch.plans.end()));
            emit_progress(spec, "plan", plans.size(), requests.size());
        }
        const double plan_ms = elapsed_ms(plan_start);
        if (builds > 0) {
            const auto [unique_requests, unique_plans] = dedupe_requests(requests, plans);
            queue_store_publish(unique_requests, unique_plans, builds);
        }

        // Candidate phase. Results are indexed by candidate position so the
        // accelerator path can run chunks through a pipeline of threads: the
        // engine's slot pool overlaps chunk N's device execution with chunk
        // N+1's host pack + upload (the unique-candidate-scan wall measured
        // in docs/performance/e3-kernel-increments-20260808.md §2).
        const auto candidates_start = std::chrono::steady_clock::now();
        std::vector<CandidateResult> results(result_count);

#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda) {
            try {
                (void)resident_cuda_engine();
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"CUDA backend is not available: "} + error.what());
            }
            cuda_engine_->reset_analysis_telemetry();
        }
#else
        if (spec.backend == BackendChoice::cuda) {
            throw WorkerError("unsupported", "CUDA backend was not compiled");
        }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        if (spec.backend == BackendChoice::vulkan) {
            try {
                (void)resident_vulkan_engine();
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"Vulkan backend is not available: "} + error.what());
            }
            vulkan_engine_->reset_analysis_telemetry();
        }
#else
        if (spec.backend == BackendChoice::vulkan) {
            throw WorkerError("unsupported", "Vulkan backend was not compiled");
        }
#endif
#if defined(GETNATIVE_HAS_METAL)
        if (spec.backend == BackendChoice::metal) {
            resident_metal_engine().reset_analysis_telemetry();
        }
#endif
#if defined(GETNATIVE_HAS_METAL)
        if (spec.backend == BackendChoice::metal) {
            try {
                (void)resident_metal_engine();
            } catch (const std::exception &error) {
                throw WorkerError(
                    "unsupported",
                    std::string{"Metal backend is not available: "} + error.what());
            }
            metal_engine_->reset_analysis_telemetry();
        }
#else
        if (spec.backend == BackendChoice::metal) {
            throw WorkerError("unsupported", "Metal backend was not compiled");
        }
#endif

        const auto build_chunk = [&](std::size_t chunk_index,
                                     std::vector<CandidateAnalysis> &chunk) {
            const std::size_t begin = chunk_index * kCandidateChunkSize;
            const std::size_t end =
                std::min(begin + kCandidateChunkSize, result_count);
            chunk.clear();
            chunk.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                CandidateAnalysis candidate;
                // Kernel-mode result ids are the kernel's index into the
                // request's ordered kernels list; the payload echoes each
                // parsed kernel spec, so ids stay unambiguous even with a
                // duplicated (b, c) grid.
                candidate.id = spec.kernel_mode
                    ? std::to_string(index) : spec.candidates[index];
                if (spec.axis_mode == AxisMode::width_only) {
                    candidate.horizontal = plans[index];
                    candidate.axes = AnalysisAxes::horizontal;
                } else if (spec.axis_mode == AxisMode::height_only) {
                    candidate.vertical = plans[index];
                    candidate.axes = AnalysisAxes::vertical;
                } else {
                    candidate.vertical = plans[index];
                    candidate.horizontal = plans[result_count + index];
                    candidate.axes = AnalysisAxes::both;
                }
                chunk.push_back(std::move(candidate));
            }
        };

        const std::size_t chunk_total =
            (result_count + kCandidateChunkSize - 1U) / kCandidateChunkSize;
        std::size_t completed = 0U;
        std::size_t cpu_worker_count = 0U;
        const bool accelerator_backend =
            spec.backend == BackendChoice::cuda
            || spec.backend == BackendChoice::vulkan
            || spec.backend == BackendChoice::metal;
        const auto analyze_accelerator_chunk = [&] (
            const std::vector<CandidateAnalysis> &chunk)
            -> std::vector<CandidateResult> {
            (void)chunk;
#if defined(GETNATIVE_HAS_CUDA)
            if (spec.backend == BackendChoice::cuda) {
                return cuda_engine_->analyze_axis_batch_f32(
                    source, chunk, spec.metric, job.stop_source.get_token(),
                    gpu_stage_profile_from_environment());
            }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
            if (spec.backend == BackendChoice::vulkan) {
                return vulkan_engine_->analyze_axis_batch_f32(
                    source, chunk, spec.metric, job.stop_source.get_token(),
                    gpu_stage_profile_from_environment());
            }
#endif
#if defined(GETNATIVE_HAS_METAL)
            if (spec.backend == BackendChoice::metal) {
                return metal_engine_->analyze_axis_batch_f32(
                    source, chunk, spec.metric, job.stop_source.get_token(),
                    {}, gpu_stage_profile_from_environment());
            }
#endif
            throw WorkerError("unsupported", "accelerator backend is unavailable");
        };

        if (spec.backend == BackendChoice::cpu) {
            std::vector<CandidateAnalysis> all_candidates;
            all_candidates.reserve(result_count);
            for (std::size_t chunk_index = 0; chunk_index < chunk_total; ++chunk_index) {
                std::vector<CandidateAnalysis> chunk;
                build_chunk(chunk_index, chunk);
                all_candidates.insert(
                    all_candidates.end(),
                    std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
            }
            std::int32_t maximum_half_bandwidth = 0;
            for (const CandidateAnalysis &candidate : all_candidates) {
                if (candidate.horizontal) {
                    maximum_half_bandwidth = std::max(
                        maximum_half_bandwidth, candidate.horizontal->half_bandwidth);
                }
                if (candidate.vertical) {
                    maximum_half_bandwidth = std::max(
                        maximum_half_bandwidth, candidate.vertical->half_bandwidth);
                }
            }
            const std::size_t hardware =
                std::max<std::size_t>(1U, std::thread::hardware_concurrency());
            const std::size_t automatic_cap = maximum_half_bandwidth <= 2 ? 8U : 16U;
            cpu_worker_count = std::min(
                {spec.worker_count != 0U ? spec.worker_count : automatic_cap,
                 hardware, result_count});
            CpuAnalysisExecutor::Execution execution = cpu_executor_.analyze(
                source, all_candidates, spec.metric, cpu_worker_count,
                job.stop_source.get_token(),
                [&](std::size_t done) {
                    emit_progress(spec, "candidates", done, result_count);
                });
            results = std::move(execution.results);
            if (execution.cancelled
                || job.cancel_requested.load(std::memory_order_relaxed)) {
                results.resize(execution.completed_prefix);
                emit_partial_cancelled(spec, results);
                return;
            }
            completed = execution.completed;
        }
#if defined(GETNATIVE_HAS_METAL)
        else if (spec.backend == BackendChoice::metal) {
            std::vector<CandidateAnalysis> all_candidates;
            all_candidates.reserve(result_count);
            for (std::size_t chunk_index = 0; chunk_index < chunk_total; ++chunk_index) {
                std::vector<CandidateAnalysis> chunk;
                build_chunk(chunk_index, chunk);
                all_candidates.insert(
                    all_candidates.end(),
                    std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
            }
            try {
                results = resident_metal_engine().analyze_axis_batch_f32(
                    source, all_candidates, spec.metric, job.stop_source.get_token(),
                    [&](std::size_t done, std::size_t total) {
                        emit_progress(spec, "candidates", done, total);
                    },
                    gpu_stage_profile_from_environment());
            } catch (const WorkerError &) {
                throw;
            } catch (const std::exception &error) {
                check_cancelled(job);
                const std::string_view what{error.what()};
                if (what.find("cancelled") != std::string_view::npos) {
                    throw WorkerError("cancelled", error.what());
                }
                throw;
            }
            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                emit_partial_cancelled(spec, results);
                return;
            }
            completed = results.size();
            emit_progress(spec, "candidates", completed, result_count);
        }
#endif

        else if (accelerator_backend && chunk_total > 1U) {
            // Pipeline depth: worker_count when given (1..8), else 3.
            // After the pack-path rework the knee moved: with host pack
            // cheap, depth 3 covers the remaining pack/upload latency
            // (301 candidates, 1080p h_plus_w, median: lanczos8 273/166/152
            // ms at p1/p2/p3, p4 163 — deeper regresses on contention;
            // bicubic 59/42/41/41). Chunk size stays 32: 64 coarsens
            // overlap granularity and regresses both depths.
            const std::size_t requested = spec.worker_count != 0U
                ? std::max<std::size_t>(1U, std::min<std::size_t>(8U, spec.worker_count))
                : 3U;
            const std::size_t thread_count = std::min(requested, chunk_total);
            std::atomic_size_t cursor{0U};
            std::atomic_size_t done_candidates{0U};
            std::mutex progress_mutex;
            std::vector<std::atomic<bool>> chunk_done(chunk_total);
            for (auto &flag : chunk_done) flag.store(false, std::memory_order_relaxed);
            std::exception_ptr failure;
            std::mutex failure_mutex;
            {
                std::vector<std::thread> pipeline;
                pipeline.reserve(thread_count);
                for (std::size_t worker = 0; worker < thread_count; ++worker) {
                    pipeline.emplace_back([&] {
                        while (true) {
                            const std::size_t chunk_index =
                                cursor.fetch_add(1U, std::memory_order_relaxed);
                            if (chunk_index >= chunk_total) return;
                            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                                return;
                            }
                            try {
                                std::vector<CandidateAnalysis> chunk;
                                build_chunk(chunk_index, chunk);
                                const std::size_t begin =
                                    chunk_index * kCandidateChunkSize;
                                std::vector<CandidateResult> chunk_results =
                                    analyze_accelerator_chunk(chunk);
                                for (std::size_t offset = 0;
                                     offset < chunk_results.size(); ++offset) {
                                    results[begin + offset] =
                                        std::move(chunk_results[offset]);
                                }
                                chunk_done[chunk_index].store(
                                    true, std::memory_order_release);
                                {
                                    // Keep concurrent chunk progress monotonic on
                                    // the wire; completion and emission are one
                                    // ordered operation for statistics consumers.
                                    const std::scoped_lock lock(progress_mutex);
                                    const std::size_t done = done_candidates.fetch_add(
                                        chunk_results.size(), std::memory_order_relaxed)
                                        + chunk_results.size();
                                    emit_progress(spec, "candidates", done,
                                                  result_count);
                                }
                            } catch (...) {
                                if (job.cancel_requested.load(
                                        std::memory_order_relaxed)) {
                                    return; // cooperative cancellation, not a failure
                                }
                                const std::scoped_lock lock(failure_mutex);
                                if (!failure) failure = std::current_exception();
                                cursor.store(chunk_total, std::memory_order_relaxed);
                                return;
                            }
                        }
                    });
                }
                for (std::thread &thread : pipeline) thread.join();
            }
            if (failure) std::rethrow_exception(failure);
            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                // Contiguous-prefix partial results in candidate order.
                std::vector<CandidateResult> partial;
                for (std::size_t chunk_index = 0; chunk_index < chunk_total;
                     ++chunk_index) {
                    if (!chunk_done[chunk_index].load(std::memory_order_acquire)) {
                        break;
                    }
                    const std::size_t begin = chunk_index * kCandidateChunkSize;
                    const std::size_t end = std::min(
                        begin + kCandidateChunkSize, result_count);
                    for (std::size_t index = begin; index < end; ++index) {
                        partial.push_back(std::move(results[index]));
                    }
                }
                emit_partial_cancelled(spec, partial);
                return;
            }
            completed = result_count;
        } else if (accelerator_backend) {
            for (std::size_t chunk_index = 0; chunk_index < chunk_total;
                 ++chunk_index) {
                std::vector<CandidateAnalysis> chunk;
                build_chunk(chunk_index, chunk);
                std::vector<CandidateResult> chunk_results;
                try {
                    chunk_results = analyze_accelerator_chunk(chunk);
                } catch (...) {
                    check_cancelled(job);
                    throw;
                }
                const std::size_t begin = chunk_index * kCandidateChunkSize;
                for (std::size_t offset = 0; offset < chunk_results.size(); ++offset) {
                    results[begin + offset] = std::move(chunk_results[offset]);
                }
                completed += chunk_results.size();
                if (job.cancel_requested.load(std::memory_order_relaxed)) {
                    results.resize(completed);
                    emit_partial_cancelled(spec, results);
                    return;
                }
                emit_progress(spec, "candidates", completed, result_count);
            }
        }
        results.resize(completed);
        const double candidates_ms = elapsed_ms(candidates_start);

        const char *backend_name = backend_choice_name(spec.backend);
        std::vector<std::pair<std::string, JsonValue>> telemetry_members = {
            {"plan_build_count", JsonValue::integer(static_cast<std::int64_t>(builds))},
            {"plan_cache_hits", JsonValue::integer(static_cast<std::int64_t>(cache_hits))},
            {"plan_store_hits", JsonValue::integer(static_cast<std::int64_t>(store_hits))},
            {"plan_store_fetch_ms", JsonValue::number(store_fetch_ms)},
            {"plan_resident_entries", JsonValue::integer(
                static_cast<std::int64_t>(plan_cache_.size()))},
            {"plan_cache_misses", JsonValue::integer(static_cast<std::int64_t>(builds))},
            {"asset_cache_hits", JsonValue::integer(asset_cache_hit ? 1 : 0)},
            {"asset_cache_misses", JsonValue::integer(asset_cache_hit ? 0 : 1)},
            {"worker_queue_ms", JsonValue::number(worker_queue_ms)},
            {"asset_wait_ms", JsonValue::number(asset_wait_ms)},
            {"plan_ms", JsonValue::number(plan_ms)},
            {"candidate_ms", JsonValue::number(candidates_ms)},
            {"candidates_ms", JsonValue::number(candidates_ms)},
            {"job_total_ms", JsonValue::number(elapsed_ms(job_start))},
            {"total_ms", JsonValue::number(elapsed_ms(job_start))},
            {"backend", JsonValue::string(backend_name)},
            {"isa", JsonValue::string(std::string{
                cpu_isa_name(cpu_dispatch_info().selected)})},
        };
        if (spec.backend == BackendChoice::cpu) {
            telemetry_members.emplace_back(
                "worker_count", JsonValue::integer(
                    static_cast<std::int64_t>(cpu_worker_count)));
            telemetry_members.emplace_back(
                "cpu_executor_capacity", JsonValue::integer(
                    static_cast<std::int64_t>(cpu_executor_.capacity())));
        }
#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda) {
            const CudaRuntimeTelemetry cuda = cuda_engine_->runtime_telemetry();
            telemetry_members.emplace_back(
                "cuda_source_upload_bytes",
                JsonValue::integer(static_cast<std::int64_t>(cuda.source_upload_bytes)));
            telemetry_members.emplace_back(
                "cuda_plan_upload_bytes",
                JsonValue::integer(static_cast<std::int64_t>(cuda.plan_upload_bytes)));
            telemetry_members.emplace_back(
                "cuda_source_cache_hits",
                JsonValue::integer(static_cast<std::int64_t>(cuda.source_cache_hits)));
            telemetry_members.emplace_back(
                "cuda_source_cache_misses",
                JsonValue::integer(static_cast<std::int64_t>(cuda.source_cache_misses)));
            telemetry_members.emplace_back(
                "cuda_plan_cache_hits",
                JsonValue::integer(static_cast<std::int64_t>(cuda.plan_cache_hits)));
            telemetry_members.emplace_back(
                "cuda_plan_cache_misses",
                JsonValue::integer(static_cast<std::int64_t>(cuda.plan_cache_misses)));
            telemetry_members.emplace_back(
                "cuda_host_plan_cache_hits",
                JsonValue::integer(static_cast<std::int64_t>(cuda.host_plan_cache_hits)));
            telemetry_members.emplace_back(
                "cuda_host_plan_cache_misses",
                JsonValue::integer(static_cast<std::int64_t>(cuda.host_plan_cache_misses)));
            telemetry_members.emplace_back(
                "cuda_host_plan_cache_bytes",
                JsonValue::integer(static_cast<std::int64_t>(cuda.host_plan_cache_bytes)));
            telemetry_members.emplace_back(
                "cuda_source_upload_count",
                JsonValue::integer(static_cast<std::int64_t>(cuda.source_upload_count)));
            telemetry_members.emplace_back(
                "cuda_source_transpose_count",
                JsonValue::integer(static_cast<std::int64_t>(cuda.source_transpose_count)));
            telemetry_members.emplace_back(
                "cuda_kernel_ms", JsonValue::number(cuda.kernel_ms));
            telemetry_members.emplace_back(
                "cuda_gpu_total_ms", JsonValue::number(cuda.gpu_total_ms));
            telemetry_members.emplace_back(
                "cuda_host_pack_ms", JsonValue::number(cuda.host_pack_ms));
            telemetry_members.emplace_back(
                "cuda_source_staging_ms",
                JsonValue::number(cuda.source_staging_ms));
            telemetry_members.emplace_back(
                "cuda_source_upload_ms",
                JsonValue::number(cuda.source_upload_ms));
            telemetry_members.emplace_back(
                "cuda_plan_upload_ms", JsonValue::number(cuda.plan_upload_ms));
            telemetry_members.emplace_back(
                "cuda_source_transpose_ms",
                JsonValue::number(cuda.source_transpose_ms));
            telemetry_members.emplace_back(
                "cuda_inverse_horizontal_ms",
                JsonValue::number(cuda.inverse_horizontal_ms));
            telemetry_members.emplace_back(
                "cuda_inverse_vertical_ms",
                JsonValue::number(cuda.inverse_vertical_ms));
            telemetry_members.emplace_back(
                "cuda_forward_intermediate_ms",
                JsonValue::number(cuda.forward_intermediate_ms));
            telemetry_members.emplace_back(
                "cuda_metric_ms", JsonValue::number(cuda.metric_ms));
            telemetry_members.emplace_back(
                "cuda_result_readback_ms",
                JsonValue::number(cuda.result_readback_ms));
            telemetry_members.emplace_back(
                "cuda_execution_slot_wait_ms",
                JsonValue::number(cuda.execution_slot_wait_ms));
            telemetry_members.emplace_back(
                "cuda_device", JsonValue::string(cuda_engine_->device_info().name));
        }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        if (spec.backend == BackendChoice::vulkan) {
            const VulkanRuntimeTelemetry vulkan =
                vulkan_engine_->runtime_telemetry();
            telemetry_members.emplace_back(
                "vulkan_command_buffer_submissions",
                JsonValue::integer(static_cast<std::int64_t>(
                    vulkan.command_buffer_submission_count)));
            telemetry_members.emplace_back(
                "vulkan_kernel_dispatches",
                JsonValue::integer(static_cast<std::int64_t>(
                    vulkan.kernel_dispatch_count)));
            telemetry_members.emplace_back(
                "vulkan_tiles",
                JsonValue::integer(static_cast<std::int64_t>(vulkan.tile_count)));
            telemetry_members.emplace_back(
                "vulkan_source_upload_bytes",
                JsonValue::integer(static_cast<std::int64_t>(
                    vulkan.source_upload_bytes)));
            telemetry_members.emplace_back(
                "vulkan_plan_upload_bytes",
                JsonValue::integer(static_cast<std::int64_t>(
                    vulkan.plan_upload_bytes)));
            telemetry_members.emplace_back(
                "vulkan_result_readback_bytes",
                JsonValue::integer(static_cast<std::int64_t>(
                    vulkan.result_readback_bytes)));
            telemetry_members.emplace_back(
                "vulkan_host_pack_ms", JsonValue::number(vulkan.host_pack_ms));
            telemetry_members.emplace_back(
                "vulkan_gpu_execution_ms",
                JsonValue::number(vulkan.gpu_execution_ms));
            telemetry_members.emplace_back(
                "vulkan_execution_slot_wait_ms",
                JsonValue::number(vulkan.execution_slot_wait_ms));
            telemetry_members.emplace_back(
                "vulkan_device",
                JsonValue::string(vulkan_engine_->device_info().name));
        }
#endif
#if defined(GETNATIVE_HAS_METAL)
        if (spec.backend == BackendChoice::metal) {
            const MetalRuntimeTelemetry metal = metal_engine_->runtime_telemetry();
            telemetry_members.emplace_back(
                "metal_source_direct_write_bytes", JsonValue::integer(
                    static_cast<std::int64_t>(metal.source_direct_write_bytes)));
            telemetry_members.emplace_back(
                "metal_source_legacy_copy_bytes", JsonValue::integer(
                    static_cast<std::int64_t>(metal.source_legacy_copy_bytes)));
            telemetry_members.emplace_back(
                "metal_plan_direct_write_bytes", JsonValue::integer(
                    static_cast<std::int64_t>(metal.plan_direct_write_bytes)));
            telemetry_members.emplace_back(
                "metal_plan_legacy_copy_bytes", JsonValue::integer(
                    static_cast<std::int64_t>(metal.plan_legacy_copy_bytes)));
            telemetry_members.emplace_back(
                "metal_source_pack_ms", JsonValue::number(metal.source_pack_ms));
            telemetry_members.emplace_back(
                "metal_plan_pack_ms", JsonValue::number(metal.plan_pack_ms));
            telemetry_members.emplace_back(
                "metal_external_source_zero_copy",
                JsonValue::boolean(metal.external_source_zero_copy));
            telemetry_members.emplace_back(
                "metal_shared_uma_path", JsonValue::boolean(metal.shared_uma_path));
            telemetry_members.emplace_back(
                "metal_buffer_reuse_count", JsonValue::integer(
                    static_cast<std::int64_t>(metal.working_buffer_reuse_count)));
            telemetry_members.emplace_back(
                "metal_fallback_reason", metal.fallback_reason.empty()
                    ? JsonValue{} : JsonValue::string(metal.fallback_reason));
            telemetry_members.emplace_back(
                "metal_device",
                JsonValue::string(metal_engine_->device_info().name));
        }
#endif
        std::vector<JsonValue> candidate_values;
        candidate_values.reserve(results.size());
        for (const CandidateResult &result : results) {
            std::vector<std::pair<std::string, JsonValue>> entry = {
                {"id", JsonValue::string(result.id)},
                {"error", JsonValue::number(result.error)},
            };
            if (spec.kernel_mode) {
                const std::size_t kernel_index =
                    static_cast<std::size_t>(std::stoull(result.id));
                entry.emplace_back(
                    "kernel", filter_to_json(spec.kernel_filters[kernel_index]));
            }
            candidate_values.push_back(JsonValue::object(std::move(entry)));
        }
        const char *mode_name = spec.kernel_mode ? "kernel" : "height";
        std::vector<std::pair<std::string, JsonValue>> payload = {
            {"mode", JsonValue::string(mode_name)},
            {"candidates", JsonValue::array(std::move(candidate_values))},
            {"telemetry", JsonValue::object(std::move(telemetry_members))},
        };
        if (spec.kernel_mode) {
            payload.emplace_back("candidate",
                                 JsonValue::string(spec.candidates.front()));
        }
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("result")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string(mode_name)},
            {"payload", JsonValue::object(std::move(payload))},
        }));
    }

    // -----------------------------------------------------------------------
    // Verify job execution (protocol v1.1)
    // -----------------------------------------------------------------------

#if defined(GETNATIVE_HAS_MEDIA)
    void emit_verify_fallback(const VerifyJobSpec &spec,
                              const VerifyJobSpec::Fallback &fallback) {
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("warning")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"code", JsonValue::string(fallback.code)},
            {"message", JsonValue::string(
                fallback.from + " -> " + fallback.to + ": " + fallback.reason)},
            {"from", JsonValue::string(fallback.from)},
            {"to", JsonValue::string(fallback.to)},
            {"reason", JsonValue::string(fallback.reason)},
            {"frame_seq", JsonValue::integer(
                static_cast<std::int64_t>(fallback.frame_seq))},
        }));
    }

    [[nodiscard]] JsonValue verify_fallbacks_json(const VerifyJobSpec &spec) {
        std::vector<JsonValue> values;
        values.reserve(spec.fallback_chain.size());
        for (const auto &fallback : spec.fallback_chain) {
            values.push_back(JsonValue::object({
                {"code", JsonValue::string(fallback.code)},
                {"from", JsonValue::string(fallback.from)},
                {"to", JsonValue::string(fallback.to)},
                {"reason", JsonValue::string(fallback.reason)},
                {"frame_seq", JsonValue::integer(
                    static_cast<std::int64_t>(fallback.frame_seq))},
            }));
        }
        return JsonValue::array(std::move(values));
    }

    [[nodiscard]] static const char *scan_selection_name(
        media::ScanSelection selection) noexcept {
        switch (selection) {
        case media::ScanSelection::all: return "all";
        case media::ScanSelection::decoded_i_picture: return "decoded_i_picture";
        case media::ScanSelection::every_n: return "every_n";
        }
        return "all";
    }

    [[nodiscard]] static std::uint64_t eligible_frame_count(
        const media::MediaIndex &index, const media::ScanScope &scope) {
        const std::uint64_t start = scope.start_frame.value_or(0U);
        const std::uint64_t end = scope.end_frame.value_or(
            index.frames.empty() ? 0U : index.frames.back().frame_index);
        return static_cast<std::uint64_t>(std::count_if(
            index.frames.begin(), index.frames.end(),
            [start, end](const media::FrameIdentity &frame) {
                return frame.frame_index >= start && frame.frame_index <= end;
            }));
    }

    [[nodiscard]] static JsonValue verify_coverage_json(
        const media::ScanScope &scope, std::uint64_t eligible_frames,
        std::uint64_t selected_frames, std::uint64_t processed_frames,
        std::uint64_t failed_frames) {
        return JsonValue::object({
            {"selection", JsonValue::string(scan_selection_name(scope.selection))},
            {"eligible_frames", JsonValue::integer(
                static_cast<std::int64_t>(eligible_frames))},
            {"selected_frames", JsonValue::integer(
                static_cast<std::int64_t>(selected_frames))},
            {"processed_frames", JsonValue::integer(
                static_cast<std::int64_t>(processed_frames))},
            {"failed_frames", JsonValue::integer(
                static_cast<std::int64_t>(failed_frames))},
        });
    }

    void run_media_verify_job(Job &job) {
        VerifyJobSpec &spec = job.verify;
        const VerifyJobSpec::MediaInput &input = *spec.media;
        const double worker_queue_ms = elapsed_ms(job.queued_at);
        const auto job_start = std::chrono::steady_clock::now();

        for (const auto &fallback : spec.fallback_chain) {
            emit_verify_fallback(spec, fallback);
        }
        if (spec.requested_concurrency > spec.concurrency) {
            emit(JsonValue::object({
                {"protocol_version", JsonValue::integer(kProtocolVersion)},
                {"type", JsonValue::string("warning")},
                {"request_id", JsonValue::string(spec.request_id)},
                {"job_id", JsonValue::string(spec.job_id)},
                {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                {"code", JsonValue::string("concurrency_clamped")},
                {"message", JsonValue::string(
                    "GPU analysis slots cap media verification concurrency at "
                    + std::to_string(spec.concurrency))},
            }));
        }
        check_cancelled(job);


        const auto index_start = std::chrono::steady_clock::now();
        media::MediaIndex index;
        media::DecoderOptions cuda_decoder_options;
        media::DecoderOptions metal_decoder_options;
        media::DecoderOptions host_hw_options;
        cuda_decoder_options.frame_concurrency = spec.concurrency;
        metal_decoder_options.frame_concurrency = spec.concurrency;
        host_hw_options.frame_concurrency = spec.concurrency;
        bool use_cuda_decode = false;
        bool use_metal_decode = false;
        bool use_host_hw_decode = false;
#if !defined(GETNATIVE_HAS_CUDA)
        (void)use_cuda_decode;
#endif
#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda
            && media::backend_runtime_available(
                media::DecoderOptions::Backend::cuda)) {
            const bool already_fell_back = std::any_of(
                spec.fallback_chain.begin(), spec.fallback_chain.end(),
                [](const VerifyJobSpec::Fallback &fallback) {
                    return fallback.code == "hardware_decode_fallback";
                });
            if (!already_fell_back) {
                CudaAnalysisEngine &cuda = resident_cuda_engine();
                cuda_decoder_options.backend = media::DecoderOptions::Backend::cuda;
                cuda_decoder_options.native_context = cuda.native_context();
                cuda_decoder_options.native_queue = cuda.native_decode_stream();
            }
        }
#endif
        if (spec.backend == BackendChoice::vulkan) {
            const bool already_fell_back = std::any_of(
                spec.fallback_chain.begin(), spec.fallback_chain.end(),
                [](const VerifyJobSpec::Fallback &fallback) {
                    return fallback.code == "hardware_decode_fallback";
                });
            if (!already_fell_back) {
                host_hw_options.backend = media::preferred_host_hwdec();
            }
        }
        const auto index_progress = [&](std::uint64_t decoded) {
            if (job.cancel_requested.load(std::memory_order_relaxed)) return;
            // Total remains unknown during preparation. Once indexing
            // finishes the next progress event publishes the exact scope.
            if (decoded % 256U == 0U) {
                emit_progress(spec.request_id, spec.job_id, "verify", "index",
                              0U, 0U, JsonValue::array());
            }
        };
        if (spec.backend == BackendChoice::metal) {
#if defined(__APPLE__) && defined(GETNATIVE_HAS_MEDIA) && defined(GETNATIVE_HAS_METAL)
            metal_decoder_options.backend = media::DecoderOptions::Backend::videotoolbox;
            try {
                index = media::ensure_index(input.path, input.stream_index, input.cache_directory,
                                            metal_decoder_options, job.stop_source.get_token(), index_progress).index;
                use_metal_decode = true;
            } catch (const std::exception &error) {
                if (job.cancel_requested.load(std::memory_order_relaxed)) {
                    throw;
                }
                if (spec.requested_backend != BackendChoice::automatic) {
                    throw WorkerError("metal_zero_copy_unsupported", error.what());
                }
                const VerifyJobSpec::Fallback fallback{
                    "hardware_decode_fallback", "videotoolbox", "software",
                    error.what(), 0U};
                spec.fallback_chain.push_back(fallback);
                emit_verify_fallback(spec, fallback);
            }
#else
            if (spec.requested_backend != BackendChoice::automatic) {
                throw WorkerError(
                    "metal_zero_copy_unsupported",
                    "VideoToolbox Metal Verify is unavailable");
            }
            const VerifyJobSpec::Fallback fallback{
                "hardware_decode_fallback", "videotoolbox", "software",
                "VideoToolbox Metal Verify is unavailable", 0U};
            spec.fallback_chain.push_back(fallback);
            emit_verify_fallback(spec, fallback);
#endif
        }
        try {
            if (cuda_decoder_options.backend
                == media::DecoderOptions::Backend::cuda) {
                try {
                    index = media::ensure_index(
                        input.path, input.stream_index, input.cache_directory,
                        cuda_decoder_options, job.stop_source.get_token(),
                        index_progress).index;
                    use_cuda_decode = true;
                } catch (const std::exception &error) {
                    if (job.cancel_requested.load(std::memory_order_relaxed)) {
                        throw;
                    }
                    const VerifyJobSpec::Fallback fallback{
                        "hardware_decode_fallback", "nvdec", "software",
                        error.what(), 0U};
                    spec.fallback_chain.push_back(fallback);
                    emit_verify_fallback(spec, fallback);
                    index = media::ensure_index(
                        input.path, input.stream_index, input.cache_directory,
                        media::DecoderOptions{}, job.stop_source.get_token(),
                        index_progress).index;
                }
            } else if (host_hw_options.backend
                       != media::DecoderOptions::Backend::software) {
                index = media::ensure_index(
                    input.path, input.stream_index, input.cache_directory,
                    media::DecoderOptions{}, job.stop_source.get_token(),
                    index_progress).index;
                use_host_hw_decode = true;
            } else if (!use_metal_decode) {
                index = media::ensure_index(
                    input.path, input.stream_index, input.cache_directory,
                    media::DecoderOptions{}, job.stop_source.get_token(),
                    index_progress).index;
            }
        } catch (const std::exception &error) {
            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                throw WorkerError("cancelled", "media indexing cancelled");
            }
            throw WorkerError("media_index_error", error.what());
        }
        const double index_ms = elapsed_ms(index_start);
        check_cancelled(job);
        if (!input.fingerprint.empty() && input.fingerprint != index.fingerprint) {
            throw WorkerError(
                "media_fingerprint_mismatch",
                "the source file changed after it was imported");
        }
        if (index.width != spec.width || index.height != spec.height) {
            throw WorkerError(
                "media_geometry_mismatch",
                "decoded media dimensions do not match the locked verify geometry");
        }

        std::vector<media::FrameIdentity> selected;
        try {
            selected = media::select_frames(index, input.scope);
        } catch (const std::exception &error) {
            throw WorkerError("bad_request", error.what());
        }
        const std::uint64_t eligible_frames = eligible_frame_count(index, input.scope);
        const std::uint64_t selected_frames = static_cast<std::uint64_t>(selected.size());
        if (selected.size() > static_cast<std::size_t>(kVerifyMaxFrames)) {
            throw WorkerError("bad_request", "scan scope exceeds the verify frame limit");
        }
        emit_progress(spec.request_id, spec.job_id, "verify", "index",
                      0U, selected.size(), JsonValue::array(),
                      verify_coverage_json(
                          input.scope, eligible_frames, selected_frames, 0U, 0U));

        const std::int32_t primary_size =
            spec.axis_mode == AxisMode::width_only ? spec.width : spec.height;
        const std::int32_t secondary_size =
            spec.axis_mode == AxisMode::width_only ? spec.height : spec.width;
        const double value = parse_json(spec.candidate).number_value;
        std::vector<AxisPlanRequest> requests;
        if (spec.geometry) {
            requests.push_back(make_geometry_axis_request(
                *spec.geometry, spec.axis_mode == AxisMode::width_only, primary_size, spec.filter));
        } else {
            requests.push_back(make_axis_request(primary_size, value, spec.filter));
        }
        if (spec.axis_mode == AxisMode::height_plus_width) {
            if (spec.geometry) {
                requests.push_back(make_geometry_axis_request(*spec.geometry, true, secondary_size, spec.filter));
            } else {
                const double derived = static_cast<double>(secondary_size) * value
                    / static_cast<double>(primary_size);
                if (derived < 2.0) {
                    throw WorkerError("bad_request", "derived secondary axis length is too small");
                }
                requests.push_back(make_axis_request(secondary_size, derived, spec.filter));
            }
        }

        const auto plan_start = std::chrono::steady_clock::now();
        double store_fetch_ms = 0.0;
        std::size_t store_hits = 0U;
        const std::uint64_t grid_hash =
            plan_store_or_null() != nullptr ? PlanStore::grid_hash(requests) : 0U;
        if (grid_hash != 0U) {
            store_hits += fetch_from_store(
                grid_hash, {requests.data(), requests.size()}, &store_fetch_ms);
        }
        AxisPlanCacheBatchResult batch = plan_cache_.get_or_build_batch(
            std::span<const AxisPlanRequest>{requests.data(), requests.size()});
        const double plan_ms = elapsed_ms(plan_start);
        if (batch.physical_build_count > 0U) {
            queue_store_publish(requests, batch.plans, batch.physical_build_count);
        }
        emit_progress(spec.request_id, spec.job_id, "verify", "plan",
                      0U, selected.size(), JsonValue::array(),
                      verify_coverage_json(
                          input.scope, eligible_frames, selected_frames, 0U, 0U));
        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            emit_verify_cancelled(
                spec, 0U, 0U, "media verification cancelled",
                verify_coverage_json(
                    input.scope, eligible_frames, selected_frames, 0U, 0U));
            return;
        }

        std::shared_ptr<const AxisPlan> horizontal;
        std::shared_ptr<const AxisPlan> vertical;
        AnalysisAxes axes = AnalysisAxes::vertical;
        if (spec.axis_mode == AxisMode::width_only) {
            horizontal = batch.plans.front();
            axes = AnalysisAxes::horizontal;
        } else if (spec.axis_mode == AxisMode::height_only) {
            vertical = batch.plans.front();
            axes = AnalysisAxes::vertical;
        } else {
            vertical = batch.plans.front();
            horizontal = batch.plans.back();
            axes = AnalysisAxes::both;
        }
        const CandidateAnalysis candidate{
            "verify", horizontal, vertical, axes};
        const std::array<CandidateAnalysis, 1U> candidates{candidate};
        std::atomic<double> compute_ms{0.0};

#if defined(GETNATIVE_HAS_CUDA) || defined(GETNATIVE_HAS_VULKAN) || defined(GETNATIVE_HAS_METAL)
        const ConstImageView preflight_dimensions{
            reinterpret_cast<const float *>(std::uintptr_t{1}),
            spec.width, spec.height, spec.width};
#endif
        try {
#if defined(GETNATIVE_HAS_METAL)
            if (spec.backend == BackendChoice::metal) {
                resident_metal_engine().preflight_axis_batch(
                    preflight_dimensions, candidates, spec.metric, spec.concurrency);
            }
#endif
#if defined(GETNATIVE_HAS_CUDA)
            if (spec.backend == BackendChoice::cuda) {
                resident_cuda_engine().preflight_axis_batch(
                    preflight_dimensions, candidates, spec.metric,
                    spec.concurrency);
            }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
            if (spec.backend == BackendChoice::vulkan) {
                resident_vulkan_engine().preflight_axis_batch(
                    preflight_dimensions, candidates, spec.metric,
                    spec.concurrency);
            }
#endif
        } catch (const std::exception &error) {
            throw WorkerError(
                "media_concurrency_unavailable",
                std::string{"media verification concurrency preflight failed: "}
                    + error.what());
        }

#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda) {
            resident_cuda_engine().reset_analysis_telemetry();
        }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        if (spec.backend == BackendChoice::vulkan) {
            resident_vulkan_engine().reset_analysis_telemetry();
        }
#endif
#if defined(GETNATIVE_HAS_METAL)
        if (spec.backend == BackendChoice::metal) {
            resident_metal_engine().reset_analysis_telemetry();
        }
#endif

        constexpr std::size_t result_batch_size = 32U;
        std::vector<JsonValue> result_batch;
        result_batch.reserve(result_batch_size);
        std::vector<JsonValue> final_frames;
        final_frames.reserve(selected.size());
        std::uint64_t completed = 0U;
        std::uint64_t progress_completed = 0U;
        std::vector<char> finished_seqs(selected.size(), 0);
        const auto json_seq = [](const JsonValue &value) -> std::int64_t {
            const JsonValue *seq = value.find("seq");
            if (seq == nullptr) return 0;
            return static_cast<std::int64_t>(std::llround(seq->number_value));
        };
        const auto flush_results = [&] {
            if (result_batch.empty()) return;
            emit_progress(spec.request_id, spec.job_id, "verify", "verify",
                          progress_completed, selected.size(),
                          JsonValue::array(std::move(result_batch)),
                          verify_coverage_json(
                              input.scope, eligible_frames, selected_frames,
                              completed, 0U));
            result_batch.clear();
            result_batch.reserve(result_batch_size);
        };

        media::DecodeTelemetry decode_telemetry;
        std::string actual_decoder = "software";
        bool zero_copy = false;
        std::size_t max_inflight = 0U;
        std::size_t surface_lease_peak = 0U;
        double queue_wait_ms = 0.0;
        const auto append_result = [&](std::uint64_t seq,
                                       const media::FrameIdentity &identity,
                                       double error,
                                       std::uint64_t analyzed) {
            JsonValue result = JsonValue::object({
                {"seq", JsonValue::integer(static_cast<std::int64_t>(seq))},
                {"frame_index", JsonValue::integer(
                    static_cast<std::int64_t>(identity.frame_index))},
                {"pts", identity.pts
                    ? JsonValue::integer(*identity.pts) : JsonValue{}},
                {"timestamp_seconds", identity.timestamp_seconds
                    ? JsonValue::number(*identity.timestamp_seconds) : JsonValue{}},
                {"error", JsonValue::number(error)},
            });
            final_frames.push_back(result);
            result_batch.push_back(std::move(result));
            if (seq < finished_seqs.size()) {
                finished_seqs[static_cast<std::size_t>(seq)] = 1;
            }
            ++completed;
            progress_completed = std::max(progress_completed, analyzed);
            if (result_batch.size() >= result_batch_size) flush_results();
        };
        const auto analyze_host = [&](const media::HostFrame &frame) {
            if (frame.width != spec.width || frame.height != spec.height) {
                throw WorkerError(
                    "media_resolution_changed",
                    "decoded media resolution changed during verification");
            }
            ConstImageView view{
                frame.pixels.data(), frame.width, frame.height, frame.width};
            const auto compute_start = std::chrono::steady_clock::now();
            double error = 0.0;
            if (spec.backend == BackendChoice::cpu) {
                thread_local CpuWorkspace cpu_workspace;
                if (axes == AnalysisAxes::both) {
                    error = analyze_candidate_f32(
                        view, *horizontal, *vertical, spec.metric, cpu_workspace);
                } else {
                    error = analyze_axis_candidate_f32(
                        view,
                        axes == AnalysisAxes::vertical ? *vertical : *horizontal,
                        axes, spec.metric, cpu_workspace);
                }
            } else if (spec.backend == BackendChoice::cuda) {
#if defined(GETNATIVE_HAS_CUDA)
                error = resident_cuda_engine().analyze_axis_batch_f32(
                    view, candidates, spec.metric,
                    job.stop_source.get_token(),
                    gpu_stage_profile_from_environment()).front().error;
#else
                throw WorkerError("unsupported", "CUDA backend was not compiled");
#endif
            } else if (spec.backend == BackendChoice::vulkan) {
#if defined(GETNATIVE_HAS_VULKAN)
                error = resident_vulkan_engine().analyze_axis_batch_f32(
                    view, candidates, spec.metric,
                    job.stop_source.get_token(),
                    gpu_stage_profile_from_environment()).front().error;
#else
                throw WorkerError("unsupported", "Vulkan backend was not compiled");
#endif
            } else if (spec.backend == BackendChoice::metal) {
#if defined(GETNATIVE_HAS_METAL)
                error = resident_metal_engine().analyze_axis_batch_f32(
                    view, candidates, spec.metric,
                    job.stop_source.get_token(), {},
                    gpu_stage_profile_from_environment()).front().error;
#else
                throw WorkerError("unsupported", "Metal backend was not compiled");
#endif
            }
            compute_ms.fetch_add(
                elapsed_ms(compute_start), std::memory_order_relaxed);
            return error;
        };
        try {
            bool decoded = false;
#if defined(GETNATIVE_HAS_CUDA)
            if (spec.backend == BackendChoice::cuda && use_cuda_decode) {
                try {
                    CudaAnalysisEngine &cuda = resident_cuda_engine();
                    cuda_decoder_options.expected_bit_depth = index.bit_depth;
                    MediaVerifyPipeline<media::CudaFrame> pipeline{
                        spec.concurrency, job.stop_source.get_token(),
                        [&](const media::CudaFrame &frame) {
                            if (frame.width != spec.width
                                || frame.height != spec.height) {
                                throw WorkerError(
                                    "media_resolution_changed",
                                    "decoded media resolution changed during verification");
                            }
                            CudaLumaFormat format = CudaLumaFormat::nv12;
                            switch (frame.format) {
                            case media::CudaLumaFormat::nv12:
                                format = CudaLumaFormat::nv12;
                                break;
                            case media::CudaLumaFormat::p010:
                                format = CudaLumaFormat::p010;
                                break;
                            case media::CudaLumaFormat::p016:
                                format = CudaLumaFormat::p016;
                                break;
                            case media::CudaLumaFormat::yuv444p8:
                                format = CudaLumaFormat::yuv444p8;
                                break;
                            case media::CudaLumaFormat::yuv444p16:
                                format = CudaLumaFormat::yuv444p16;
                                break;
                            }
                            const CudaLumaFrameView view{
                                frame.device_pointer,
                                frame.pitch_bytes,
                                frame.width,
                                frame.height,
                                format,
                                frame.bit_depth,
                                frame.range == "full"
                                    ? CudaColorRange::full
                                    : CudaColorRange::limited,
                                frame.context,
                                frame.producer_stream,
                            };
                            const auto compute_start =
                                std::chrono::steady_clock::now();
                            try {
                                const double error =
                                    cuda.analyze_axis_batch_cuda_luma(
                                        view, candidates, spec.metric,
                                        job.stop_source.get_token(),
                                        gpu_stage_profile_from_environment()).front().error;
                                compute_ms.fetch_add(
                                    elapsed_ms(compute_start),
                                    std::memory_order_relaxed);
                                return error;
                            } catch (const WorkerError &) {
                                throw;
                            } catch (const std::exception &error) {
                                if (job.stop_source.stop_requested()) {
                                    throw WorkerError(
                                        "cancelled", "CUDA analysis cancelled");
                                }
                                throw WorkerError(
                                    "compute_error",
                                    std::string{"CUDA analysis failed: "}
                                        + error.what());
                            }
                        },
                        [&](std::uint64_t seq,
                            const media::FrameIdentity &identity, double error,
                            std::uint64_t analyzed) {
                            append_result(seq, identity, error, analyzed);
                        },
                        [&] { job.stop_source.request_stop(); }};
                    pipeline.run([&](auto consume) {
                        media::decode_selected_cuda(
                            input.path, index, selected, cuda_decoder_options,
                            job.stop_source.get_token(), std::move(consume),
                            &decode_telemetry);
                    });
                    max_inflight = std::max(max_inflight, pipeline.max_inflight());
                    surface_lease_peak = std::max(
                        surface_lease_peak, pipeline.max_inflight());
                    queue_wait_ms += pipeline.queue_wait_ms();
                    actual_decoder = "nvdec";
                    zero_copy = true;
                    decoded = true;
                } catch (const WorkerError &) {
                    throw;
                } catch (const std::exception &error) {
                    if (job.cancel_requested.load(std::memory_order_relaxed)) {
                        throw WorkerError("cancelled", "media decode cancelled");
                    }
                    const VerifyJobSpec::Fallback fallback{
                        "hardware_decode_fallback", "nvdec", "software",
                        error.what(), completed};
                    spec.fallback_chain.push_back(fallback);
                    emit_verify_fallback(spec, fallback);
                }
            }
#endif
            if (spec.backend == BackendChoice::vulkan && use_host_hw_decode
                && host_hw_options.backend != media::DecoderOptions::Backend::software) {
                try {
                    MediaVerifyPipeline<media::HostFrame> pipeline{
                        spec.concurrency, job.stop_source.get_token(), analyze_host,
                        [&](std::uint64_t seq,
                            const media::FrameIdentity &identity, double error,
                            std::uint64_t analyzed) {
                            append_result(seq, identity, error, analyzed);
                        },
                        [&] { job.stop_source.request_stop(); }};
                    pipeline.run([&](auto consume) {
                        media::decode_selected_indexed(
                            input.path, index, selected, host_hw_options,
                            job.stop_source.get_token(), std::move(consume),
                            &decode_telemetry);
                    });
                    max_inflight = std::max(max_inflight, pipeline.max_inflight());
                    queue_wait_ms += pipeline.queue_wait_ms();
                    actual_decoder = media::decoder_backend_id(host_hw_options.backend);
                    zero_copy = false;
                    decoded = true;
                } catch (const WorkerError &) {
                    throw;
                } catch (const std::exception &error) {
                    if (job.cancel_requested.load(std::memory_order_relaxed)) {
                        throw WorkerError("cancelled", "media decode cancelled");
                    }
                    const VerifyJobSpec::Fallback fallback{
                        "hardware_decode_fallback",
                        media::decoder_backend_id(host_hw_options.backend),
                        "software", error.what(), completed};
                    spec.fallback_chain.push_back(fallback);
                    emit_verify_fallback(spec, fallback);
                }
            }
#if defined(__APPLE__) && defined(GETNATIVE_HAS_METAL)
            if (spec.backend == BackendChoice::metal && use_metal_decode) {
                try {
                    MetalAnalysisEngine &metal = resident_metal_engine();
                    MediaVerifyPipeline<media::MetalFrame> pipeline{
                        spec.concurrency, job.stop_source.get_token(),
                        [&](const media::MetalFrame &frame) {
                            if (frame.width != spec.width || frame.height != spec.height) {
                                throw WorkerError(
                                    "media_resolution_changed",
                                    "decoded media resolution changed during verification");
                            }
                            MetalLumaFrameView view{
                                frame.pixel_buffer, frame.width, frame.height,
                                frame.bit_depth, frame.surface_format, frame.range};
                            const auto start = std::chrono::steady_clock::now();
                            try {
                                const double error = metal.analyze_axis_batch_metal_luma(
                                    view, candidates, spec.metric,
                                    job.stop_source.get_token(),
                                    gpu_stage_profile_from_environment()).front().error;
                                compute_ms.fetch_add(
                                    elapsed_ms(start), std::memory_order_relaxed);
                                return error;
                            } catch (const WorkerError &) {
                                throw;
                            } catch (const std::exception &error) {
                                if (job.stop_source.stop_requested()) {
                                    throw WorkerError(
                                        "cancelled", "Metal analysis cancelled");
                                }
                                throw WorkerError(
                                    "compute_error",
                                    std::string{"Metal analysis failed: "}
                                        + error.what());
                            }
                        },
                        [&](std::uint64_t seq,
                            const media::FrameIdentity &identity, double error,
                            std::uint64_t analyzed) {
                            append_result(seq, identity, error, analyzed);
                        },
                        [&] { job.stop_source.request_stop(); }};
                    pipeline.run([&](auto consume) {
                        media::decode_selected_metal(
                            input.path, index, selected, metal_decoder_options,
                            job.stop_source.get_token(), std::move(consume),
                            &decode_telemetry);
                    });
                    max_inflight = std::max(max_inflight, pipeline.max_inflight());
                    surface_lease_peak = std::max(
                        surface_lease_peak, pipeline.max_inflight());
                    queue_wait_ms += pipeline.queue_wait_ms();
                    actual_decoder = "videotoolbox";
                    zero_copy = true;
                    if (decode_telemetry.host_frame_bytes != 0U
                        || decode_telemetry.conversion_bytes != 0U) {
                        throw WorkerError(
                            "metal_zero_copy_unsupported",
                            "Metal Verify produced host frame or conversion bytes");
                    }
                    decoded = true;
                } catch (const WorkerError &) {
                    throw;
                } catch (const std::exception &error) {
                    if (job.cancel_requested.load(std::memory_order_relaxed)) {
                        throw WorkerError("cancelled", "media decode cancelled");
                    }
                    if (spec.requested_backend != BackendChoice::automatic) {
                        throw WorkerError("metal_zero_copy_unsupported", error.what());
                    }
                    const VerifyJobSpec::Fallback fallback{
                        "hardware_decode_fallback", "videotoolbox", "software",
                        error.what(), completed};
                    spec.fallback_chain.push_back(fallback);
                    emit_verify_fallback(spec, fallback);
                }
            }
#endif
            if (!decoded) {
                media::DecodeTelemetry software_telemetry;
                const RemainingVerifyFrames remaining =
                    remaining_verify_frames(selected, finished_seqs);
                if (remaining.identities.empty()) {
                    decoded = true;
                } else {
                MediaVerifyPipeline<media::HostFrame> pipeline{
                    spec.concurrency, job.stop_source.get_token(), analyze_host,
                    [&](std::uint64_t seq,
                        const media::FrameIdentity &identity, double error,
                        std::uint64_t) {
                        if (seq >= remaining.original_seqs.size()) {
                            throw WorkerError(
                                "media_decode_error",
                                "software fallback seq is outside remaining frames");
                        }
                        append_result(
                            remaining.original_seqs[static_cast<std::size_t>(seq)],
                            identity, error, completed + 1U);
                    },
                    [&] { job.stop_source.request_stop(); }};
                pipeline.run([&](auto consume) {
                    media::decode_selected_indexed(
                        input.path, index, remaining.identities,
                        media::DecoderOptions{}, job.stop_source.get_token(),
                        std::move(consume), &software_telemetry);
                });
                max_inflight = std::max(max_inflight, pipeline.max_inflight());
                queue_wait_ms += pipeline.queue_wait_ms();
                decode_telemetry.decoded_frames += software_telemetry.decoded_frames;
                decode_telemetry.decode_retries += software_telemetry.decode_retries;
                decode_telemetry.selected_frames += software_telemetry.selected_frames;
                decode_telemetry.discarded_packets += software_telemetry.discarded_packets;
                decode_telemetry.host_frame_bytes += software_telemetry.host_frame_bytes;
                decode_telemetry.conversion_bytes += software_telemetry.conversion_bytes;
                decode_telemetry.decode_ms += software_telemetry.decode_ms;
                decode_telemetry.convert_ms += software_telemetry.convert_ms;
                }
            }
        } catch (const WorkerError &error) {
            if (error.code() == "cancelled") {
                flush_results();
                emit_verify_cancelled(
                    spec, completed, 0U, "media verification cancelled",
                    verify_coverage_json(
                        input.scope, eligible_frames, selected_frames,
                        completed, 0U));
                return;
            }
            throw;
        } catch (const std::exception &error) {
            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                throw WorkerError("cancelled", "media decode cancelled");
            }
            throw WorkerError("media_decode_error", error.what());
        }
        flush_results();
        std::sort(
            final_frames.begin(), final_frames.end(),
            [&](const JsonValue &left, const JsonValue &right) {
                return json_seq(left) < json_seq(right);
            });
        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            emit_verify_cancelled(
                spec, completed, 0U, "media verification cancelled",
                verify_coverage_json(
                    input.scope, eligible_frames, selected_frames,
                    completed, 0U));
            return;
        }

        std::size_t source_upload_bytes = 0U;
        std::size_t device_conversion_bytes = 0U;
        std::size_t plan_upload_bytes = 0U;
        std::size_t result_readback_bytes = 0U;
        double device_convert_ms = 0.0;
        double upload_ms = 0.0;
        double readback_ms = 0.0;
        double execution_slot_wait_ms = 0.0;
        std::size_t source_direct_write_bytes = 0U;
        std::size_t source_legacy_copy_bytes = 0U;
        std::size_t plan_direct_write_bytes = 0U;
        std::size_t plan_legacy_copy_bytes = 0U;
        double source_pack_ms = 0.0;
        double plan_pack_ms = 0.0;
        bool external_source_zero_copy = false;
        bool shared_uma_path = false;
        std::size_t buffer_reuse_count = 0U;
#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda) {
            const CudaRuntimeTelemetry gpu = resident_cuda_engine().runtime_telemetry();
            source_upload_bytes = gpu.source_upload_bytes;
            device_conversion_bytes = gpu.source_conversion_bytes;
            plan_upload_bytes = gpu.plan_upload_bytes;
            result_readback_bytes = gpu.result_readback_bytes;
            device_convert_ms = gpu.source_conversion_ms;
            upload_ms = gpu.source_upload_ms + gpu.plan_upload_ms;
            readback_ms = gpu.result_readback_ms;
            execution_slot_wait_ms = gpu.execution_slot_wait_ms;
        }
#endif
#if defined(GETNATIVE_HAS_VULKAN)
        if (spec.backend == BackendChoice::vulkan) {
            const VulkanRuntimeTelemetry gpu = resident_vulkan_engine().runtime_telemetry();
            source_upload_bytes = gpu.source_upload_bytes;
            device_conversion_bytes = gpu.source_conversion_bytes;
            plan_upload_bytes = gpu.plan_upload_bytes;
            result_readback_bytes = gpu.result_readback_bytes;
            device_convert_ms = gpu.source_conversion_ms;
            upload_ms = gpu.host_pack_ms;
            readback_ms = 0.0;
            execution_slot_wait_ms = gpu.execution_slot_wait_ms;
        }
#endif
#if defined(GETNATIVE_HAS_METAL)
        if (spec.backend == BackendChoice::metal) {
            const MetalRuntimeTelemetry gpu = resident_metal_engine().runtime_telemetry();
            plan_upload_bytes = gpu.plan_upload_bytes;
            upload_ms = gpu.plan_upload_ms;
            execution_slot_wait_ms = gpu.execution_slot_wait_ms;
            source_direct_write_bytes = gpu.source_direct_write_bytes;
            source_legacy_copy_bytes = gpu.source_legacy_copy_bytes;
            plan_direct_write_bytes = gpu.plan_direct_write_bytes;
            plan_legacy_copy_bytes = gpu.plan_legacy_copy_bytes;
            source_pack_ms = gpu.source_pack_ms;
            plan_pack_ms = gpu.plan_pack_ms;
            external_source_zero_copy = gpu.external_source_zero_copy;
            shared_uma_path = gpu.shared_uma_path;
            buffer_reuse_count = gpu.working_buffer_reuse_count;
        }
#endif

        const double total_ms = elapsed_ms(job_start);
        const double fps = total_ms > 0.0
            ? static_cast<double>(completed) * 1000.0 / total_ms : 0.0;
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("result")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("verify")},
            {"coverage", verify_coverage_json(
                input.scope, eligible_frames, selected_frames, completed, 0U)},
            {"payload", JsonValue::object({
                {"mode", JsonValue::string("verify")},
                {"frames_completed", JsonValue::integer(
                    static_cast<std::int64_t>(completed))},
                {"frames_failed", JsonValue::integer(0)},
                {"frames", JsonValue::array(std::move(final_frames))},
                {"provenance", JsonValue::object({
                    {"requested_compute_backend", JsonValue::string(
                        backend_choice_name(spec.requested_backend))},
                    {"actual_compute_backend", JsonValue::string(
                        backend_choice_name(spec.backend))},
                    {"decoder", JsonValue::string(actual_decoder)},
                    {"codec", JsonValue::string(index.codec)},
                    {"profile", JsonValue::string(index.profile)},
                    {"device", spec.selected_device.empty()
                        ? JsonValue{} : JsonValue::string(spec.selected_device)},
                    {"device_uuid", spec.selected_device_uuid.empty()
                        ? JsonValue{} : JsonValue::string(spec.selected_device_uuid)},
                    {"input_pixel_format", JsonValue::string(index.pixel_format)},
                    {"bit_depth", JsonValue::integer(index.bit_depth)},
                    {"range", JsonValue::string(index.range)},
                    {"zero_copy", JsonValue::boolean(zero_copy)},
                    {"fallback_chain", verify_fallbacks_json(spec)},
                })},
                {"telemetry", JsonValue::object({
                    {"plan_build_count", JsonValue::integer(
                        static_cast<std::int64_t>(batch.physical_build_count))},
                    {"plan_cache_hits", JsonValue::integer(
                        static_cast<std::int64_t>(batch.ready_hit_count))},
                    {"plan_store_hits", JsonValue::integer(
                        static_cast<std::int64_t>(store_hits))},
                    {"plan_store_fetch_ms", JsonValue::number(store_fetch_ms)},
                    {"worker_queue_ms", JsonValue::number(worker_queue_ms)},
                    {"index_ms", JsonValue::number(index_ms)},
                    {"plan_ms", JsonValue::number(plan_ms)},
                    {"decode_ms", JsonValue::number(decode_telemetry.decode_ms)},
                    {"decoded_frames", JsonValue::integer(
                        static_cast<std::int64_t>(decode_telemetry.decoded_frames))},
                    {"decode_retries", JsonValue::integer(
                        static_cast<std::int64_t>(decode_telemetry.decode_retries))},
                    {"decode_sessions", JsonValue::integer(
                        static_cast<std::int64_t>(decode_telemetry.decode_sessions))},
                    {"discarded_packets", JsonValue::integer(
                        static_cast<std::int64_t>(decode_telemetry.discarded_packets))},
                    {"convert_ms", JsonValue::number(
                        decode_telemetry.convert_ms + device_convert_ms)},
                    {"upload_ms", JsonValue::number(upload_ms)},
                    {"compute_ms", JsonValue::number(
                        std::max(
                            0.0,
                            compute_ms.load(std::memory_order_relaxed)
                                - device_convert_ms))},
                    {"readback_ms", JsonValue::number(readback_ms)},
                    {"requested_concurrency", JsonValue::integer(
                        static_cast<std::int64_t>(spec.requested_concurrency))},
                    {"effective_concurrency", JsonValue::integer(
                        static_cast<std::int64_t>(spec.concurrency))},
                    {"max_inflight", JsonValue::integer(
                        static_cast<std::int64_t>(max_inflight))},
                    {"execution_slot_wait_ms", JsonValue::number(
                        execution_slot_wait_ms)},
                    {"queue_wait_ms", JsonValue::number(queue_wait_ms)},
                    {"surface_lease_peak", JsonValue::integer(
                        static_cast<std::int64_t>(surface_lease_peak))},
                    {"host_frame_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(decode_telemetry.host_frame_bytes))},
                    {"conversion_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(
                            decode_telemetry.conversion_bytes
                            + device_conversion_bytes))},
                    {"source_upload_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(source_upload_bytes))},
                    {"plan_upload_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(plan_upload_bytes))},
                    {"source_direct_write_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(source_direct_write_bytes))},
                    {"source_legacy_copy_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(source_legacy_copy_bytes))},
                    {"plan_direct_write_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(plan_direct_write_bytes))},
                    {"plan_legacy_copy_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(plan_legacy_copy_bytes))},
                    {"source_pack_ms", JsonValue::number(source_pack_ms)},
                    {"plan_pack_ms", JsonValue::number(plan_pack_ms)},
                    {"external_source_zero_copy", JsonValue::boolean(
                        external_source_zero_copy)},
                    {"shared_uma_path", JsonValue::boolean(shared_uma_path)},
                    {"buffer_reuse_count", JsonValue::integer(
                        static_cast<std::int64_t>(buffer_reuse_count))},
                    {"result_readback_bytes", JsonValue::integer(
                        static_cast<std::int64_t>(result_readback_bytes))},
                    {"job_total_ms", JsonValue::number(total_ms)},
                    {"fps", JsonValue::number(fps)},
                    {"backend", JsonValue::string(backend_choice_name(spec.backend))},
                })},
            })},
        }));
    }
#endif

    // One locked recipe, many streamed frames. Analysis workers consume the
    // inbox as the reader thread appends verify_frame items, so decode on the
    // producer side overlaps analysis naturally; the app bounds its asset
    // production with the accepted event's suggested_in_flight hint.
    void run_verify_job(Job &job) {
        const VerifyJobSpec &spec = job.verify;
        const double worker_queue_ms = elapsed_ms(job.queued_at);
        const auto job_start = std::chrono::steady_clock::now();

        const std::int32_t primary_size =
            spec.axis_mode == AxisMode::width_only ? spec.width : spec.height;
        const std::int32_t secondary_size =
            spec.axis_mode == AxisMode::width_only ? spec.height : spec.width;
        const double value = parse_json(spec.candidate).number_value;

        std::vector<AxisPlanRequest> requests;
        if (spec.geometry) {
            requests.push_back(make_geometry_axis_request(
                *spec.geometry, spec.axis_mode == AxisMode::width_only, primary_size, spec.filter));
        } else {
            requests.push_back(make_axis_request(primary_size, value, spec.filter));
        }
        if (spec.axis_mode == AxisMode::height_plus_width) {
            if (spec.geometry) {
                requests.push_back(make_geometry_axis_request(*spec.geometry, true, secondary_size, spec.filter));
            } else {
                const double derived = static_cast<double>(secondary_size) * value
                    / static_cast<double>(primary_size);
                if (derived < 2.0) {
                    throw WorkerError("bad_request", "derived secondary axis length is too small");
                }
                requests.push_back(make_axis_request(secondary_size, derived, spec.filter));
            }
        }

        const auto plan_start = std::chrono::steady_clock::now();
        double store_fetch_ms = 0.0;
        std::size_t store_hits = 0;
        const std::uint64_t grid_hash =
            plan_store_or_null() != nullptr ? PlanStore::grid_hash(requests) : 0U;
        if (grid_hash != 0U) {
            store_hits += fetch_from_store(
                grid_hash, {requests.data(), requests.size()}, &store_fetch_ms);
        }
        AxisPlanCacheBatchResult batch = plan_cache_.get_or_build_batch(
            std::span<const AxisPlanRequest>{requests.data(), requests.size()});
        const double plan_ms = elapsed_ms(plan_start);
        if (batch.physical_build_count > 0) {
            queue_store_publish(requests, batch.plans, batch.physical_build_count);
        }
        emit_progress(spec.request_id, spec.job_id, "verify", "plan",
                      requests.size(), requests.size(), JsonValue::array());
        check_cancelled(job);

        std::shared_ptr<const AxisPlan> horizontal;
        std::shared_ptr<const AxisPlan> vertical;
        AnalysisAxes axes = AnalysisAxes::vertical;
        if (spec.axis_mode == AxisMode::width_only) {
            horizontal = batch.plans.front();
            axes = AnalysisAxes::horizontal;
        } else if (spec.axis_mode == AxisMode::height_only) {
            vertical = batch.plans.front();
            axes = AnalysisAxes::vertical;
        } else {
            vertical = batch.plans.front();
            horizontal = batch.plans.back();
            axes = AnalysisAxes::both;
        }

        const std::size_t worker_count = effective_verify_workers(spec);
        const std::size_t result_batch_size = std::max<std::size_t>(
            1U, std::min(kVerifyResultBatchSize, worker_count * 2U));
#if defined(_WIN32)
        const std::uint64_t elements =
            static_cast<std::uint64_t>(spec.width) * static_cast<std::uint64_t>(spec.height);
#endif
        std::atomic<double> frame_load_ms{0.0};
        std::atomic<double> frame_analyze_ms{0.0};

        const auto worker_body = [&] {
#if defined(_WIN32)
            std::vector<float> buffer(static_cast<std::size_t>(elements));
#endif
            CpuWorkspace workspace;
            while (true) {
                VerifyFrameItem item;
                {
                    std::unique_lock lock(job.verify_mutex);
                    job.verify_condition.wait(lock, [&] {
                        return !job.verify_inbox.empty() || job.verify_stream_ended
                            || job.cancel_requested.load(std::memory_order_relaxed);
                    });
                    if (job.cancel_requested.load(std::memory_order_relaxed)) return;
                    if (job.verify_inbox.empty()) {
                        if (job.verify_stream_ended) return;
                        continue;
                    }
                    item = std::move(job.verify_inbox.front());
                    job.verify_inbox.pop_front();
                }

                std::optional<double> error;
                try {
                    const auto load_start = std::chrono::steady_clock::now();
                    const auto analyze_view = [&](ConstImageView view) {
                        const auto analyze_start = std::chrono::steady_clock::now();
                        double frame_error = 0.0;
                        if (axes == AnalysisAxes::both) {
                            frame_error = analyze_candidate_f32(
                                view, *horizontal, *vertical, spec.metric, workspace);
                        } else {
                            frame_error = analyze_axis_candidate_f32(
                                view, axes == AnalysisAxes::vertical ? *vertical : *horizontal,
                                axes, spec.metric, workspace);
                        }
                        frame_analyze_ms.fetch_add(elapsed_ms(analyze_start),
                                                   std::memory_order_relaxed);
                        return frame_error;
                    };
                    if (item.ring) {
                        ConstImageView view{
                            item.ring->slot_data(item.slot), spec.width, spec.height, spec.width};
                        frame_load_ms.fetch_add(elapsed_ms(load_start),
                                                std::memory_order_relaxed);
                        error = analyze_view(view);
                    } else {
#if !defined(_WIN32)
                        MappedFrame mapped(item.asset);
                        ConstImageView view{mapped.data(), spec.width, spec.height, spec.width};
#else
                        load_frame_into(item.asset, buffer.data());
                        ConstImageView view{buffer.data(), spec.width, spec.height, spec.width};
#endif
                        frame_load_ms.fetch_add(elapsed_ms(load_start),
                                                std::memory_order_relaxed);
                        error = analyze_view(view);
                    }
                } catch (const std::exception &failure) {
                    {
                        const std::scoped_lock lock(job.verify_mutex);
                        ++job.verify_failed;
                    }
                    emit(JsonValue::object({
                        {"protocol_version", JsonValue::integer(kProtocolVersion)},
                        {"type", JsonValue::string("warning")},
                        {"request_id", JsonValue::string(spec.request_id)},
                        {"job_id", JsonValue::string(spec.job_id)},
                        {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                        {"code", JsonValue::string("frame_asset_error")},
                        {"message", JsonValue::string(failure.what())},
                        {"seq", JsonValue::integer(static_cast<std::int64_t>(item.seq))},
                    }));
                }
                if (item.ring) {
                    emit(JsonValue::object({
                        {"protocol_version", JsonValue::integer(kProtocolVersion)},
                        {"type", JsonValue::string("verify_consumed")},
                        {"request_id", JsonValue::string(spec.request_id)},
                        {"job_id", JsonValue::string(spec.job_id)},
                        {"timestamp_ms", JsonValue::integer(timestamp_ms())},
                        {"seq", JsonValue::integer(static_cast<std::int64_t>(item.seq))},
                        {"slot", JsonValue::integer(static_cast<std::int64_t>(item.slot))},
                        {"generation", JsonValue::integer(
                            static_cast<std::int64_t>(item.generation))},
                    }));
                }
                {
                    const std::scoped_lock lock(job.verify_mutex);
                    job.verify_outbox.push_back({item.seq, error});
                    if (error) ++job.verify_completed;
                }
                job.verify_condition.notify_all();
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers.emplace_back(worker_body);
        }

        // Drain loop: batch results into progress events until the stream
        // completes or cancellation lands. A declared-total mismatch never
        // completes normally; verify_end cancels the job in that case.
        const auto drain_outbox = [&] {
            std::vector<VerifyFrameResult> pending;
            std::uint64_t processed = 0;
            std::uint64_t total = 0;
            {
                const std::scoped_lock lock(job.verify_mutex);
                pending = std::move(job.verify_outbox);
                job.verify_outbox.clear();
                processed = job.verify_completed + job.verify_failed;
                if (spec.expected_frames > 0) {
                    total = static_cast<std::uint64_t>(spec.expected_frames);
                } else {
                    total = job.verify_stream_ended ? job.verify_declared_total
                                                    : job.verify_received;
                }
            }
            if (pending.empty()) return;
            std::vector<JsonValue> results;
            results.reserve(pending.size());
            for (const VerifyFrameResult &entry : pending) {
                results.push_back(JsonValue::object({
                    {"seq", JsonValue::integer(static_cast<std::int64_t>(entry.seq))},
                    {"error", entry.error ? JsonValue::number(*entry.error)
                                          : JsonValue{}},
                }));
            }
            emit_progress(spec.request_id, spec.job_id, "verify", "verify",
                          processed, total, JsonValue::array(std::move(results)));
        };

        while (true) {
            bool finished = false;
            {
                std::unique_lock lock(job.verify_mutex);
                job.verify_condition.wait(lock, [&] {
                    return job.verify_outbox.size() >= result_batch_size
                        || job.cancel_requested.load(std::memory_order_relaxed)
                        || (job.verify_stream_ended
                            && job.verify_completed + job.verify_failed >= job.verify_received
                            && job.verify_inbox.empty());
                });
                finished = job.cancel_requested.load(std::memory_order_relaxed)
                    || (job.verify_stream_ended
                        && job.verify_completed + job.verify_failed >= job.verify_received
                        && job.verify_inbox.empty()
                        && job.verify_declared_total == job.verify_received);
            }
            drain_outbox();
            if (finished) break;
        }
        for (std::thread &worker : workers) {
            if (worker.joinable()) worker.join();
        }

        std::uint64_t completed = 0;
        std::uint64_t failed = 0;
        std::string cancel_detail;
        {
            const std::scoped_lock lock(job.verify_mutex);
            completed = job.verify_completed;
            failed = job.verify_failed;
            cancel_detail = job.verify_cancel_detail;
        }

        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            drain_outbox();
            emit_verify_cancelled(spec, completed, failed,
                                  cancel_detail.empty() ? "cancelled" : cancel_detail);
            return;
        }

        const double stream_ms = elapsed_ms(job_start);
        double asset_wait_ms = stream_ms;
        {
            const std::scoped_lock lock(job.verify_mutex);
            if (job.verify_first_frame_at) {
                asset_wait_ms = std::chrono::duration<double, std::milli>(
                    *job.verify_first_frame_at - job_start).count();
            }
        }
        const double fps = stream_ms > 0.0
            ? static_cast<double>(completed) * 1000.0 / stream_ms
            : 0.0;
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("result")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("verify")},
            {"payload", JsonValue::object({
                {"mode", JsonValue::string("verify")},
                {"frames_completed", JsonValue::integer(static_cast<std::int64_t>(completed))},
                {"frames_failed", JsonValue::integer(static_cast<std::int64_t>(failed))},
                {"telemetry", JsonValue::object({
                    {"plan_build_count", JsonValue::integer(
                        static_cast<std::int64_t>(batch.physical_build_count))},
                    {"plan_cache_hits", JsonValue::integer(
                        static_cast<std::int64_t>(batch.ready_hit_count))},
                    {"plan_store_hits", JsonValue::integer(
                        static_cast<std::int64_t>(store_hits))},
                    {"plan_store_fetch_ms", JsonValue::number(store_fetch_ms)},
                    {"plan_resident_entries", JsonValue::integer(
                        static_cast<std::int64_t>(plan_cache_.size()))},
                    {"plan_cache_misses", JsonValue::integer(
                        static_cast<std::int64_t>(batch.physical_build_count))},
                    {"asset_cache_hits", JsonValue::integer(0)},
                    {"asset_cache_misses", JsonValue::integer(0)},
                    {"worker_queue_ms", JsonValue::number(worker_queue_ms)},
                    {"asset_wait_ms", JsonValue::number(asset_wait_ms)},
                    {"plan_ms", JsonValue::number(plan_ms)},
                    {"candidate_ms", JsonValue::number(frame_analyze_ms.load())},
                    {"frame_load_ms", JsonValue::number(frame_load_ms.load())},
                    {"frame_analyze_ms", JsonValue::number(frame_analyze_ms.load())},
                    {"job_total_ms", JsonValue::number(stream_ms)},
                    {"stream_ms", JsonValue::number(stream_ms)},
                    {"fps", JsonValue::number(fps)},
                    {"worker_count", JsonValue::integer(
                        static_cast<std::int64_t>(worker_count))},
                    {"backend", JsonValue::string("cpu")},
                    {"isa", JsonValue::string(std::string{
                        cpu_isa_name(cpu_dispatch_info().selected)})},
                })},
            })},
        }));
    }

    void emit_verify_cancelled(const VerifyJobSpec &spec, std::uint64_t completed,
                               std::uint64_t failed, const std::string &detail,
                               JsonValue coverage = JsonValue{}) {
        std::vector<std::pair<std::string, JsonValue>> members = {
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("cancelled")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("verify")},
            {"partial", JsonValue::boolean(completed + failed > 0)},
            {"detail", JsonValue::string(detail)},
            {"payload", JsonValue::object({
                {"mode", JsonValue::string("verify")},
                {"frames_completed", JsonValue::integer(static_cast<std::int64_t>(completed))},
                {"frames_failed", JsonValue::integer(static_cast<std::int64_t>(failed))},
            })},
        };
        if (coverage.type == JsonValue::Type::object) {
            members.emplace_back("coverage", std::move(coverage));
        }
        emit(JsonValue::object(std::move(members)));
    }

    void emit_partial_cancelled(const AnalyzeJobSpec &spec,
                                const std::vector<CandidateResult> &results) {
        std::vector<JsonValue> candidate_values;
        candidate_values.reserve(results.size());
        for (const CandidateResult &result : results) {
            candidate_values.push_back(JsonValue::object({
                {"id", JsonValue::string(result.id)},
                {"error", JsonValue::number(result.error)},
            }));
        }
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("cancelled")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"partial", JsonValue::boolean(!results.empty())},
            {"detail", JsonValue::string("cancelled")},
            {"payload", JsonValue::object({
                {"mode", JsonValue::string(spec.kernel_mode ? "kernel" : "height")},
                {"candidates", JsonValue::array(std::move(candidate_values))},
            })},
        }));
    }

    static double elapsed_ms(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }

    static AxisPlanRequest make_axis_request(std::int32_t source_size, double value,
                                             const Filter &filter) {
        AxisPlanRequest request;
        request.source_size = source_size;
        request.destination_size =
            static_cast<std::int32_t>(python_int(value));
        request.active_length = value;
        request.shift = 0.0;
        request.filter = filter;
        request.border = BorderMode::mirror;
        if (request.destination_size < 2 || request.destination_size >= source_size) {
            throw WorkerError(
                "bad_request",
                "candidate destination must be within 2..source_size-1");
        }
        return request;
    }

    static AxisPlanRequest make_geometry_axis_request(const Geometry &geometry,
                                                       bool horizontal,
                                                       std::int32_t source_size,
                                                       const Filter &filter) {
        AxisPlanRequest request;
        request.source_size = source_size;
        request.destination_size = static_cast<std::int32_t>(
            horizontal ? geometry.width : geometry.height);
        request.active_length = horizontal ? geometry.src_width : geometry.src_height;
        request.shift = horizontal ? geometry.src_left : geometry.src_top;
        request.filter = filter;
        request.border = BorderMode::mirror;
        if (request.destination_size < 2 || request.source_size < 2
            || request.destination_size >= request.source_size
            || !std::isfinite(request.active_length) || !std::isfinite(request.shift)
            || request.active_length <= 0.0 || request.shift < 0.0) {
            throw WorkerError("bad_request", "geometry destination must be within source bounds");
        }
        return request;
    }
};

} // namespace

int run_worker(std::istream &input, std::ostream &output, std::ostream &log) {
    WorkerSession session(output, log);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        std::string request_id;
        try {
            JsonValue command = parse_json(line);
            if (command.type != JsonValue::Type::object) {
                throw WorkerError("bad_request", "commands must be JSON objects");
            }
            if (const JsonValue *id = command.find("request_id")) {
                if (id->type == JsonValue::Type::string) request_id = id->string_value;
            }
            if (const JsonValue *version = command.find("protocol_version")) {
                if (version->type != JsonValue::Type::number
                    || version->number_value != static_cast<double>(kProtocolVersion)) {
                    throw WorkerError("protocol_error", "unsupported protocol_version");
                }
            } else {
                throw WorkerError("protocol_error", "missing protocol_version");
            }
            const std::string type = require_string(command, "type");
            if (type == "hello") {
                session.hello(command);
            } else if (type == "capabilities") {
                session.capabilities(command);
            } else if (type == "analyze") {
                session.analyze(command);
            } else if (type == "verify_begin") {
                session.verify_begin(command);
#if defined(GETNATIVE_HAS_MEDIA)
            } else if (type == "verify_media_begin") {
                session.verify_media_begin(command);
            } else if (type == "media_index_begin") {
                session.media_begin(command, MediaJobKind::index);
            } else if (type == "media_frame_window") {
                session.media_begin(command, MediaJobKind::frame_window);
            } else if (type == "media_preview_begin") {
                session.media_begin(command, MediaJobKind::preview);
            } else if (type == "media_asset_batch_begin") {
                session.media_begin(command, MediaJobKind::asset_batch);
#endif
            } else if (type == "verify_frame") {
                session.verify_frame(command);
            } else if (type == "verify_ring_attach") {
                session.verify_ring_attach(command);
            } else if (type == "verify_end") {
                session.verify_end(command);
            } else if (type == "cancel") {
                session.cancel(command);
            } else if (type == "shutdown") {
                session.shutdown(command);
            } else {
                throw WorkerError("bad_request", "unknown command type: " + type);
            }
        } catch (const JsonError &error) {
            WorkerError wrapped("bad_request",
                                "invalid JSON at offset "
                                    + std::to_string(error.offset()) + ": " + error.what());
            WorkerSession::emit_static_error(output, request_id, wrapped);
        } catch (const WorkerError &error) {
            WorkerSession::emit_static_error(output, request_id, error);
        } catch (const std::exception &error) {
            WorkerSession::emit_static_error(
                output, request_id, WorkerError("internal", error.what()));
        }
        if (session.shutdown_acknowledged()) break;
    }
    session.request_stop();
    return 0;
}

} // namespace getnative::cli
