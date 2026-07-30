#include "axis_planner.hpp"

#include "axis_plan_key.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
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
            build_axis_plan_with_tap_evaluation(
                unique_requests[index], options.tap_evaluation));
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
