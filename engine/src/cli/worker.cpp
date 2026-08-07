#include "worker.hpp"

#include "capabilities.hpp"
#include "json.hpp"

#include "getnative/axis_plan.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/cpu_features.hpp"
#include "getnative/crop_geometry.hpp"
#if defined(GETNATIVE_HAS_CUDA)
#include "getnative/cuda_analysis.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
    MetricSpec metric{};
    std::size_t worker_count = 0;
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
    if (mode != "height") {
        throw WorkerError("unsupported", "only mode=height is available in protocol v1");
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
    spec.filter = parse_filter(require_member(command, "kernel"));
    spec.metric = parse_metric(require_member(command, "metric"));

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
// Frame asset loading
// ---------------------------------------------------------------------------

std::vector<float> load_frame_asset(const FrameAsset &asset) {
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
    std::vector<float> frame(static_cast<std::size_t>(elements));
    input.read(reinterpret_cast<char *>(frame.data()), static_cast<std::streamsize>(bytes));
    if (!input) {
        throw WorkerError("frame_asset_error", "failed while reading frame asset: " + asset.path, true);
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Worker session
// ---------------------------------------------------------------------------

struct Job {
    explicit Job(AnalyzeJobSpec job_spec) : spec(std::move(job_spec)) {}

    AnalyzeJobSpec spec;
    std::atomic<bool> cancel_requested{false};
    std::stop_source stop_source;
    bool started = false;
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
    }

    ~WorkerSession() {
        request_stop();
        if (executor_.joinable()) executor_.join();
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
            if (running_) running_->cancel_requested.store(true);
        }
        condition_.notify_all();
        for (const auto &job : dropped) {
            emit_cancelled(job->spec, false, "shutdown");
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
        {
            const std::scoped_lock lock(mutex_);
            queue_.push_back(job);
        }
        condition_.notify_one();
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("accepted")},
            {"request_id", JsonValue::string(job->spec.request_id)},
            {"job_id", JsonValue::string(job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("height")},
        }));
    }

    void cancel(const JsonValue &command) {
        require_greeting();
        const std::string request_id = require_string(command, "request_id");
        const std::string job_id = require_string(command, "job_id");
        bool found = false;
        std::shared_ptr<Job> queued;
        {
            const std::scoped_lock lock(mutex_);
            if (running_ && running_->spec.job_id == job_id) {
                running_->cancel_requested.store(true);
                running_->stop_source.request_stop();
                found = true;
            } else {
                for (auto iterator = queue_.begin(); iterator != queue_.end(); ++iterator) {
                    if ((*iterator)->spec.job_id == job_id) {
                        queued = std::move(*iterator);
                        queue_.erase(iterator);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (queued) {
            emit_cancelled(queued->spec, false, "cancelled_before_start");
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
            if (running_) running_->cancel_requested.store(true);
        }
        condition_.notify_all();
        for (const auto &job : dropped) {
            emit_cancelled(job->spec, false, "shutdown");
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
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("progress")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"phase", JsonValue::string(std::string{phase})},
            {"completed", JsonValue::integer(static_cast<std::int64_t>(completed))},
            {"total", JsonValue::integer(static_cast<std::int64_t>(total))},
        }));
    }

    void emit_cancelled(const AnalyzeJobSpec &spec, bool partial,
                        std::string_view detail) {
        emit(JsonValue::object({
            {"protocol_version", JsonValue::integer(kProtocolVersion)},
            {"type", JsonValue::string("cancelled")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"partial", JsonValue::boolean(partial)},
            {"detail", JsonValue::string(std::string{detail})},
        }));
    }

    static void check_cancelled(const Job &job) {
        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            throw WorkerError("cancelled", "job was cancelled");
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
                        emit_cancelled(job->spec, false, "cancelled");
                    } else {
                        emit_error(job->spec.request_id, error);
                    }
                } catch (...) {
                }
            } catch (const std::exception &error) {
                try {
                    emit_error(job->spec.request_id,
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
        // ratio, matching the standard integer-width rule).
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

        std::vector<AxisPlanRequest> requests;
        requests.reserve(spec.axis_mode == AxisMode::height_plus_width
                             ? values.size() * 2U
                             : values.size());
        for (const double value : values) {
            requests.push_back(make_axis_request(primary_size, value, spec.filter));
        }
        if (spec.axis_mode == AxisMode::height_plus_width) {
            for (const double value : values) {
                const double derived =
                    static_cast<double>(secondary_size) * value / static_cast<double>(primary_size);
                if (derived < 2.0) {
                    throw WorkerError("bad_request", "derived secondary axis length is too small");
                }
                requests.push_back(make_axis_request(secondary_size, derived, spec.filter));
            }
        }

        // Plan phase with chunked progress + cancellation checkpoints.
        const auto plan_start = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<const AxisPlan>> plans;
        plans.reserve(requests.size());
        std::size_t cache_hits = 0;
        std::size_t builds = 0;
        for (std::size_t begin = 0; begin < requests.size(); begin += kPlanChunkSize) {
            check_cancelled(job);
            const std::size_t end = std::min(begin + kPlanChunkSize, requests.size());
            AxisPlanCacheBatchResult batch = plan_cache_.get_or_build_batch(
                std::span<const AxisPlanRequest>{requests.data() + begin, end - begin});
            cache_hits += batch.ready_hit_count;
            builds += batch.physical_build_count;
            plans.insert(plans.end(),
                         std::make_move_iterator(batch.plans.begin()),
                         std::make_move_iterator(batch.plans.end()));
            emit_progress(spec, "plan", plans.size(), requests.size());
        }
        const double plan_ms = elapsed_ms(plan_start);

        // Candidate phase.
        const auto candidates_start = std::chrono::steady_clock::now();
        std::vector<CandidateResult> results;
        results.reserve(spec.candidates.size());

#if defined(GETNATIVE_HAS_CUDA)
        CudaRuntimeTelemetry cuda_start_telemetry;
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

        for (std::size_t begin = 0; begin < spec.candidates.size();
             begin += kCandidateChunkSize) {
            const std::size_t end =
                std::min(begin + kCandidateChunkSize, spec.candidates.size());
            std::vector<CandidateAnalysis> chunk;
            chunk.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                CandidateAnalysis candidate;
                candidate.id = spec.candidates[index];
                if (spec.axis_mode == AxisMode::width_only) {
                    candidate.horizontal = plans[index];
                    candidate.axes = AnalysisAxes::horizontal;
                } else if (spec.axis_mode == AxisMode::height_only) {
                    candidate.vertical = plans[index];
                    candidate.axes = AnalysisAxes::vertical;
                } else {
                    candidate.vertical = plans[index];
                    candidate.horizontal = plans[values.size() + index];
                    candidate.axes = AnalysisAxes::both;
                }
                chunk.push_back(std::move(candidate));
            }
            std::vector<CandidateResult> chunk_results;
            try {
                if (spec.backend == BackendChoice::cpu) {
                    chunk_results = analyze_batch_f32(
                        source, chunk, spec.metric, spec.worker_count);
                }
#if defined(GETNATIVE_HAS_CUDA)
                else {
                    chunk_results = cuda_engine_->analyze_axis_batch_f32(
                        source, chunk, spec.metric, job.stop_source.get_token());
                }
#endif
            } catch (...) {
                check_cancelled(job);
                throw;
            }
            results.insert(results.end(),
                           std::make_move_iterator(chunk_results.begin()),
                           std::make_move_iterator(chunk_results.end()));
            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                emit_partial_cancelled(spec, results);
                return;
            }
            emit_progress(spec, "candidates", results.size(), spec.candidates.size());
        }
        const double candidates_ms = elapsed_ms(candidates_start);

        const char *backend_name = spec.backend == BackendChoice::cuda ? "cuda" : "cpu";
        std::vector<std::pair<std::string, JsonValue>> telemetry_members = {
            {"plan_build_count", JsonValue::integer(static_cast<std::int64_t>(builds))},
            {"plan_cache_hits", JsonValue::integer(static_cast<std::int64_t>(cache_hits))},
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
                "cuda_device", JsonValue::string(cuda_engine_->device_info().name));
        }
#endif

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
            {"type", JsonValue::string("result")},
            {"request_id", JsonValue::string(spec.request_id)},
            {"job_id", JsonValue::string(spec.job_id)},
            {"timestamp_ms", JsonValue::integer(timestamp_ms())},
            {"mode", JsonValue::string("height")},
            {"payload", JsonValue::object({
                {"mode", JsonValue::string("height")},
                {"candidates", JsonValue::array(std::move(candidate_values))},
                {"telemetry", JsonValue::object(std::move(telemetry_members))},
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
                {"mode", JsonValue::string("height")},
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
