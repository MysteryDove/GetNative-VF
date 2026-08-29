#include "benchmark_support.hpp"

#include "getnative/filter.hpp"
#include "getnative/metal_analysis.hpp"
#include "getnative/joining_thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr double metal_absolute_tolerance = 1e-7;
constexpr double metal_relative_tolerance = 5e-4;
constexpr double repeat_tolerance = 1e-12;
constexpr double noise_gate = 0.10;

enum class ProfileArm {
    cpu_serial,
    cpu_frame,
    metal,
};

struct Configuration {
    std::vector<std::size_t> frame_counts{1U, 2U, 10U, 100U, 1000U};
    std::size_t samples = 3U;
    std::size_t frame_workers = 0U;
    std::size_t ring_size = 8U;
    std::size_t tile_size = 32U;
    std::size_t reduction_groups = 8U;
    std::size_t inverse_threads = 32U;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    std::int32_t native_height = 810;
    std::string kernel = "bicubic-catrom";
    double blur = 1.0;
    bool assert_gates = false;
    bool profile_split_kernels = false;
    std::optional<ProfileArm> profile_arm;
    std::optional<std::filesystem::path> json_output;
};

struct Fixture {
    std::vector<std::vector<float>> frames;
    std::shared_ptr<const getnative::AxisPlan> plan;
    double plan_preparation_ms = 0.0;
    std::string plan_identity;
};

struct CpuRun {
    std::vector<double> values;
    double wall_ms = 0.0;
    std::size_t effective_workers = 1U;
};

struct MetalRun {
    std::vector<double> values;
    double wall_ms = 0.0;
    double plan_buffer_allocation_ms = 0.0;
    double pipeline_creation_ms = 0.0;
    double host_residual_ms = 0.0;
    getnative::MetalRuntimeTelemetry telemetry;
};

struct CaseSamples {
    std::size_t frame_count = 0U;
    std::size_t effective_frame_workers = 0U;
    std::vector<std::string> arm_order;
    std::vector<double> cpu_serial_wall_ms;
    std::vector<double> cpu_serial_throughput_fps;
    std::vector<double> cpu_serial_cold_total_ms;
    std::vector<double> cpu_parallel_wall_ms;
    std::vector<double> cpu_parallel_throughput_fps;
    std::vector<double> cpu_parallel_cold_total_ms;
    std::vector<double> metal_wall_ms;
    std::vector<double> metal_throughput_fps;
    std::vector<double> metal_cold_total_ms;
    std::vector<double> metal_buffer_allocation_ms;
    std::vector<double> metal_working_buffer_allocation_ms;
    std::vector<double> metal_plan_buffer_allocation_ms;
    std::vector<double> metal_source_upload_ms;
    std::vector<double> metal_plan_upload_ms;
    std::vector<double> metal_buffer_wiring_ms;
    std::vector<double> metal_pipeline_creation_ms;
    std::vector<double> metal_gpu_execution_ms;
    std::vector<double> metal_host_residual_ms;
    std::vector<double> metal_plan_upload_bytes;
    std::vector<double> metal_command_submissions;
    std::vector<double> cpu_parallel_speedup;
    std::vector<double> cpu_serial_to_metal_speedup;
    std::vector<double> cpu_parallel_to_metal_speedup;
    double maximum_cpu_parallel_error = 0.0;
    double maximum_metal_error = 0.0;
    double maximum_cpu_repeat_error = 0.0;
    double maximum_metal_repeat_error = 0.0;
    bool correctness_pass = true;
    bool result_stability_pass = true;
    std::string cpu_result_identity;
    std::string metal_result_identity;
};

struct CaseSummary {
    std::size_t frame_count = 0U;
    std::size_t effective_frame_workers = 0U;
    getnative::benchmark::Summary cpu_serial_wall_ms;
    getnative::benchmark::Summary cpu_serial_throughput_fps;
    getnative::benchmark::Summary cpu_serial_cold_total_ms;
    getnative::benchmark::Summary cpu_parallel_wall_ms;
    getnative::benchmark::Summary cpu_parallel_throughput_fps;
    getnative::benchmark::Summary cpu_parallel_cold_total_ms;
    getnative::benchmark::Summary metal_wall_ms;
    getnative::benchmark::Summary metal_throughput_fps;
    getnative::benchmark::Summary metal_cold_total_ms;
    getnative::benchmark::Summary metal_buffer_allocation_ms;
    getnative::benchmark::Summary metal_working_buffer_allocation_ms;
    getnative::benchmark::Summary metal_plan_buffer_allocation_ms;
    getnative::benchmark::Summary metal_source_upload_ms;
    getnative::benchmark::Summary metal_plan_upload_ms;
    getnative::benchmark::Summary metal_buffer_wiring_ms;
    getnative::benchmark::Summary metal_pipeline_creation_ms;
    getnative::benchmark::Summary metal_gpu_execution_ms;
    getnative::benchmark::Summary metal_host_residual_ms;
    getnative::benchmark::Summary metal_plan_upload_bytes;
    getnative::benchmark::Summary metal_command_submissions;
    getnative::benchmark::Summary cpu_parallel_speedup;
    getnative::benchmark::Summary cpu_serial_to_metal_speedup;
    getnative::benchmark::Summary cpu_parallel_to_metal_speedup;
    double maximum_cpu_parallel_error = 0.0;
    double maximum_metal_error = 0.0;
    double maximum_cpu_repeat_error = 0.0;
    double maximum_metal_repeat_error = 0.0;
    bool correctness_pass = true;
    bool result_stability_pass = true;
    bool noise_pass = true;
    std::string cpu_result_identity;
    std::string metal_result_identity;
    std::vector<std::string> arm_order;
};

[[nodiscard]] double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::size_t parse_size(std::string_view text) {
    std::size_t value = 0U;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0U) {
        throw std::invalid_argument("numeric option must be a positive integer");
    }
    return value;
}

[[nodiscard]] std::vector<std::size_t> parse_frame_counts(std::string_view text) {
    std::vector<std::size_t> result;
    std::size_t begin = 0U;
    while (begin < text.size()) {
        const std::size_t separator = text.find(',', begin);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        if (end == begin) throw std::invalid_argument("frame list contains an empty item");
        result.push_back(parse_size(text.substr(begin, end - begin)));
        if (separator == std::string_view::npos) break;
        begin = separator + 1U;
    }
    if (result.empty()) throw std::invalid_argument("frame list must not be empty");
    if (!std::is_sorted(result.begin(), result.end())
        || std::adjacent_find(result.begin(), result.end()) != result.end()) {
        throw std::invalid_argument("frame list must be strictly increasing");
    }
    return result;
}

[[nodiscard]] std::int32_t parse_i32(std::string_view text) {
    const std::size_t value = parse_size(text);
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument("dimension exceeds int32 range");
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] ProfileArm parse_profile_arm(std::string_view text) {
    if (text == "cpu-serial") return ProfileArm::cpu_serial;
    if (text == "cpu-frame") return ProfileArm::cpu_frame;
    if (text == "metal") return ProfileArm::metal;
    throw std::invalid_argument(
        "profile arm must be cpu-serial, cpu-frame, or metal");
}

[[nodiscard]] std::string_view profile_arm_name(ProfileArm arm) {
    switch (arm) {
    case ProfileArm::cpu_serial: return "cpu-serial";
    case ProfileArm::cpu_frame: return "cpu-frame";
    case ProfileArm::metal: return "metal";
    }
    throw std::logic_error("unknown profile arm");
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv) {
    Configuration result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--frames" && index + 1 < argc) {
            result.frame_counts = parse_frame_counts(argv[++index]);
        } else if (argument == "--samples" && index + 1 < argc) {
            result.samples = parse_size(argv[++index]);
        } else if (argument == "--frame-workers" && index + 1 < argc) {
            result.frame_workers = parse_size(argv[++index]);
        } else if (argument == "--ring-size" && index + 1 < argc) {
            result.ring_size = parse_size(argv[++index]);
        } else if (argument == "--tile-size" && index + 1 < argc) {
            result.tile_size = parse_size(argv[++index]);
        } else if (argument == "--reduction-groups" && index + 1 < argc) {
            result.reduction_groups = parse_size(argv[++index]);
        } else if (argument == "--inverse-threads" && index + 1 < argc) {
            result.inverse_threads = parse_size(argv[++index]);
        } else if (argument == "--width" && index + 1 < argc) {
            result.width = parse_i32(argv[++index]);
        } else if (argument == "--height" && index + 1 < argc) {
            result.height = parse_i32(argv[++index]);
        } else if (argument == "--native-height" && index + 1 < argc) {
            result.native_height = parse_i32(argv[++index]);
        } else if (argument == "--kernel" && index + 1 < argc) {
            result.kernel = argv[++index];
        } else if (argument == "--blur" && index + 1 < argc) {
            result.blur = std::stod(argv[++index]);
        } else if (argument == "--profile-arm" && index + 1 < argc) {
            result.profile_arm = parse_profile_arm(argv[++index]);
        } else if (argument == "--profile-split-kernels") {
            result.profile_split_kernels = true;
        } else if (argument == "--json-out" && index + 1 < argc) {
            result.json_output = std::filesystem::path{argv[++index]};
        } else if (argument == "--assert") {
            result.assert_gates = true;
        } else {
            throw std::invalid_argument(
                "usage: getnative_fixed_recipe_benchmark "
                "[--frames 1,2,10,100,1000] [--samples N] [--frame-workers N] "
                "[--ring-size N] [--tile-size N] [--reduction-groups N] "
                "[--inverse-threads N] [--width N] [--height N] "
                "[--native-height N] [--kernel NAME] [--blur SCALE] [--json-out PATH] [--assert] "
                "[--profile-arm cpu-serial|cpu-frame|metal] "
                "[--profile-split-kernels]");
        }
    }
    if (result.native_height >= result.height) {
        throw std::invalid_argument("native height must be smaller than source height");
    }
    if (!(result.blur > 0.0) || !std::isfinite(result.blur)) {
        throw std::invalid_argument("blur must be finite and greater than zero");
    }
    if (result.profile_arm && result.frame_counts.size() != 1U) {
        throw std::invalid_argument("profile mode requires exactly one frame count");
    }
    if (result.profile_arm && (result.json_output || result.assert_gates)) {
        throw std::invalid_argument(
            "profile mode does not emit benchmark JSON or evaluate benchmark gates");
    }
    return result;
}

[[nodiscard]] getnative::Filter benchmark_filter(std::string_view name) {
    if (name == "bilinear") return getnative::Filter::bilinear();
    if (name == "bicubic" || name == "bicubic-catrom") {
        return getnative::Filter::bicubic(0.0, 0.5);
    }
    if (name == "bicubic-mitchell") {
        return getnative::Filter::bicubic(1.0 / 3.0, 1.0 / 3.0);
    }
    if (name == "spline16") return getnative::Filter::spline16();
    if (name == "spline36") return getnative::Filter::spline36();
    if (name == "spline64") return getnative::Filter::spline64();
    if (name == "lanczos1") return getnative::Filter::lanczos(1);
    if (name == "lanczos2") return getnative::Filter::lanczos(2);
    if (name == "lanczos3") return getnative::Filter::lanczos(3);
    if (name == "lanczos4") return getnative::Filter::lanczos(4);
    if (name == "lanczos5") return getnative::Filter::lanczos(5);
    if (name == "lanczos6") return getnative::Filter::lanczos(6);
    if (name == "lanczos7") return getnative::Filter::lanczos(7);
    if (name == "lanczos8") return getnative::Filter::lanczos(8);
    throw std::invalid_argument("unsupported benchmark kernel");
}

void fnv1a_bytes(std::uint64_t &hash, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
}

template <class T>
void fnv1a_value(std::uint64_t &hash, const T &value) {
    fnv1a_bytes(hash, &value, sizeof(value));
}

template <class T>
void fnv1a_vector(std::uint64_t &hash, const std::vector<T> &values) {
    fnv1a_bytes(hash, values.data(), values.size() * sizeof(T));
}

[[nodiscard]] std::string hexadecimal(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

[[nodiscard]] std::string plan_identity(const getnative::AxisPlan &plan) {
    std::uint64_t hash = 1469598103934665603ULL;
    fnv1a_value(hash, plan.source_size);
    fnv1a_value(hash, plan.destination_size);
    fnv1a_value(hash, plan.support);
    fnv1a_value(hash, plan.half_bandwidth);
    fnv1a_value(hash, plan.forward_width);
    fnv1a_value(hash, plan.active_length);
    fnv1a_value(hash, plan.shift);
    fnv1a_vector(hash, plan.forward_offsets);
    fnv1a_vector(hash, plan.forward_indices);
    fnv1a_vector(hash, plan.forward_weights);
    fnv1a_vector(hash, plan.transpose_offsets);
    fnv1a_vector(hash, plan.transpose_indices);
    fnv1a_vector(hash, plan.transpose_weights);
    fnv1a_vector(hash, plan.lower_ld);
    fnv1a_vector(hash, plan.upper_l);
    fnv1a_vector(hash, plan.inverse_diagonal);
    return hexadecimal(hash);
}

[[nodiscard]] std::string result_identity(const std::vector<double> &values) {
    std::uint64_t hash = 1469598103934665603ULL;
    fnv1a_vector(hash, values);
    return hexadecimal(hash);
}

[[nodiscard]] Fixture make_fixture(
    const Configuration &config,
    const getnative::Filter &filter) {
    Fixture result;
    const getnative::AxisPlanRequest request{
        config.height,
        config.native_height,
        static_cast<double>(config.native_height),
        0.0,
        filter,
        getnative::BorderMode::mirror,
    };
    const auto plan_start = Clock::now();
    result.plan = std::make_shared<const getnative::AxisPlan>(
        getnative::build_axis_plan(request));
    result.plan_preparation_ms = elapsed_ms(plan_start);
    result.plan_identity = plan_identity(*result.plan);

    const std::size_t image_elements = static_cast<std::size_t>(config.width)
        * static_cast<std::size_t>(config.height);
    result.frames.resize(config.ring_size);
    for (std::size_t frame_index = 0; frame_index < config.ring_size; ++frame_index) {
        auto &frame = result.frames[frame_index];
        frame.resize(image_elements);
        for (std::size_t index = 0; index < image_elements; ++index) {
            const std::uint64_t mixed = index * 1315423911ULL
                + frame_index * 2654435761ULL
                + (index >> 5U) * 2246822519ULL;
            frame[index] = static_cast<float>((mixed & 0xffffU) / 65535.0);
        }
    }
    return result;
}

[[nodiscard]] getnative::ConstImageView frame_view(
    const Configuration &config,
    const Fixture &fixture,
    std::size_t logical_frame) {
    const auto &frame = fixture.frames[logical_frame % fixture.frames.size()];
    return {frame.data(), config.width, config.height, config.width};
}

[[nodiscard]] CpuRun run_cpu_serial(
    const Configuration &config,
    const Fixture &fixture,
    const getnative::MetricSpec &metric,
    std::size_t frame_count) {
    const std::array<getnative::CandidateAnalysis, 1U> candidate{{
        {"locked-recipe", nullptr, fixture.plan, getnative::AnalysisAxes::vertical},
    }};
    const auto start = Clock::now();
    std::vector<double> values(frame_count);
    for (std::size_t index = 0; index < frame_count; ++index) {
        const auto result = getnative::analyze_batch_f32(
            frame_view(config, fixture, index), candidate, metric);
        if (result.size() != 1U || result.front().id != "locked-recipe") {
            throw std::runtime_error("CPU result changed fixed-recipe identity");
        }
        values[index] = result.front().error;
    }
    return {std::move(values), elapsed_ms(start), 1U};
}

[[nodiscard]] CpuRun run_cpu_parallel(
    const Configuration &config,
    const Fixture &fixture,
    const getnative::MetricSpec &metric,
    std::size_t frame_count,
    std::size_t worker_limit) {
    const auto start = Clock::now();
    std::vector<double> values(frame_count);
    const std::size_t worker_count = std::min(frame_count, worker_limit);
    std::atomic_size_t cursor{0U};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    std::vector<getnative::JoiningThread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            getnative::CpuWorkspace workspace;
            while (true) {
                const std::size_t index = cursor.fetch_add(1U, std::memory_order_relaxed);
                if (index >= frame_count) break;
                try {
                    values[index] = getnative::analyze_axis_candidate_f32(
                        frame_view(config, fixture, index), *fixture.plan,
                        getnative::AnalysisAxes::vertical, metric, workspace);
                } catch (...) {
                    const std::scoped_lock lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                    cursor.store(frame_count, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    workers.clear();
    if (failure) std::rethrow_exception(failure);
    return {std::move(values), elapsed_ms(start), worker_count};
}

[[nodiscard]] MetalRun run_metal(
    const Configuration &config,
    const Fixture &fixture,
    const getnative::MetricSpec &metric,
    std::size_t frame_count,
    getnative::MetalAnalysisEngine &metal) {
    const std::array<getnative::CandidateAnalysis, 1U> candidate{{
        {"locked-recipe", nullptr, fixture.plan, getnative::AnalysisAxes::vertical},
    }};
    metal.reset_analysis_telemetry();
    const double pipeline_before = metal.runtime_telemetry().pipeline_creation_ms;
    const auto start = Clock::now();
    std::vector<double> values(frame_count);
    for (std::size_t index = 0; index < frame_count; ++index) {
        const auto result = metal.analyze_axis_batch_f32(
            frame_view(config, fixture, index), candidate, metric);
        if (result.size() != 1U || result.front().id != "locked-recipe") {
            throw std::runtime_error("Metal result changed fixed-recipe identity");
        }
        values[index] = result.front().error;
    }
    const double wall_ms = elapsed_ms(start);
    auto telemetry = metal.runtime_telemetry();
    const double pipeline_creation_ms = std::max(
        0.0, telemetry.pipeline_creation_ms - pipeline_before);
    const double plan_buffer_allocation_ms = std::max(
        0.0, telemetry.buffer_allocation_ms - telemetry.working_buffer_allocation_ms);
    const double accounted_ms = telemetry.buffer_allocation_ms
        + telemetry.source_upload_ms
        + telemetry.plan_upload_ms
        + telemetry.buffer_wiring_ms
        + pipeline_creation_ms
        + telemetry.gpu_execution_ms;
    const double host_residual_ms = std::max(0.0, wall_ms - accounted_ms);
    return {
        std::move(values), wall_ms, plan_buffer_allocation_ms,
        pipeline_creation_ms, host_residual_ms, std::move(telemetry),
    };
}

[[nodiscard]] double maximum_difference(
    const std::vector<double> &lhs,
    const std::vector<double> &rhs) {
    if (lhs.size() != rhs.size()) throw std::runtime_error("result length changed");
    double result = 0.0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        result = std::max(result, std::abs(lhs[index] - rhs[index]));
    }
    return result;
}

[[nodiscard]] bool metal_within_tolerance(
    const std::vector<double> &cpu,
    const std::vector<double> &metal,
    double &maximum_error) {
    if (cpu.size() != metal.size()) return false;
    bool pass = true;
    for (std::size_t index = 0; index < cpu.size(); ++index) {
        const double error = std::abs(cpu[index] - metal[index]);
        maximum_error = std::max(maximum_error, error);
        const double tolerance = std::max(
            metal_absolute_tolerance,
            metal_relative_tolerance * std::abs(cpu[index]));
        pass = pass && error <= tolerance;
    }
    return pass;
}

void append_sample(CaseSamples &samples, const CpuRun &serial,
                   const CpuRun &parallel, const MetalRun &metal,
                   double plan_preparation_ms) {
    const double frame_scale = static_cast<double>(samples.frame_count) * 1000.0;
    samples.effective_frame_workers = parallel.effective_workers;
    samples.cpu_serial_wall_ms.push_back(serial.wall_ms);
    samples.cpu_serial_throughput_fps.push_back(frame_scale / serial.wall_ms);
    samples.cpu_serial_cold_total_ms.push_back(serial.wall_ms + plan_preparation_ms);
    samples.cpu_parallel_wall_ms.push_back(parallel.wall_ms);
    samples.cpu_parallel_throughput_fps.push_back(frame_scale / parallel.wall_ms);
    samples.cpu_parallel_cold_total_ms.push_back(parallel.wall_ms + plan_preparation_ms);
    samples.metal_wall_ms.push_back(metal.wall_ms);
    samples.metal_throughput_fps.push_back(frame_scale / metal.wall_ms);
    samples.metal_cold_total_ms.push_back(metal.wall_ms + plan_preparation_ms);
    samples.metal_buffer_allocation_ms.push_back(metal.telemetry.buffer_allocation_ms);
    samples.metal_working_buffer_allocation_ms.push_back(
        metal.telemetry.working_buffer_allocation_ms);
    samples.metal_plan_buffer_allocation_ms.push_back(metal.plan_buffer_allocation_ms);
    samples.metal_source_upload_ms.push_back(metal.telemetry.source_upload_ms);
    samples.metal_plan_upload_ms.push_back(metal.telemetry.plan_upload_ms);
    samples.metal_buffer_wiring_ms.push_back(metal.telemetry.buffer_wiring_ms);
    samples.metal_pipeline_creation_ms.push_back(metal.pipeline_creation_ms);
    samples.metal_gpu_execution_ms.push_back(metal.telemetry.gpu_execution_ms);
    samples.metal_host_residual_ms.push_back(metal.host_residual_ms);
    samples.metal_plan_upload_bytes.push_back(
        static_cast<double>(metal.telemetry.plan_upload_bytes));
    samples.metal_command_submissions.push_back(
        static_cast<double>(metal.telemetry.command_buffer_submission_count));
    samples.cpu_parallel_speedup.push_back(serial.wall_ms / parallel.wall_ms);
    samples.cpu_serial_to_metal_speedup.push_back(serial.wall_ms / metal.wall_ms);
    samples.cpu_parallel_to_metal_speedup.push_back(parallel.wall_ms / metal.wall_ms);
}

[[nodiscard]] std::array<std::string_view, 3U> arm_order(std::size_t sample_index) {
    switch (sample_index % 3U) {
    case 0U: return {"cpu_serial", "cpu_frame_parallel", "metal_serial_calls"};
    case 1U: return {"cpu_frame_parallel", "metal_serial_calls", "cpu_serial"};
    default: return {"metal_serial_calls", "cpu_serial", "cpu_frame_parallel"};
    }
}

[[nodiscard]] CaseSamples measure_case(
    const Configuration &config,
    const Fixture &fixture,
    const getnative::MetricSpec &metric,
    std::size_t frame_count,
    std::size_t worker_limit,
    getnative::MetalAnalysisEngine &metal) {
    CaseSamples samples;
    samples.frame_count = frame_count;
    const std::size_t warmup_frames = std::min(
        frame_count, std::max<std::size_t>(2U, std::min(worker_limit, fixture.frames.size())));
    (void)run_cpu_serial(config, fixture, metric, warmup_frames);
    (void)run_cpu_parallel(config, fixture, metric, warmup_frames, worker_limit);
    (void)run_metal(config, fixture, metric, warmup_frames, metal);

    std::optional<std::vector<double>> expected_cpu;
    std::optional<std::vector<double>> expected_metal;
    for (std::size_t sample_index = 0; sample_index < config.samples; ++sample_index) {
        CpuRun serial;
        CpuRun parallel;
        MetalRun gpu;
        const auto order = arm_order(sample_index);
        for (const std::string_view arm : order) {
            if (arm == "cpu_serial") {
                serial = run_cpu_serial(config, fixture, metric, frame_count);
            } else if (arm == "cpu_frame_parallel") {
                parallel = run_cpu_parallel(
                    config, fixture, metric, frame_count, worker_limit);
            } else {
                gpu = run_metal(config, fixture, metric, frame_count, metal);
            }
        }
        std::ostringstream order_text;
        order_text << order[0] << ',' << order[1] << ',' << order[2];
        samples.arm_order.push_back(order_text.str());

        const double cpu_parallel_error = maximum_difference(serial.values, parallel.values);
        samples.maximum_cpu_parallel_error = std::max(
            samples.maximum_cpu_parallel_error, cpu_parallel_error);
        samples.correctness_pass = samples.correctness_pass
            && cpu_parallel_error <= repeat_tolerance
            && metal_within_tolerance(serial.values, gpu.values, samples.maximum_metal_error);

        if (!expected_cpu) {
            expected_cpu = serial.values;
            expected_metal = gpu.values;
            samples.cpu_result_identity = result_identity(serial.values);
            samples.metal_result_identity = result_identity(gpu.values);
        } else {
            samples.maximum_cpu_repeat_error = std::max(
                samples.maximum_cpu_repeat_error,
                maximum_difference(*expected_cpu, serial.values));
            samples.maximum_metal_repeat_error = std::max(
                samples.maximum_metal_repeat_error,
                maximum_difference(*expected_metal, gpu.values));
        }
        samples.result_stability_pass = samples.result_stability_pass
            && samples.maximum_cpu_repeat_error <= repeat_tolerance
            && samples.maximum_metal_repeat_error <= repeat_tolerance;
        append_sample(samples, serial, parallel, gpu, fixture.plan_preparation_ms);
    }
    return samples;
}

void run_profile_mode(
    const Configuration &config,
    const Fixture &fixture,
    const getnative::MetricSpec &metric,
    std::size_t worker_limit) {
    const ProfileArm arm = *config.profile_arm;
    const std::size_t frame_count = config.frame_counts.front();
    const std::size_t warmup_frames = std::min(
        frame_count, std::max<std::size_t>(2U, std::min(worker_limit, fixture.frames.size())));
    std::vector<double> wall_samples;
    std::vector<double> gpu_samples;
    std::vector<double> source_upload_samples;
    std::vector<double> plan_upload_samples;
    std::vector<double> plan_allocation_samples;
    std::vector<double> host_residual_samples;
    std::string retained_identity;
    std::size_t effective_workers = 1U;

    const auto retain_result = [&](const std::vector<double> &values) {
        const std::string identity = result_identity(values);
        if (retained_identity.empty()) {
            retained_identity = identity;
        } else if (retained_identity != identity) {
            throw std::runtime_error("profile result identity changed between samples");
        }
    };

    if (arm == ProfileArm::metal) {
        if (!getnative::metal_backend_available()) {
            throw std::runtime_error("no Metal device is available");
        }
        getnative::MetalAnalysisEngine metal({
            config.tile_size,
            config.reduction_groups,
            0U,
            config.profile_split_kernels,
            config.inverse_threads,
        });
        (void)run_metal(config, fixture, metric, warmup_frames, metal);
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            const MetalRun run = run_metal(config, fixture, metric, frame_count, metal);
            retain_result(run.values);
            wall_samples.push_back(run.wall_ms);
            gpu_samples.push_back(run.telemetry.gpu_execution_ms);
            source_upload_samples.push_back(run.telemetry.source_upload_ms);
            plan_upload_samples.push_back(run.telemetry.plan_upload_ms);
            plan_allocation_samples.push_back(run.plan_buffer_allocation_ms);
            host_residual_samples.push_back(run.host_residual_ms);
        }
    } else {
        const auto run_selected_cpu = [&](std::size_t frames) {
            return arm == ProfileArm::cpu_serial
                ? run_cpu_serial(config, fixture, metric, frames)
                : run_cpu_parallel(config, fixture, metric, frames, worker_limit);
        };
        (void)run_selected_cpu(warmup_frames);
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            CpuRun run = run_selected_cpu(frame_count);
            retain_result(run.values);
            effective_workers = run.effective_workers;
            wall_samples.push_back(run.wall_ms);
        }
    }

    const auto wall = getnative::benchmark::summarize(std::move(wall_samples));
    const double throughput = static_cast<double>(frame_count) * 1000.0 / wall.median;
    std::cout << std::fixed << std::setprecision(3)
              << "profile_mode=true\n"
              << "profile_arm=" << profile_arm_name(arm) << '\n'
              << "kernel=" << config.kernel << '\n'
              << "blur=" << config.blur << '\n'
              << "frames=" << frame_count << '\n'
              << "samples=" << config.samples << '\n'
              << "effective_workers=" << effective_workers << '\n'
              << "wall_ms=" << wall.median << '\n'
              << "throughput_fps=" << throughput << '\n'
              << "result_identity_fnv1a64=" << retained_identity << '\n';
    if (arm == ProfileArm::metal) {
        std::cout << "gpu_execution_ms="
                  << getnative::benchmark::summarize(std::move(gpu_samples)).median << '\n'
                  << "source_upload_ms="
                  << getnative::benchmark::summarize(
                         std::move(source_upload_samples)).median << '\n'
                  << "plan_upload_ms="
                  << getnative::benchmark::summarize(
                         std::move(plan_upload_samples)).median << '\n'
                  << "plan_buffer_allocation_ms="
                  << getnative::benchmark::summarize(
                         std::move(plan_allocation_samples)).median << '\n'
                  << "host_residual_ms="
                  << getnative::benchmark::summarize(
                         std::move(host_residual_samples)).median << '\n'
                  << "profile_split_kernels="
                  << (config.profile_split_kernels ? "true" : "false") << '\n';
    }
}

[[nodiscard]] double relative_mad(const getnative::benchmark::Summary &summary) {
    return summary.median == 0.0
        ? (summary.mad == 0.0 ? 0.0 : std::numeric_limits<double>::infinity())
        : summary.mad / std::abs(summary.median);
}

[[nodiscard]] CaseSummary summarize_case(CaseSamples samples) {
    CaseSummary result{
        samples.frame_count,
        samples.effective_frame_workers,
        getnative::benchmark::summarize(std::move(samples.cpu_serial_wall_ms)),
        getnative::benchmark::summarize(std::move(samples.cpu_serial_throughput_fps)),
        getnative::benchmark::summarize(std::move(samples.cpu_serial_cold_total_ms)),
        getnative::benchmark::summarize(std::move(samples.cpu_parallel_wall_ms)),
        getnative::benchmark::summarize(std::move(samples.cpu_parallel_throughput_fps)),
        getnative::benchmark::summarize(std::move(samples.cpu_parallel_cold_total_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_wall_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_throughput_fps)),
        getnative::benchmark::summarize(std::move(samples.metal_cold_total_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_buffer_allocation_ms)),
        getnative::benchmark::summarize(
            std::move(samples.metal_working_buffer_allocation_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_plan_buffer_allocation_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_source_upload_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_plan_upload_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_buffer_wiring_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_pipeline_creation_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_gpu_execution_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_host_residual_ms)),
        getnative::benchmark::summarize(std::move(samples.metal_plan_upload_bytes)),
        getnative::benchmark::summarize(std::move(samples.metal_command_submissions)),
        getnative::benchmark::summarize(std::move(samples.cpu_parallel_speedup)),
        getnative::benchmark::summarize(std::move(samples.cpu_serial_to_metal_speedup)),
        getnative::benchmark::summarize(std::move(samples.cpu_parallel_to_metal_speedup)),
        samples.maximum_cpu_parallel_error,
        samples.maximum_metal_error,
        samples.maximum_cpu_repeat_error,
        samples.maximum_metal_repeat_error,
        samples.correctness_pass,
        samples.result_stability_pass,
        true,
        std::move(samples.cpu_result_identity),
        std::move(samples.metal_result_identity),
        std::move(samples.arm_order),
    };
    result.noise_pass = relative_mad(result.cpu_serial_wall_ms) <= noise_gate
        && relative_mad(result.cpu_parallel_wall_ms) <= noise_gate
        && relative_mad(result.metal_wall_ms) <= noise_gate;
    return result;
}

void append_named_summary(std::ostream &output, std::string_view name,
                          const getnative::benchmark::Summary &summary,
                          bool &first) {
    if (!first) output << ',';
    first = false;
    output << getnative::benchmark::json_string(name) << ':';
    getnative::benchmark::append_summary(output, summary);
}

void append_phase_unavailable(std::ostream &output, std::string_view name,
                              std::string_view reason, bool &first) {
    if (!first) output << ',';
    first = false;
    output << getnative::benchmark::json_string(name)
           << ":{\"available\":false,\"reason\":"
           << getnative::benchmark::json_string(reason) << '}';
}

void append_case_json(std::ostream &output, const CaseSummary &summary) {
    output << "{\"frames\":" << summary.frame_count
           << ",\"effective_frame_workers\":" << summary.effective_frame_workers
           << ",\"arm_order\":[";
    for (std::size_t index = 0; index < summary.arm_order.size(); ++index) {
        if (index != 0U) output << ',';
        output << getnative::benchmark::json_string(summary.arm_order[index]);
    }
    output << "],\"cpu_serial\":{\"implementation\":"
           << getnative::benchmark::json_string(
                  "current analyze_batch_f32 with one candidate per frame")
           << ',';
    bool first = true;
    append_named_summary(output, "execution_total_wall_ms", summary.cpu_serial_wall_ms, first);
    append_named_summary(output, "cold_total_wall_with_plan_ms",
                         summary.cpu_serial_cold_total_ms, first);
    append_named_summary(output, "throughput_fps", summary.cpu_serial_throughput_fps, first);
    output << "},\"cpu_frame_parallel\":{\"implementation\":"
           << getnative::benchmark::json_string(
                  "bounded atomic frame cursor with one reusable workspace per worker")
           << ',';
    first = true;
    append_named_summary(output, "execution_total_wall_ms", summary.cpu_parallel_wall_ms, first);
    append_named_summary(output, "cold_total_wall_with_plan_ms",
                         summary.cpu_parallel_cold_total_ms, first);
    append_named_summary(output, "throughput_fps", summary.cpu_parallel_throughput_fps, first);
    append_named_summary(output, "speedup_vs_cpu_serial", summary.cpu_parallel_speedup, first);
    output << "},\"metal_serial_calls\":{";
    first = true;
    append_named_summary(output, "execution_total_wall_ms", summary.metal_wall_ms, first);
    append_named_summary(output, "cold_total_wall_with_plan_ms",
                         summary.metal_cold_total_ms, first);
    append_named_summary(output, "throughput_fps", summary.metal_throughput_fps, first);
    append_named_summary(output, "speedup_vs_cpu_serial",
                         summary.cpu_serial_to_metal_speedup, first);
    append_named_summary(output, "speedup_vs_cpu_frame_parallel",
                         summary.cpu_parallel_to_metal_speedup, first);
    output << ",\"phases\":{";
    first = true;
    append_phase_unavailable(
        output, "decode_ms", "fixture contains predecoded float frames", first);
    append_phase_unavailable(
        output, "color_conversion_ms", "fixture is already single-plane float", first);
    append_named_summary(output, "buffer_allocation_ms",
                         summary.metal_buffer_allocation_ms, first);
    append_named_summary(output, "working_buffer_allocation_ms",
                         summary.metal_working_buffer_allocation_ms, first);
    append_named_summary(output, "plan_buffer_allocation_ms",
                         summary.metal_plan_buffer_allocation_ms, first);
    append_named_summary(output, "source_upload_ms", summary.metal_source_upload_ms, first);
    append_named_summary(output, "plan_upload_ms", summary.metal_plan_upload_ms, first);
    append_named_summary(output, "buffer_wiring_ms", summary.metal_buffer_wiring_ms, first);
    append_named_summary(output, "pipeline_creation_ms",
                         summary.metal_pipeline_creation_ms, first);
    append_named_summary(output, "gpu_execution_ms", summary.metal_gpu_execution_ms, first);
    append_named_summary(output, "host_residual_ms", summary.metal_host_residual_ms, first);
    append_phase_unavailable(
        output, "readback_ms",
        "backend exposes shared-buffer completion and merge only inside host residual", first);
    append_phase_unavailable(
        output, "cpu_merge_ms",
        "backend reduction merge is not independently instrumented", first);
    append_phase_unavailable(
        output, "plan_pack_ms",
        "backend plan packing is included in host residual", first);
    output << "},\"telemetry\":{";
    first = true;
    append_named_summary(output, "plan_upload_bytes", summary.metal_plan_upload_bytes, first);
    append_named_summary(output, "command_submissions",
                         summary.metal_command_submissions, first);
    output << "}},\"correctness\":{\"pass\":"
           << (summary.correctness_pass ? "true" : "false")
           << ",\"stable_across_samples\":"
           << (summary.result_stability_pass ? "true" : "false")
           << ",\"maximum_cpu_parallel_error\":" << std::setprecision(17)
           << summary.maximum_cpu_parallel_error
           << ",\"maximum_metal_error\":" << summary.maximum_metal_error
           << ",\"maximum_cpu_repeat_error\":" << summary.maximum_cpu_repeat_error
           << ",\"maximum_metal_repeat_error\":" << summary.maximum_metal_repeat_error
           << ",\"cpu_result_identity_fnv1a64\":"
           << getnative::benchmark::json_string(summary.cpu_result_identity)
           << ",\"metal_result_identity_fnv1a64\":"
           << getnative::benchmark::json_string(summary.metal_result_identity)
           << "},\"noise\":{\"relative_mad_limit\":" << noise_gate
           << ",\"cpu_serial_relative_mad\":" << relative_mad(summary.cpu_serial_wall_ms)
           << ",\"cpu_parallel_relative_mad\":"
           << relative_mad(summary.cpu_parallel_wall_ms)
           << ",\"metal_relative_mad\":" << relative_mad(summary.metal_wall_ms)
           << ",\"pass\":" << (summary.noise_pass ? "true" : "false") << "}}";
}

[[nodiscard]] std::string make_decision(
    bool condition, std::string_view adopt, std::string_view reject) {
    return std::string{condition ? adopt : reject};
}

[[nodiscard]] std::string make_json(
    const Configuration &config,
    const Fixture &fixture,
    const getnative::MetalAnalysisEngine &metal,
    const std::vector<CaseSummary> &summaries,
    std::size_t worker_limit,
    bool all_gates_pass,
    int argc,
    char **argv) {
    const CaseSummary &decision_case = summaries.back();
    const double known_plan_residency_ms =
        decision_case.metal_plan_upload_ms.median
        + decision_case.metal_plan_buffer_allocation_ms.median;
    const double known_plan_residency_share =
        known_plan_residency_ms / decision_case.metal_wall_ms.median;
    const double metal_host_ms = std::max(
        0.0, decision_case.metal_wall_ms.median
            - decision_case.metal_gpu_execution_ms.median);
    const double overlap_floor_ms = std::max(
        metal_host_ms, decision_case.metal_gpu_execution_ms.median);
    const double overlap_upper_bound_speedup = overlap_floor_ms == 0.0
        ? 1.0 : decision_case.metal_wall_ms.median / overlap_floor_ms;
    const bool decision_valid = decision_case.correctness_pass
        && decision_case.result_stability_pass && decision_case.noise_pass;
    const std::string cpu_decision = make_decision(
        decision_valid && decision_case.cpu_parallel_speedup.median >= 1.20,
        "ADOPT_FRAME_LEVEL_PARALLELISM",
        "REJECT_FRAME_LEVEL_PARALLELISM_AS_PRIMARY_OPTIMIZATION");
    const std::string residency_decision = make_decision(
        decision_valid && known_plan_residency_share >= 0.05,
        "PROCEED_PREPARED_PLAN_RESIDENCY_EXPERIMENT",
        "REJECT_PREPARED_PLAN_RESIDENCY_AS_PRIMARY_OPTIMIZATION");
    const std::string inflight_decision = make_decision(
        decision_valid && overlap_upper_bound_speedup >= 1.10,
        "PROCEED_BOUNDED_METAL_IN_FLIGHT_EXPERIMENT",
        "REJECT_BOUNDED_METAL_IN_FLIGHT_AS_PRIMARY_OPTIMIZATION");

    std::ostringstream output;
    output << '{';
    getnative::benchmark::append_common_metadata(
        output, "getnative_fixed_recipe_benchmark", "prepared-once",
        "deterministic-predecoded-frame-ring-v1", argc, argv);
    output << ",\"device\":"
           << getnative::benchmark::json_string(metal.device_info().name)
           << ",\"sample_count\":" << config.samples
           << ",\"fixture\":{\"width\":" << config.width
           << ",\"height\":" << config.height
           << ",\"native_height\":" << config.native_height
           << ",\"kernel\":" << getnative::benchmark::json_string(config.kernel)
           << ",\"blur\":" << config.blur
           << ",\"axes\":\"vertical\",\"candidate_count\":1"
           << ",\"predecoded_ring_size\":" << config.ring_size
           << ",\"logical_frame_counts\":[";
    for (std::size_t index = 0; index < config.frame_counts.size(); ++index) {
        if (index != 0U) output << ',';
        output << config.frame_counts[index];
    }
    output << "]},\"fixed_recipe\":{\"candidate_id\":\"locked-recipe\""
           << ",\"plan_prepared_once\":true"
           << ",\"plan_preparation_ms\":";
    getnative::benchmark::append_summary(
        output, getnative::benchmark::summarize({fixture.plan_preparation_ms}));
    output << ",\"axis_plan_storage_bytes\":"
           << getnative::axis_plan_storage_bytes(*fixture.plan)
           << ",\"axis_plan_identity_fnv1a64\":"
           << getnative::benchmark::json_string(fixture.plan_identity)
           << ",\"immutable_pointer_stable\":true}"
           << ",\"configuration\":{\"frame_worker_limit\":" << worker_limit
           << ",\"tile_size\":" << config.tile_size
           << ",\"reduction_groups\":" << config.reduction_groups
           << ",\"inverse_threads\":" << config.inverse_threads << "}"
           << ",\"cases\":[";
    for (std::size_t index = 0; index < summaries.size(); ++index) {
        if (index != 0U) output << ',';
        append_case_json(output, summaries[index]);
    }
    output << "],\"decisions\":{\"basis_frames\":" << decision_case.frame_count
           << ",\"measurement_valid\":" << (decision_valid ? "true" : "false")
           << ",\"cpu_frame_parallelism\":{\"decision\":"
           << getnative::benchmark::json_string(cpu_decision)
           << ",\"measured_speedup\":" << decision_case.cpu_parallel_speedup.median
           << "},\"prepared_plan_residency\":{\"decision\":"
           << getnative::benchmark::json_string(residency_decision)
           << ",\"known_removable_ms\":" << known_plan_residency_ms
           << ",\"known_removable_wall_share\":" << known_plan_residency_share
           << ",\"scope\":\"plan upload plus plan-buffer allocation; packing remains residual\"}"
           << ",\"bounded_metal_in_flight\":{\"decision\":"
           << getnative::benchmark::json_string(inflight_decision)
           << ",\"measured_gpu_ms\":" << decision_case.metal_gpu_execution_ms.median
           << ",\"measured_non_gpu_wall_ms\":" << metal_host_ms
           << ",\"ideal_overlap_upper_bound_speedup\":"
           << overlap_upper_bound_speedup
           << ",\"scope\":\"upper bound only; requires a measured bounded-ring prototype\"}}"
           << ",\"all_gates_pass\":" << (all_gates_pass ? "true" : "false")
           << "}\n";
    return output.str();
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Configuration config = parse_arguments(argc, argv);
        if (config.json_output) {
            getnative::benchmark::validate_json_output_path(*config.json_output);
        }
        if (!getnative::metal_backend_available()) {
            throw std::runtime_error("no Metal device is available");
        }
        const std::size_t detected_workers = std::max<std::size_t>(
            1U, std::thread::hardware_concurrency());
        const std::size_t worker_limit = config.frame_workers == 0U
            ? detected_workers : config.frame_workers;
        auto filter = benchmark_filter(config.kernel);
        filter.blur = config.blur;
        const Fixture fixture = make_fixture(config, filter);
        const getnative::MetricSpec metric{5, 5, 5, 5, 0.015F, 1U};
        if (config.profile_arm) {
            run_profile_mode(config, fixture, metric, worker_limit);
            return EXIT_SUCCESS;
        }
        getnative::MetalAnalysisEngine metal({
            config.tile_size,
            config.reduction_groups,
            0U,
            config.profile_split_kernels,
            config.inverse_threads,
        });

        std::vector<CaseSummary> summaries;
        summaries.reserve(config.frame_counts.size());
        bool all_gates_pass = true;
        for (const std::size_t frame_count : config.frame_counts) {
            auto summary = summarize_case(measure_case(
                config, fixture, metric, frame_count, worker_limit, metal));
            all_gates_pass = all_gates_pass
                && summary.correctness_pass
                && summary.result_stability_pass;
            if (frame_count >= 100U) all_gates_pass = all_gates_pass && summary.noise_pass;
            std::cout << std::fixed << std::setprecision(3)
                      << "kernel=" << config.kernel
                      << " blur=" << config.blur
                      << " frames=" << frame_count
                      << " cpu_serial_ms=" << summary.cpu_serial_wall_ms.median
                      << " cpu_parallel_ms=" << summary.cpu_parallel_wall_ms.median
                      << " cpu_parallel_speedup=" << summary.cpu_parallel_speedup.median
                      << " metal_ms=" << summary.metal_wall_ms.median
                      << " metal_gpu_ms=" << summary.metal_gpu_execution_ms.median
                      << " metal_host_residual_ms=" << summary.metal_host_residual_ms.median
                      << " correctness=" << (summary.correctness_pass ? "pass" : "fail")
                      << " noise=" << (summary.noise_pass ? "pass" : "fail") << '\n';
            summaries.push_back(std::move(summary));
        }

        const std::string json = make_json(
            config, fixture, metal, summaries, worker_limit,
            all_gates_pass, argc, argv);
        if (config.json_output) {
            getnative::benchmark::atomic_write_json(*config.json_output, json);
        }
        if (config.assert_gates && !all_gates_pass) {
            std::cerr << "fixed-recipe benchmark correctness, stability, or noise gate failed\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "fixed-recipe benchmark failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
