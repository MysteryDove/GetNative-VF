#include "worker.hpp"

#include "capabilities.hpp"
#include "json.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/cpu_features.hpp"
#include "getnative/crop_geometry.hpp"
#include "getnative/plan_store.hpp"
#if defined(GETNATIVE_HAS_CUDA)
#include "getnative/cuda_analysis.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
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

// ---------------------------------------------------------------------------
// Analyze request model
// ---------------------------------------------------------------------------

enum class AxisMode : std::uint8_t { height_only, width_only, height_plus_width };
enum class BackendChoice : std::uint8_t { cpu, cuda };

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
    MetricSpec metric{};
    std::size_t worker_count = 0;
    std::int64_t expected_frames = -1;
};

struct VerifyFrameItem {
    std::uint64_t seq = 0;
    FrameAsset asset;
};

struct VerifyFrameResult {
    std::uint64_t seq = 0;
    std::optional<double> error;
};

AxisMode parse_axis_mode(const std::string &value) {
    if (value == "h_only") return AxisMode::height_only;
    if (value == "w_only") return AxisMode::width_only;
    if (value == "h_plus_w") return AxisMode::height_plus_width;
    throw WorkerError("bad_request", "unknown axis_mode: " + value);
}

Filter parse_filter(const JsonValue &kernel) {
    const std::string id = require_string(kernel, "id");
    if (id == "bilinear") return Filter::bilinear();
    if (id == "spline16") return Filter::spline16();
    if (id == "spline36") return Filter::spline36();
    if (id == "spline64") return Filter::spline64();
    if (id == "bicubic") {
        const JsonValue *b = kernel.find("b");
        const JsonValue *c = kernel.find("c");
        const double b_value = (b && !b->is_null()) ? require_number(kernel, "b") : 0.0;
        const double c_value = (c && !c->is_null()) ? require_number(kernel, "c") : 0.5;
        if (!std::isfinite(b_value) || !std::isfinite(c_value)) {
            throw WorkerError("bad_request", "bicubic parameters must be finite");
        }
        return Filter::bicubic(b_value, c_value);
    }
    if (id == "lanczos") {
        const JsonValue *taps = kernel.find("taps");
        const std::int32_t value = (taps && !taps->is_null())
            ? require_int(kernel, "taps")
            : 3;
        if (value < 1 || value > 15) {
            throw WorkerError("bad_request", "lanczos taps must be within 1..15");
        }
        return Filter::lanczos(value);
    }
    throw WorkerError("bad_request", "unknown kernel id: " + id);
}

// Canonical echo of a parsed kernel spec (defaults filled): bicubic always
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
    const std::int32_t p_norm = optional_int(metric, "p_norm", 1);
    if (p_norm != 1) {
        throw WorkerError("unsupported", "only p_norm=1 is available in protocol v1");
    }
    result.norm = static_cast<std::uint32_t>(p_norm);
    if (result.crop_left < 0 || result.crop_right < 0 || result.crop_top < 0
        || result.crop_bottom < 0) {
        throw WorkerError("bad_request", "crop values must be non-negative");
    }
    return result;
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
    if (backend == "cpu" || backend == "auto") {
        // auto keeps the current contract: CPU is the deterministic oracle.
        spec.backend = BackendChoice::cpu;
    } else if (backend == "cuda") {
        spec.backend = BackendChoice::cuda;
    } else {
        throw WorkerError("unsupported", "unknown backend: " + backend);
    }
    spec.frame = parse_frame_asset(require_member(command, "frame_asset"));
    spec.axis_mode = parse_axis_mode(require_string(command, "axis_mode"));
    spec.metric = parse_metric(require_member(command, "metric"));

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

VerifyJobSpec parse_verify_begin(const JsonValue &command, std::string job_id) {
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
    if (backend == "cpu" || backend == "auto") {
        // CPU is the deterministic oracle and the only verification backend
        // in protocol v1.1; CUDA frame streaming is a documented follow-up.
    } else if (backend == "cuda") {
        throw WorkerError(
            "unsupported",
            "verify mode is CPU-only in protocol v1.1 (CUDA verification is an E2 follow-up)");
    } else {
        throw WorkerError("unsupported", "unknown backend: " + backend);
    }

    spec.axis_mode = parse_axis_mode(require_string(command, "axis_mode"));
    spec.filter = parse_filter(require_member(command, "kernel"));
    spec.metric = parse_metric(require_member(command, "metric"));

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

    if (command.find("worker_count")) {
        const double workers = require_number(command, "worker_count");
        if (workers < 0.0 || std::trunc(workers) != workers) {
            throw WorkerError("bad_request", "worker_count must be a non-negative integer");
        }
        spec.worker_count = static_cast<std::size_t>(workers);
    }
    if (command.find("expected_frames")) {
        spec.expected_frames = require_int64(command, "expected_frames");
        if (spec.expected_frames < 1 || spec.expected_frames > kVerifyMaxFrames) {
            throw WorkerError("bad_request", "expected_frames must be within 1..1000000");
        }
    }
    return spec;
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

// Plan store directory resolution (E4). The store is OPT-IN: on hosts with
// fast NVMe and many cores the parallel batch build beats pack fetch latency
// at every measured shape (docs/performance/e4-cold-plan-store-20260808.md),
// so it serves low-parallelism hosts and future sparse patterns rather than
// the default path. GETNATIVE_PLAN_CACHE=on enables the platform cache root;
// GETNATIVE_PLAN_CACHE_DIR enables an explicit directory; "off" disables.
std::optional<std::filesystem::path> resolve_plan_store_dir() {
    const char *toggle = std::getenv("GETNATIVE_PLAN_CACHE");
    const bool enabled = toggle != nullptr && std::string_view{toggle} == "on";
    if (toggle != nullptr && std::string_view{toggle} == "off") return std::nullopt;
    if (!enabled && std::getenv("GETNATIVE_PLAN_CACHE_DIR") == nullptr) {
        return std::nullopt;
    }
    if (const char *explicit_dir = std::getenv("GETNATIVE_PLAN_CACHE_DIR")) {
        if (*explicit_dir != '\0') return std::filesystem::path{explicit_dir};
    }
#if defined(_WIN32)
    if (const char *local = std::getenv("LOCALAPPDATA")) {
        if (*local != '\0') return std::filesystem::path{local} / "getnative" / "plans";
    }
    return std::nullopt;
#else
    if (const char *xdg = std::getenv("XDG_CACHE_HOME")) {
        if (*xdg != '\0') return std::filesystem::path{xdg} / "getnative" / "plans";
    }
    if (const char *home = std::getenv("HOME")) {
        if (*home != '\0') {
            return std::filesystem::path{home} / ".cache" / "getnative" / "plans";
        }
    }
    return std::nullopt;
#endif
}

void load_frame_into(const FrameAsset &asset, float *destination) {
    const std::uint64_t elements =
        static_cast<std::uint64_t>(asset.width) * static_cast<std::uint64_t>(asset.height);
    const std::uint64_t bytes = elements * sizeof(float);
    std::ifstream input(asset.path, std::ios::binary | std::ios::ate);
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
        void *mapped = ::mmap(nullptr, bytes_, PROT_READ,
                              MAP_PRIVATE | MAP_POPULATE, fd, 0);
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
    std::string verify_cancel_detail;
};

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

class WorkerSession {
public:
    WorkerSession(std::ostream &output, std::ostream &log)
        : output_(output), log_(log), plan_cache_{} {
        executor_ = std::thread([this] { execute_loop(); });
        store_writer_ = std::thread([this] { store_write_loop(); });
    }

    ~WorkerSession() {
        request_stop();
        if (executor_.joinable()) executor_.join();
        {
            const std::scoped_lock lock(store_mutex_);
            store_stop_ = true;
        }
        store_condition_.notify_all();
        if (store_writer_.joinable()) store_writer_.join();
    }

    void request_stop() {
        std::deque<std::shared_ptr<Job>> dropped;
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
        for (const auto &job : dropped) {
            emit_cancelled(*job, false, "shutdown");
        }
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
            {"engine_version", JsonValue::string("0.1.0")},
            {"commands", JsonValue::object({
                {"analyze", JsonValue::boolean(true)},
                {"cancel", JsonValue::boolean(true)},
                {"verify_begin", JsonValue::boolean(true)},
                {"verify_frame", JsonValue::boolean(true)},
                {"verify_end", JsonValue::boolean(true)},
            })},
        }));
    }

    void capabilities(const JsonValue &command) {
        require_greeting();
        const std::string request_id = require_string(command, "request_id");
        std::ostringstream payload;
        write_capabilities(payload, true);
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
        auto job = std::make_shared<Job>(std::move(spec));
        // Emit accepted BEFORE the job becomes visible to the executor:
        // event order on the wire must place accepted ahead of any job
        // event (a warm plan cache makes plan progress otherwise racy).
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("accepted")},
            {"request_id", JsonValue::string(job->spec.request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("height")},
        }));
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

    void verify_frame(const JsonValue &command) {
        require_greeting();
        const std::string request_id = require_string(command, "request_id");
        const std::string job_id = require_string(command, "job_id");
        const std::int64_t seq_value = require_int64(command, "seq");
        if (seq_value < 0) {
            throw WorkerError("bad_request", "verify_frame seq must be non-negative");
        }
        FrameAsset asset = parse_frame_asset(require_member(command, "frame_asset"));
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
            if (asset.width != job->verify.width || asset.height != job->verify.height) {
                throw WorkerError(
                    "bad_request",
                    "verify frame asset geometry does not match the recipe");
            }
            job->verify_inbox.push_back({static_cast<std::uint64_t>(seq_value),
                                         std::move(asset)});
            ++job->verify_received;
        }
        job->verify_condition.notify_all();
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
        for (const auto &job : dropped) {
            emit_cancelled(*job, false, "shutdown");
        }
        if (executor_.joinable()) executor_.join();
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
#if defined(GETNATIVE_HAS_CUDA)
    std::optional<CudaAnalysisEngine> cuda_engine_;
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

    void require_greeting() {
        if (!greeted_) {
            throw WorkerError("protocol_error", "hello must be the first command");
        }
    }

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
                       JsonValue results) {
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
            log_ << "worker: plan store at " << directory->string() << '\n';
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

    // -----------------------------------------------------------------------
    // Job execution
    // -----------------------------------------------------------------------

    void run_job(Job &job) {
        if (job.kind == JobKind::verify) {
            run_verify_job(job);
            return;
        }
        run_analyze_job(job);
    }

    void run_analyze_job(Job &job) {
        const AnalyzeJobSpec &spec = job.spec;
        const auto job_start = std::chrono::steady_clock::now();

        const FrameCache::Entry &frame_entry = frame_cache_.get(spec.frame);
        ConstImageView source{
            frame_entry.pixels.data(), frame_entry.width, frame_entry.height,
            frame_entry.width};

        const std::int32_t primary_size =
            spec.axis_mode == AxisMode::width_only ? source.width : source.height;
        const std::int32_t secondary_size =
            spec.axis_mode == AxisMode::width_only ? source.height : source.width;

        // Candidate axis requests (primary axis follows the candidate grid;
        // the secondary axis in h_plus_w mode is derived from the aspect
        // ratio, matching the standard integer-width rule). Kernel mode maps
        // each kernel filter onto the single fixed axis value instead.
        std::vector<double> values;
        values.reserve(spec.candidates.size());
        for (const std::string &decimal : spec.candidates) {
            const double value = parse_json(decimal).number_value;
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
        if (spec.kernel_mode) {
            for (const Filter &filter : spec.kernel_filters) {
                requests.push_back(make_axis_request(primary_size, values.front(), filter));
            }
        } else {
            for (const double value : values) {
                requests.push_back(make_axis_request(primary_size, value, spec.filter));
            }
        }
        if (spec.axis_mode == AxisMode::height_plus_width) {
            if (spec.kernel_mode) {
                const double derived = static_cast<double>(secondary_size)
                    * values.front() / static_cast<double>(primary_size);
                if (derived < 2.0) {
                    throw WorkerError("bad_request", "derived secondary axis length is too small");
                }
                for (const Filter &filter : spec.kernel_filters) {
                    requests.push_back(make_axis_request(secondary_size, derived, filter));
                }
            } else {
                for (const double value : values) {
                    const double derived =
                        static_cast<double>(secondary_size) * value / static_cast<double>(primary_size);
                    if (derived < 2.0) {
                        throw WorkerError("bad_request", "derived secondary axis length is too small");
                    }
                    requests.push_back(make_axis_request(secondary_size, derived, spec.filter));
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
        // CUDA path can run chunks through a pipeline of threads: the
        // engine's slot pool overlaps chunk N's device execution with chunk
        // N+1's host pack + upload (the unique-candidate-scan wall measured
        // in docs/performance/e3-kernel-increments-20260808.md §2).
        const auto candidates_start = std::chrono::steady_clock::now();
        std::vector<CandidateResult> results(result_count);

#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda) {
            if (!cuda_engine_) {
                try {
                    log_ << "worker: initializing CUDA analysis engine...\n";
                    cuda_engine_.emplace();
                } catch (const std::exception &error) {
                    throw WorkerError(
                        "unsupported",
                        std::string{"CUDA backend is not available: "} + error.what());
                }
                log_ << "worker: CUDA analysis engine initialized on "
                     << cuda_engine_->device_info().name << '\n';
            }
            cuda_engine_->reset_analysis_telemetry();
        }
#else
        if (spec.backend == BackendChoice::cuda) {
            throw WorkerError("unsupported", "CUDA backend was not compiled");
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

#if defined(GETNATIVE_HAS_CUDA)
        if (spec.backend == BackendChoice::cuda && chunk_total > 1U) {
            // Pipeline depth: worker_count when given (1..8), else 2 —
            // measured sweet spot (p2 vs p1: -24%/-32% candidate phase on
            // lanczos3/8; p4+ shows no further gain).
            const std::size_t requested = spec.worker_count != 0U
                ? std::max<std::size_t>(1U, std::min<std::size_t>(8U, spec.worker_count))
                : 2U;
            const std::size_t thread_count = std::min(requested, chunk_total);
            std::atomic_size_t cursor{0U};
            std::atomic_size_t done_candidates{0U};
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
                                    cuda_engine_->analyze_axis_batch_f32(
                                        source, chunk, spec.metric,
                                        job.stop_source.get_token());
                                for (std::size_t offset = 0;
                                     offset < chunk_results.size(); ++offset) {
                                    results[begin + offset] =
                                        std::move(chunk_results[offset]);
                                }
                                chunk_done[chunk_index].store(
                                    true, std::memory_order_release);
                                const std::size_t done = done_candidates.fetch_add(
                                    chunk_results.size(), std::memory_order_relaxed)
                                    + chunk_results.size();
                                emit_progress(spec, "candidates", done,
                                              result_count);
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
        } else
#endif
        {
            for (std::size_t chunk_index = 0; chunk_index < chunk_total;
                 ++chunk_index) {
                std::vector<CandidateAnalysis> chunk;
                build_chunk(chunk_index, chunk);
                std::vector<CandidateResult> chunk_results;
                try {
#if defined(GETNATIVE_HAS_CUDA)
                    if (spec.backend == BackendChoice::cuda) {
                        chunk_results = cuda_engine_->analyze_axis_batch_f32(
                            source, chunk, spec.metric, job.stop_source.get_token());
                    } else
#endif
                    {
                        chunk_results = analyze_batch_f32(
                            source, chunk, spec.metric, spec.worker_count);
                    }
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

        const char *backend_name = spec.backend == BackendChoice::cuda ? "cuda" : "cpu";
        std::vector<std::pair<std::string, JsonValue>> telemetry_members = {
            {"plan_build_count", JsonValue::integer(static_cast<std::int64_t>(builds))},
            {"plan_cache_hits", JsonValue::integer(static_cast<std::int64_t>(cache_hits))},
            {"plan_store_hits", JsonValue::integer(static_cast<std::int64_t>(store_hits))},
            {"plan_store_fetch_ms", JsonValue::number(store_fetch_ms)},
            {"plan_resident_entries", JsonValue::integer(
                static_cast<std::int64_t>(plan_cache_.size()))},
            {"plan_ms", JsonValue::number(plan_ms)},
            {"candidates_ms", JsonValue::number(candidates_ms)},
            {"total_ms", JsonValue::number(elapsed_ms(job_start))},
            {"backend", JsonValue::string(backend_name)},
            {"isa", JsonValue::string(std::string{
                cpu_isa_name(cpu_dispatch_info().selected)})},
        };
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

    // One locked recipe, many streamed frames. Analysis workers consume the
    // inbox as the reader thread appends verify_frame items, so decode on the
    // producer side overlaps analysis naturally; the app bounds its asset
    // production with the accepted event's suggested_in_flight hint.
    void run_verify_job(Job &job) {
        const VerifyJobSpec &spec = job.verify;
        const auto job_start = std::chrono::steady_clock::now();

        const std::int32_t primary_size =
            spec.axis_mode == AxisMode::width_only ? spec.width : spec.height;
        const std::int32_t secondary_size =
            spec.axis_mode == AxisMode::width_only ? spec.height : spec.width;
        const double value = parse_json(spec.candidate).number_value;

        std::vector<AxisPlanRequest> requests;
        requests.push_back(make_axis_request(primary_size, value, spec.filter));
        if (spec.axis_mode == AxisMode::height_plus_width) {
            const double derived = static_cast<double>(secondary_size) * value
                / static_cast<double>(primary_size);
            if (derived < 2.0) {
                throw WorkerError("bad_request", "derived secondary axis length is too small");
            }
            requests.push_back(make_axis_request(secondary_size, derived, spec.filter));
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
#if !defined(_WIN32)
                    MappedFrame mapped(item.asset);
                    ConstImageView view{mapped.data(), spec.width, spec.height, spec.width};
#else
                    load_frame_into(item.asset, buffer.data());
                    ConstImageView view{buffer.data(), spec.width, spec.height, spec.width};
#endif
                    frame_load_ms.fetch_add(elapsed_ms(load_start),
                                            std::memory_order_relaxed);
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
                    error = frame_error;
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
                    return job.verify_outbox.size() >= kVerifyResultBatchSize
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
                    {"plan_ms", JsonValue::number(plan_ms)},
                    {"frame_load_ms", JsonValue::number(frame_load_ms.load())},
                    {"frame_analyze_ms", JsonValue::number(frame_analyze_ms.load())},
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
                               std::uint64_t failed, const std::string &detail) {
        emit(JsonValue::object({
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
        }));
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
            } else if (type == "verify_frame") {
                session.verify_frame(command);
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
