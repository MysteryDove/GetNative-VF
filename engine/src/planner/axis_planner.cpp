#include "axis_planner.hpp"

#include "axis_plan_key.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace getnative::detail {
namespace {

struct ActiveBuildGuard {
    explicit ActiveBuildGuard(std::atomic_size_t &active) noexcept : active(active) {}
    ~ActiveBuildGuard() { active.fetch_sub(1U, std::memory_order_relaxed); }

    ActiveBuildGuard(const ActiveBuildGuard &) = delete;
    ActiveBuildGuard &operator=(const ActiveBuildGuard &) = delete;

    std::atomic_size_t &active;
};

void update_peak(std::atomic_size_t &peak, std::size_t active) noexcept {
    std::size_t observed = peak.load(std::memory_order_relaxed);
    while (observed < active
           && !peak.compare_exchange_weak(
               observed, active, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

[[nodiscard]] std::size_t effective_workers(
    std::size_t unique_count,
    std::size_t requested_workers) noexcept {
    if (unique_count == 0U) return 0U;
    const std::size_t automatic_cap = std::max<std::size_t>(
        1U, std::min<std::size_t>(std::thread::hardware_concurrency(), 8U));
    const std::size_t bounded = requested_workers == 0U
        ? std::min(unique_count, automatic_cap)
        : std::clamp(requested_workers, std::size_t{1U}, unique_count);
    return unique_count <= 2U ? 1U : bounded;
}

struct BicubicGeometryKey {
    std::int32_t source_size;
    std::int32_t destination_size;
    std::uint64_t active_length;
    std::uint64_t shift;
    BorderMode border;

    friend bool operator==(const BicubicGeometryKey &, const BicubicGeometryKey &) = default;
};

struct BicubicGeometryKeyHash {
    [[nodiscard]] std::size_t operator()(const BicubicGeometryKey &key) const noexcept {
        std::size_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::uint64_t value) {
            hash ^= static_cast<std::size_t>(value);
            hash *= 1099511628211ULL;
        };
        mix(static_cast<std::uint32_t>(key.source_size));
        mix(static_cast<std::uint32_t>(key.destination_size));
        mix(key.active_length);
        mix(key.shift);
        mix(static_cast<std::uint8_t>(key.border));
        return hash;
    }
};

[[nodiscard]] bool can_share_bicubic_geometry(
    const AxisPlanRequest &request) noexcept {
    return request.filter.type == KernelType::bicubic
        && !(request.filter.b == 0.0 && request.filter.c == 0.0)
        && std::isfinite(request.filter.b) && std::isfinite(request.filter.c)
        && request.source_size > 0 && request.destination_size > 0
        && request.active_length > 0.0 && std::isfinite(request.active_length)
        && std::isfinite(request.shift);
}

[[nodiscard]] BicubicGeometryKey bicubic_geometry_key(
    const AxisPlanRequest &request) noexcept {
    return {
        request.source_size,
        request.destination_size,
        std::bit_cast<std::uint64_t>(request.active_length),
        std::bit_cast<std::uint64_t>(request.shift),
        request.border,
    };
}

template <typename Function>
void run_parallel_tasks(
    std::size_t task_count,
    std::size_t worker_limit,
    Function function) {
    if (task_count == 0U) return;
    const std::size_t worker_count = std::min(task_count, worker_limit);
    if (worker_count <= 1U) {
        for (std::size_t index = 0; index < task_count; ++index) function(index);
        return;
    }

    std::atomic_size_t cursor{0U};
    std::mutex failure_mutex;
    std::optional<std::size_t> lowest_failure_index;
    std::exception_ptr failure;
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (true) {
                const std::size_t index = cursor.fetch_add(1U, std::memory_order_relaxed);
                if (index >= task_count) break;
                try {
                    function(index);
                } catch (...) {
                    cursor.exchange(task_count, std::memory_order_relaxed);
                    const std::scoped_lock lock(failure_mutex);
                    if (!lowest_failure_index || index < *lowest_failure_index) {
                        lowest_failure_index = index;
                        failure = std::current_exception();
                    }
                    break;
                }
            }
        });
    }
    workers.clear();
    if (failure) std::rethrow_exception(failure);
}

} // namespace

AxisPlanBatchResult build_axis_plans(
    std::span<const AxisPlanRequest> requests,
    AxisPlanBatchOptions options) {
    AxisPlanBatchResult result;
    if (requests.empty()) return result;

    std::vector<AxisPlanRequest> unique_requests;
    unique_requests.reserve(requests.size());
    std::vector<std::size_t> request_to_unique;
    request_to_unique.reserve(requests.size());
    std::unordered_map<PlanKey, std::size_t, PlanKeyHash> unique_indices;
    unique_indices.reserve(requests.size());
    for (const AxisPlanRequest &request : requests) {
        const auto [position, inserted] = unique_indices.emplace(
            plan_key(request), unique_requests.size());
        if (inserted) unique_requests.push_back(request);
        request_to_unique.push_back(position->second);
    }

    result.unique_key_count = unique_requests.size();
    result.effective_worker_count = effective_workers(
        unique_requests.size(), options.worker_count);
    std::vector<std::shared_ptr<const AxisPlanGeometry>> geometries(
        unique_requests.size());
    if (options.bicubic_geometry == BicubicGeometryMode::reuse) {
        std::optional<std::pair<std::uint64_t, std::uint64_t>> first_parameters;
        bool has_parameter_sweep = false;
        for (const AxisPlanRequest &request : unique_requests) {
            if (!can_share_bicubic_geometry(request)) continue;
            const std::pair parameters{
                std::bit_cast<std::uint64_t>(request.filter.b),
                std::bit_cast<std::uint64_t>(request.filter.c),
            };
            if (!first_parameters) {
                first_parameters = parameters;
            } else if (*first_parameters != parameters) {
                has_parameter_sweep = true;
                break;
            }
        }

        std::unordered_map<BicubicGeometryKey, std::vector<std::size_t>,
                           BicubicGeometryKeyHash> grouped_indices;
        if (has_parameter_sweep) {
            grouped_indices.reserve(unique_requests.size());
            for (std::size_t index = 0; index < unique_requests.size(); ++index) {
                const AxisPlanRequest &request = unique_requests[index];
                if (!can_share_bicubic_geometry(request)) continue;
                grouped_indices[bicubic_geometry_key(request)].push_back(index);
            }
        }

        std::vector<std::vector<std::size_t>> families;
        families.reserve(grouped_indices.size());
        for (auto &[key, indices] : grouped_indices) {
            (void)key;
            if (indices.size() < 2U) continue;
            result.bicubic_geometry_plan_count += indices.size();
            families.push_back(std::move(indices));
        }
        result.bicubic_geometry_family_count = families.size();
        result.bicubic_geometry_build_count = families.size();
        std::vector<std::shared_ptr<const AxisPlanGeometry>> family_geometries(
            families.size());
        run_parallel_tasks(
            families.size(), result.effective_worker_count,
            [&](std::size_t family_index) {
                family_geometries[family_index] = build_axis_plan_geometry(
                    unique_requests[families[family_index].front()]);
            });
        for (std::size_t family_index = 0; family_index < families.size(); ++family_index) {
            const auto &geometry = family_geometries[family_index];
            const std::size_t bytes = axis_plan_geometry_bytes(*geometry);
            if (bytes > std::numeric_limits<std::size_t>::max()
                            - result.bicubic_geometry_scratch_bytes) {
                throw std::length_error("bicubic geometry scratch accounting overflow");
            }
            result.bicubic_geometry_scratch_bytes += bytes;
            for (const std::size_t plan_index : families[family_index]) {
                geometries[plan_index] = geometry;
            }
        }
    }
    std::vector<std::shared_ptr<const AxisPlan>> unique_plans(unique_requests.size());
    std::atomic_size_t physical_builds{0U};
    std::atomic_size_t active_builds{0U};
    std::atomic_size_t peak_active_builds{0U};

    const auto build_one = [&](std::size_t index) {
        const std::size_t active = active_builds.fetch_add(
            1U, std::memory_order_relaxed) + 1U;
        ActiveBuildGuard active_guard{active_builds};
        update_peak(peak_active_builds, active);
        if (options.before_build) options.before_build(index);
        physical_builds.fetch_add(1U, std::memory_order_relaxed);
        unique_plans[index] = std::make_shared<const AxisPlan>(
            build_axis_plan_with_geometry(
                unique_requests[index], options.tap_evaluation,
                geometries[index].get()));
    };

    if (result.effective_worker_count == 1U) {
        for (std::size_t index = 0; index < unique_requests.size(); ++index) {
            build_one(index);
        }
    } else {
        std::atomic_size_t cursor{0U};
        std::mutex failure_mutex;
        std::optional<std::size_t> lowest_failure_index;
        std::exception_ptr failure;
        std::vector<std::jthread> workers;
        workers.reserve(result.effective_worker_count);
        for (std::size_t worker = 0; worker < result.effective_worker_count; ++worker) {
            workers.emplace_back([&] {
                while (true) {
                    const std::size_t index = cursor.fetch_add(1U, std::memory_order_relaxed);
                    if (index >= unique_requests.size()) break;
                    try {
                        build_one(index);
                    } catch (...) {
                        cursor.exchange(unique_requests.size(), std::memory_order_relaxed);
                        {
                            const std::scoped_lock lock(failure_mutex);
                            if (!lowest_failure_index || index < *lowest_failure_index) {
                                lowest_failure_index = index;
                                failure = std::current_exception();
                            }
                        }
                        if (options.failure_observed) options.failure_observed(index);
                        break;
                    }
                }
            });
        }
        workers.clear();
        if (failure) std::rethrow_exception(failure);
    }

    result.physical_build_count = physical_builds.load(std::memory_order_relaxed);
    result.peak_active_builds = peak_active_builds.load(std::memory_order_relaxed);
    result.plans.reserve(requests.size());
    for (const std::size_t unique_index : request_to_unique) {
        result.plans.push_back(unique_plans[unique_index]);
    }
    return result;
}

} // namespace getnative::detail
