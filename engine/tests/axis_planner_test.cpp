#include "axis_planner.hpp"
#include "getnative/cpu_analysis.hpp"
#include "getnative/joining_thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <typename T>
void expect_vector_bytes_equal(
    const std::vector<T> &actual,
    const std::vector<T> &expected,
    std::string_view name) {
    expect(actual.size() == expected.size(), std::string{name} + " size differs");
    if (!actual.empty()) {
        expect(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(T)) == 0,
               std::string{name} + " bytes differ");
    }
}

void expect_plan_equal(
    const getnative::AxisPlan &actual,
    const getnative::AxisPlan &expected) {
    expect(actual.source_size == expected.source_size, "source size differs");
    expect(actual.destination_size == expected.destination_size, "destination size differs");
    expect(actual.support == expected.support, "support differs");
    expect(actual.half_bandwidth == expected.half_bandwidth, "half bandwidth differs");
    expect(actual.forward_width == expected.forward_width, "forward width differs");
    expect(std::bit_cast<std::uint64_t>(actual.active_length)
               == std::bit_cast<std::uint64_t>(expected.active_length),
           "active length bits differ");
    expect(std::bit_cast<std::uint64_t>(actual.shift)
               == std::bit_cast<std::uint64_t>(expected.shift),
           "shift bits differ");
    expect_vector_bytes_equal(actual.forward_offsets, expected.forward_offsets,
                              "forward offsets");
    expect_vector_bytes_equal(actual.forward_indices, expected.forward_indices,
                              "forward indices");
    expect_vector_bytes_equal(actual.forward_weights, expected.forward_weights,
                              "forward weights");
    expect_vector_bytes_equal(actual.transpose_offsets, expected.transpose_offsets,
                              "transpose offsets");
    expect_vector_bytes_equal(actual.transpose_indices, expected.transpose_indices,
                              "transpose indices");
    expect_vector_bytes_equal(actual.transpose_weights, expected.transpose_weights,
                              "transpose weights");
    expect_vector_bytes_equal(actual.lower_ld, expected.lower_ld, "lower LDLT");
    expect_vector_bytes_equal(actual.upper_l, expected.upper_l, "upper L");
    expect_vector_bytes_equal(actual.inverse_diagonal, expected.inverse_diagonal,
                              "inverse diagonal");
}

#if GETNATIVE_PLANNER_FP_MODE_VALUE == 1
// Fast-math builds reassociate the reuse and recompute tap-evaluation paths
// differently: measured deltas are <= 6e-8 absolute (1-2 float ulps) on GCC
// 15.2 -O3. Assert structural equality with a bound far above that noise and
// far below the 2e-6 upstream-conformance budget; integer data and scalars
// stay bit-exact. Strict-FP builds keep the exact expectation above.
void expect_plan_equal_within_fp_noise(
    const getnative::AxisPlan &actual, const getnative::AxisPlan &expected) {
    expect(actual.source_size == expected.source_size, "source size differs");
    expect(actual.destination_size == expected.destination_size, "destination size differs");
    expect(actual.support == expected.support, "support differs");
    expect(actual.half_bandwidth == expected.half_bandwidth, "half bandwidth differs");
    expect(actual.forward_width == expected.forward_width, "forward width differs");
    expect_vector_bytes_equal(actual.forward_offsets, expected.forward_offsets,
                              "forward offsets");
    expect_vector_bytes_equal(actual.forward_indices, expected.forward_indices,
                              "forward indices");
    expect_vector_bytes_equal(actual.transpose_offsets, expected.transpose_offsets,
                              "transpose offsets");
    expect_vector_bytes_equal(actual.transpose_indices, expected.transpose_indices,
                              "transpose indices");
    const auto expect_weights_close = [](const std::vector<float> &lhs,
                                         const std::vector<float> &rhs,
                                         std::string_view name) {
        expect(lhs.size() == rhs.size(), std::string{name} + " size differs");
        for (std::size_t index = 0; index < lhs.size(); ++index) {
            if (std::abs(static_cast<double>(lhs[index])
                         - static_cast<double>(rhs[index])) > 1e-6) {
                throw std::runtime_error(
                    std::string{name} + " differs beyond fp noise at "
                    + std::to_string(index));
            }
        }
    };
    expect_weights_close(actual.forward_weights, expected.forward_weights,
                         "forward weights");
    expect_weights_close(actual.transpose_weights, expected.transpose_weights,
                         "transpose weights");
    expect_weights_close(actual.lower_ld, expected.lower_ld, "lower LDLT");
    expect_weights_close(actual.upper_l, expected.upper_l, "upper L");
    expect_weights_close(actual.inverse_diagonal, expected.inverse_diagonal,
                         "inverse diagonal");
}
#endif

[[nodiscard]] std::vector<getnative::AxisPlanRequest> fixture_requests() {
    return {
        {37, 23, 23.4, -0.375, getnative::Filter::bilinear(),
         getnative::BorderMode::zero},
        {64, 43, 43.25, -0.125, getnative::Filter::bicubic(),
         getnative::BorderMode::mirror},
        {71, 47, 47.5, 0.25, getnative::Filter::bicubic(0.2, 0.4),
         getnative::BorderMode::repeat},
        {48, 31, 31.125, -0.25, getnative::Filter::spline16(),
         getnative::BorderMode::mirror},
        {83, 55, 55.75, 0.375, getnative::Filter::spline36(),
         getnative::BorderMode::zero},
        {96, 63, 63.5, -0.5, getnative::Filter::spline64(),
         getnative::BorderMode::repeat},
        {57, 38, 38.25, 0.125, getnative::Filter::lanczos(3),
         getnative::BorderMode::mirror},
        {128, 85, 85.5, -0.25, getnative::Filter::lanczos(8),
         getnative::BorderMode::mirror},
    };
}

// Cross-mode comparisons (reuse vs recompute, family-reuse vs independent)
// differ by fast-math reassociation noise; same-mode serial/batch
// comparisons stay bit-exact and keep using expect_plan_equal.
void expect_plan_equal_cross_mode(
    const getnative::AxisPlan &actual, const getnative::AxisPlan &expected) {
#if GETNATIVE_PLANNER_FP_MODE_VALUE == 1
    expect_plan_equal_within_fp_noise(actual, expected);
#else
    expect_plan_equal(actual, expected);
#endif
}

void test_empty_and_stable_deduplication() {
    const auto empty = getnative::detail::build_axis_plans({});
    expect(empty.plans.empty(), "empty input returns no plans");
    expect(empty.unique_key_count == 0U && empty.physical_build_count == 0U
               && empty.peak_active_builds == 0U && empty.effective_worker_count == 0U,
           "empty input reports zero counts");

    const auto fixtures = fixture_requests();
    std::vector<getnative::AxisPlanRequest> identical(1000U, fixtures.front());
    const auto deduplicated = getnative::detail::build_axis_plans(identical, {8U, {}, {}});
    expect(deduplicated.plans.size() == identical.size(),
           "identical requests preserve output count");
    expect(deduplicated.unique_key_count == 1U
               && deduplicated.physical_build_count == 1U
               && deduplicated.effective_worker_count == 1U,
           "identical requests build once on the serial fast path");
    for (const auto &plan : deduplicated.plans) {
        expect(plan.get() == deduplicated.plans.front().get(),
               "identical requests share one plan pointer");
    }

    std::vector<getnative::AxisPlanRequest> alternating;
    for (std::size_t index = 0; index < 100U; ++index) {
        alternating.push_back(fixtures[index % 2U]);
    }
    const auto two_keys = getnative::detail::build_axis_plans(alternating, {8U, {}, {}});
    expect(two_keys.unique_key_count == 2U && two_keys.physical_build_count == 2U
               && two_keys.effective_worker_count == 1U,
           "two keys use the stable serial fast path");
    for (std::size_t index = 0; index < alternating.size(); ++index) {
        expect(two_keys.plans[index].get() == two_keys.plans[index % 2U].get(),
               "alternating keys preserve request order and pointer sharing");
    }
}

void test_exact_plans_for_all_worker_modes() {
    const auto requests = fixture_requests();
    std::vector<getnative::AxisPlan> serial;
    serial.reserve(requests.size());
    for (const auto &request : requests) serial.push_back(getnative::build_axis_plan(request));

    for (const std::size_t workers : {0U, 1U, 2U, 4U, 8U}) {
        const auto batch = getnative::detail::build_axis_plans(requests, {workers, {}, {}});
        expect(batch.plans.size() == requests.size(), "batch plan count differs");
        expect(batch.unique_key_count == requests.size()
                   && batch.physical_build_count == requests.size(),
               "every unique fixture builds exactly once");
        for (std::size_t index = 0; index < requests.size(); ++index) {
            expect_plan_equal(*batch.plans[index], serial[index]);
        }
    }
}

void test_tap_reuse_is_byte_identical() {
    std::vector<getnative::AxisPlanRequest> requests = fixture_requests();
    const std::array bicubic_parameters{
        std::pair{0.0, 0.0},
        std::pair{0.0, 0.5},
        std::pair{1.0 / 3.0, 1.0 / 3.0},
        std::pair{1.0, 0.0},
        std::pair{-0.25, 0.75},
    };
    for (const auto [b, c] : bicubic_parameters) {
        requests.push_back({81, 53, 53.625, -0.375,
                            getnative::Filter::bicubic(b, c),
                            getnative::BorderMode::mirror});
    }
    for (std::int32_t taps = 1; taps <= 8; ++taps) {
        for (const auto border : {getnative::BorderMode::zero,
                                  getnative::BorderMode::repeat,
                                  getnative::BorderMode::mirror}) {
            requests.push_back({73, 49, 49.375, 0.3125,
                                getnative::Filter::lanczos(taps), border});
        }
    }

    for (const auto &request : requests) {
        const auto recomputed = getnative::detail::build_axis_plan_with_tap_evaluation(
            request, getnative::detail::TapEvaluationMode::recompute);
        const auto reused = getnative::detail::build_axis_plan_with_tap_evaluation(
            request, getnative::detail::TapEvaluationMode::reuse);
        expect_plan_equal_cross_mode(reused, recomputed);
        expect_plan_equal_cross_mode(getnative::build_axis_plan(request), reused);
    }

    getnative::detail::AxisPlanBatchOptions recompute_options;
    recompute_options.worker_count = 4U;
    recompute_options.tap_evaluation = getnative::detail::TapEvaluationMode::recompute;
    const auto recomputed_batch = getnative::detail::build_axis_plans(
        requests, std::move(recompute_options));

    getnative::detail::AxisPlanBatchOptions reuse_options;
    reuse_options.worker_count = 4U;
    reuse_options.tap_evaluation = getnative::detail::TapEvaluationMode::reuse;
    const auto reused_batch = getnative::detail::build_axis_plans(
        requests, std::move(reuse_options));
    expect(recomputed_batch.plans.size() == reused_batch.plans.size(),
           "tap evaluation batches differ in size");
    for (std::size_t index = 0; index < reused_batch.plans.size(); ++index) {
        expect_plan_equal_cross_mode(*reused_batch.plans[index], *recomputed_batch.plans[index]);
    }
}

void test_bicubic_geometry_reuse_is_byte_identical() {
    const std::array bicubic_parameters{
        std::pair{1.0 / 3.0, 1.0 / 3.0},
        std::pair{0.0, 0.5},
        std::pair{1.0, 0.0},
        std::pair{0.0, 0.0},
        std::pair{-0.25, 0.75},
        std::pair{0.5, 0.25},
    };
    std::vector<getnative::AxisPlanRequest> requests;
    for (const auto border : {getnative::BorderMode::zero,
                              getnative::BorderMode::repeat,
                              getnative::BorderMode::mirror}) {
        const std::size_t family_begin = requests.size();
        for (const auto [b, c] : bicubic_parameters) {
            requests.push_back({81, 53, 53.625, -0.375,
                                getnative::Filter::bicubic(b, c), border});
        }
        requests.push_back(requests[family_begin + 1U]);
    }

    getnative::detail::AxisPlanBatchOptions independent_options;
    independent_options.worker_count = 4U;
    independent_options.bicubic_geometry =
        getnative::detail::BicubicGeometryMode::independent;
    const auto independent = getnative::detail::build_axis_plans(
        requests, std::move(independent_options));

    getnative::detail::AxisPlanBatchOptions reuse_options;
    reuse_options.worker_count = 4U;
    reuse_options.bicubic_geometry = getnative::detail::BicubicGeometryMode::reuse;
    const auto reused = getnative::detail::build_axis_plans(
        requests, std::move(reuse_options));

    expect(independent.plans.size() == reused.plans.size(),
           "bicubic geometry batches differ in size");
    for (std::size_t index = 0; index < reused.plans.size(); ++index) {
        expect_plan_equal_cross_mode(*reused.plans[index], *independent.plans[index]);
    }
    expect(independent.bicubic_geometry_family_count == 0U
               && independent.bicubic_geometry_plan_count == 0U
               && independent.bicubic_geometry_build_count == 0U
               && independent.bicubic_geometry_scratch_bytes == 0U,
           "independent bicubic mode reports no shared geometry");
    expect(reused.unique_key_count == 18U
               && reused.bicubic_geometry_family_count == 3U
               && reused.bicubic_geometry_plan_count == 15U
               && reused.bicubic_geometry_build_count == 3U
               && reused.bicubic_geometry_scratch_bytes > 0U,
           "bicubic geometry telemetry matches exact nonzero B/C families");

    for (std::size_t family = 0; family < 3U; ++family) {
        const std::size_t base = family * 7U;
        expect(reused.plans[base + 6U].get() == reused.plans[base + 1U].get(),
               "exact duplicate B/C requests retain Stage 1 pointer deduplication");
        const auto &catrom = *reused.plans[base + 1U];
        const auto &zero = *reused.plans[base + 3U];
        expect(catrom.transpose_offsets != zero.transpose_offsets
                   || catrom.transpose_indices != zero.transpose_indices,
               "B=0,C=0 retains an independent sparse topology");
    }
}

void test_exact_bit_key_distinctions_and_call_isolation() {
    const getnative::AxisPlanRequest base{
        64, 43, 43.25, 0.0, getnative::Filter::bicubic(0.2, 0.4),
        getnative::BorderMode::mirror,
    };
    const std::vector<getnative::AxisPlanRequest> distinct{
        base,
        {65, 43, 43.25, 0.0, base.filter, base.border},
        {64, 42, 43.25, 0.0, base.filter, base.border},
        {64, 43, 43.5, 0.0, base.filter, base.border},
        {64, 43, 43.25, -0.0, base.filter, base.border},
        {64, 43, 43.25, 0.0, getnative::Filter::bicubic(0.3, 0.4), base.border},
        {64, 43, 43.25, 0.0, getnative::Filter::bicubic(0.2, 0.3), base.border},
        {64, 43, 43.25, 0.0, getnative::Filter::lanczos(3), base.border},
        {64, 43, 43.25, 0.0, getnative::Filter::lanczos(4), base.border},
        {64, 43, 43.25, 0.0, base.filter, getnative::BorderMode::repeat},
    };
    const auto first = getnative::detail::build_axis_plans(distinct, {4U, {}, {}});
    expect(first.unique_key_count == distinct.size()
               && first.physical_build_count == distinct.size(),
           "every exact-bit key field remains distinct");
    const auto second = getnative::detail::build_axis_plans(distinct, {4U, {}, {}});
    for (std::size_t index = 0; index < distinct.size(); ++index) {
        expect(first.plans[index].get() != second.plans[index].get(),
               "plan state must not cross batch calls");
    }
}

void test_session_cache_batch_publish_and_ready_reuse() {
    const auto unique = fixture_requests();
    std::vector<getnative::AxisPlanRequest> requests;
    requests.reserve(1000U);
    for (std::size_t index = 0; index < 1000U; ++index) {
        requests.push_back(unique[index % unique.size()]);
    }
    getnative::AxisPlanCache cache({
        unique.size(), 64U * 1024U * 1024U,
    });

    const auto cold = cache.get_or_build_batch(requests, 4U);
    expect(cold.plans.size() == requests.size(),
           "session-cache cold call preserves request count");
    expect(cold.unique_key_count == unique.size() && cold.ready_hit_count == 0U,
           "session-cache cold call identifies every unique miss");
    expect(cold.physical_build_count == unique.size()
               && cold.published_plan_count == unique.size(),
           "session-cache cold call batch-builds and publishes every unique miss");
    expect(cold.effective_worker_count == 4U
               && cold.peak_active_builds <= cold.effective_worker_count,
           "session-cache cold call uses the bounded Stage 1 worker group");
    expect(cold.resident_entry_count == unique.size()
               && cold.resident_bytes == cache.resident_bytes(),
           "session-cache cold call reports retained ownership");

    std::size_t expected_bytes = 0U;
    for (std::size_t index = 0; index < unique.size(); ++index) {
        expected_bytes += getnative::axis_plan_storage_bytes(*cold.plans[index]);
    }
    expect(cold.resident_bytes == expected_bytes,
           "session-cache retained bytes equal unique logical plan storage");
    for (std::size_t index = 0; index < requests.size(); ++index) {
        expect(cold.plans[index].get() == cold.plans[index % unique.size()].get(),
               "session-cache cold result preserves stable pointer sharing");
    }

    const auto warm = cache.get_or_build_batch(requests, 8U);
    expect(warm.unique_key_count == unique.size()
               && warm.ready_hit_count == requests.size(),
           "session-cache warm call serves every request from retained plans");
    expect(warm.physical_build_count == 0U && warm.published_plan_count == 0U
               && warm.effective_worker_count == 0U && warm.peak_active_builds == 0U,
           "session-cache warm call starts no planner work");
    expect(warm.resident_entry_count == cold.resident_entry_count
               && warm.resident_bytes == cold.resident_bytes,
           "session-cache warm call does not grow residency");
    for (std::size_t index = 0; index < requests.size(); ++index) {
        expect(warm.plans[index].get() == cold.plans[index].get(),
               "session-cache warm call returns the published plan pointers");
    }

    std::vector<getnative::AxisPlanCacheBatchResult> concurrent(8U);
    std::vector<getnative::JoiningThread> threads;
    threads.reserve(concurrent.size());
    for (std::size_t thread = 0; thread < concurrent.size(); ++thread) {
        threads.emplace_back([&, thread] {
            concurrent[thread] = cache.get_or_build_batch(requests, 8U);
        });
    }
    threads.clear();
    for (const auto &result : concurrent) {
        expect(result.ready_hit_count == requests.size()
                   && result.physical_build_count == 0U,
               "concurrent warm session calls remain ready-only");
        for (std::size_t index = 0; index < requests.size(); ++index) {
            expect(result.plans[index].get() == cold.plans[index].get(),
                   "concurrent warm calls preserve published pointer identity");
        }
    }

    cache.clear();
    expect(cache.size() == 0U && cache.resident_bytes() == 0U,
           "session-cache clear releases retained ownership and byte accounting");
    expect(cold.plans.front()->valid(),
           "externally held plans survive session-cache clear");
}

void test_session_cache_enforces_lru_eviction() {
    const auto requests = fixture_requests();
    // Entry bound 2 with three unique plans: all three publish; the third
    // admission evicts the first (least recently used).
    getnative::AxisPlanCache entry_bounded({
        2U, 64U * 1024U * 1024U,
    });
    const auto cold = entry_bounded.get_or_build_batch(
        std::span<const getnative::AxisPlanRequest>{requests}.first(3U), 4U);
    expect(cold.physical_build_count == 3U && cold.published_plan_count == 3U
               && cold.resident_entry_count == 2U,
           "entry bound publishes all plans and retains the two most recent");
    const auto repeated = entry_bounded.get_or_build_batch(
        std::span<const getnative::AxisPlanRequest>{requests}.first(3U), 4U);
    expect(repeated.ready_hit_count == 2U && repeated.physical_build_count == 1U
               && repeated.published_plan_count == 1U,
           "the evicted oldest plan rebuilds exactly once and re-admits");
    expect(repeated.plans[0].get() != cold.plans[0].get()
               && repeated.plans[1].get() == cold.plans[1].get()
               && repeated.plans[2].get() == cold.plans[2].get(),
           "LRU keeps the recent pointers and rebuilds the evicted one");

    // Byte bound of the larger plan: admitting the second plan evicts the
    // first. (A plan larger than the cap itself is never retained.)
    const auto first_plan = getnative::build_axis_plan(requests.front());
    const auto second_plan = getnative::build_axis_plan(requests[1]);
    const std::size_t byte_cap = std::max(getnative::axis_plan_storage_bytes(first_plan),
                                          getnative::axis_plan_storage_bytes(second_plan));
    getnative::AxisPlanCache byte_bounded({8U, byte_cap});
    const auto byte_result = byte_bounded.get_or_build_batch(
        std::span<const getnative::AxisPlanRequest>{requests}.first(2U), 2U);
    expect(byte_result.physical_build_count == 2U
               && byte_result.published_plan_count == 2U
               && byte_result.resident_entry_count == 1U,
           "byte bound admits each plan and evicts the previous one");
    expect(byte_bounded.size() == 1U
               && byte_bounded.resident_bytes()
                   == getnative::axis_plan_storage_bytes(*byte_result.plans[1]),
           "session-cache logical bytes never exceed the configured ceiling");

    getnative::AxisPlanCache oversized({8U, 1U});
    const auto oversized_result = oversized.get_or_build_batch(
        std::span<const getnative::AxisPlanRequest>{requests}.first(1U), 1U);
    expect(oversized_result.physical_build_count == 1U
               && oversized_result.published_plan_count == 0U
               && oversized.size() == 0U,
           "a plan larger than the byte cap is returned but never retained");

    getnative::AxisPlanCache no_residency({0U, 0U});
    const auto transient_first = no_residency.get_or_build(requests.front());
    const auto transient_second = no_residency.get_or_build(requests.front());
    expect(transient_first->valid() && transient_second->valid()
               && transient_first.get() != transient_second.get(),
           "zero-capacity compatibility calls return uncached transient plans");
    expect(no_residency.size() == 0U && no_residency.resident_bytes() == 0U,
           "zero-capacity compatibility calls retain no ownership");
}

void test_session_cache_failed_batch_publishes_nothing() {
    const auto requests = fixture_requests();
    getnative::AxisPlanCache cache({8U, 64U * 1024U * 1024U});
    const auto seeded = cache.get_or_build(requests.front());
    const std::size_t seeded_bytes = cache.resident_bytes();
    auto invalid = requests[1U];
    invalid.source_size = 0;
    const std::vector<getnative::AxisPlanRequest> failing{requests[2U], invalid};

    bool caught = false;
    try {
        (void)cache.get_or_build_batch(failing, 2U);
    } catch (const std::invalid_argument &) {
        caught = true;
    }
    expect(caught, "session-cache batch propagates planner validation failure");
    expect(cache.size() == 1U && cache.resident_bytes() == seeded_bytes
               && cache.get_or_build(requests.front()).get() == seeded.get(),
           "failed session-cache batch publishes no partial result");
}

void test_worker_bounds_and_peak_concurrency() {
    auto requests = fixture_requests();
    const auto explicit_many = getnative::detail::build_axis_plans(
        std::span<const getnative::AxisPlanRequest>{requests}.first(3U),
        {std::numeric_limits<std::size_t>::max(), {}, {}});
    expect(explicit_many.effective_worker_count == 3U,
           "explicit workers clamp to unique count");
    expect(explicit_many.peak_active_builds <= explicit_many.effective_worker_count,
           "reported peak respects explicit worker cap");

    std::atomic_size_t arrived{0U};
    const auto concurrent = getnative::detail::build_axis_plans(requests, {
        4U,
        [&](std::size_t) {
            arrived.fetch_add(1U);
            while (arrived.load() < 4U) std::this_thread::yield();
        },
        {},
    });
    expect(concurrent.effective_worker_count == 4U,
           "explicit worker count is reported");
    expect(concurrent.peak_active_builds == 4U,
           "focused hook observes the full bounded worker group");

    const auto automatic = getnative::detail::build_axis_plans(requests);
    const std::size_t automatic_cap = std::max<std::size_t>(
        1U, std::min<std::size_t>(std::thread::hardware_concurrency(), 8U));
    expect(automatic.effective_worker_count == std::min(requests.size(), automatic_cap),
           "automatic worker count is bounded by hardware and eight");
    expect(automatic.peak_active_builds <= automatic.effective_worker_count,
           "automatic peak respects effective worker count");
}

void test_lowest_stable_failure_is_rethrown_after_join() {
    auto requests = fixture_requests();
    std::atomic_bool higher_failure_ready{false};
    bool caught_lowest = false;
    try {
        (void)getnative::detail::build_axis_plans(requests, {
            4U,
            [&](std::size_t index) {
                if (index == 3U) {
                    higher_failure_ready.store(true);
                    throw std::runtime_error("failure-3");
                }
                if (index == 1U) {
                    while (!higher_failure_ready.load()) std::this_thread::yield();
                    throw std::runtime_error("failure-1");
                }
            },
            {},
        });
    } catch (const std::runtime_error &error) {
        caught_lowest = std::string_view{error.what()} == "failure-1";
    }
    expect(caught_lowest, "lowest stable first-occurrence failure is rethrown");
}

void test_failure_stops_claiming_and_joins_started_builds() {
    auto requests = fixture_requests();
    std::atomic_bool slow_build_started{false};
    std::atomic_bool release_slow_build{false};
    std::atomic_bool failure_observed{false};
    std::atomic_bool call_finished{false};
    std::atomic_size_t hook_calls{0U};
    std::exception_ptr captured_failure;

    getnative::JoiningThread caller([&] {
        try {
            (void)getnative::detail::build_axis_plans(requests, {
                2U,
                [&](std::size_t index) {
                    hook_calls.fetch_add(1U);
                    if (index == 0U) {
                        slow_build_started.store(true);
                        while (!release_slow_build.load()) std::this_thread::yield();
                    } else if (index == 1U) {
                        while (!slow_build_started.load()) std::this_thread::yield();
                        throw std::runtime_error("failure-after-slow-start");
                    }
                },
                [&](std::size_t index) {
                    if (index == 1U) failure_observed.store(true);
                },
            });
        } catch (...) {
            captured_failure = std::current_exception();
        }
        call_finished.store(true);
    });

    while (!failure_observed.load()) std::this_thread::yield();
    expect(!call_finished.load(), "batch failure waits for an already-started worker");
    release_slow_build.store(true);
    caller.join();
    expect(captured_failure != nullptr, "batch failure is rethrown after workers join");
    expect(hook_calls.load() == 2U,
           "no new unique key is claimed after the failure is observed");
}

void test_period_replay_repeats_interior_rows() {
    // 1080/800 reduces to 27/20, so the position lattice repeats every 27
    // source rows. Per-row evaluation jitters the repeat by ~1 ulp (this
    // geometry showed 1982 unique distance bit patterns); the period cache
    // replays the class row's raw tap weights instead, making interior
    // forward-weight rows bit-identical within a class. Edge rows differ:
    // boundary coalescing near row 0 and window pinning once row_left
    // reaches destination_size - forward_width.
    const getnative::AxisPlan plan = getnative::build_axis_plan(
        {1080, 800, 800.0, 0.0, getnative::Filter::lanczos(8),
         getnative::BorderMode::mirror});
    const std::size_t width = static_cast<std::size_t>(plan.forward_width);
    expect(plan.forward_weights.size()
               == static_cast<std::size_t>(plan.source_size) * width,
           "forward weights shape differs");
    for (std::int32_t row = 100; row < 700; ++row) {
        const float *current = plan.forward_weights.data()
            + static_cast<std::size_t>(row) * width;
        const float *class_row = current - 27 * width;
        expect(std::memcmp(current, class_row, width * sizeof(float)) == 0,
               "period replay broke interior row periodicity");
    }
    // Fractional active lengths have no small period and still build.
    const getnative::AxisPlan fractional = getnative::build_axis_plan(
        {1080, 800, 800.5, 0.0, getnative::Filter::lanczos(8),
         getnative::BorderMode::mirror});
    expect(fractional.forward_weights.size() == plan.forward_weights.size(),
           "fractional active length plan shape differs");
}

void test_forward_half_pixel_ties_match_zimg_rounding() {
    // 1080 -> 552 is 45/23. Several output centers land exactly on a
    // half-pixel; this catches fast-math reassociation changing the selected
    // left tap from zimg's deterministic half-up rule.
    const getnative::AxisPlan plan = getnative::build_axis_plan(
        {1080, 552, 552.0, 0.0, getnative::Filter::bilinear(),
         getnative::BorderMode::mirror});
    constexpr std::array<std::int32_t, 12> tie_rows{
        112, 202, 247, 382, 427, 472, 742, 787, 832, 877, 922, 967,
    };
    for (const std::int32_t row : tie_rows) {
        const auto begin = plan.forward_offsets[static_cast<std::size_t>(row)];
        const auto left = plan.forward_indices[begin];
        const auto expected = static_cast<std::int32_t>(
            (static_cast<double>(row) + 0.5) * 552.0 / 1080.0 - 0.5);
        expect(left == expected,
               "half-pixel forward tie selected the wrong left tap");
        expect(plan.forward_weights[begin] == 1.0F
                   && plan.forward_weights[begin + 1U] == 0.0F,
               "half-pixel forward tie has unexpected weights");
    }
}

void test_half_pixel_tie_does_not_spike_bilinear_roundtrip() {
    // 840 = 1080 * 7/9. A 9-row period cache would replay row 4's taps onto
    // later classes, but that row sits on a zimg half-pixel tie; 1 ulp can
    // flip the window so cached weights land on the wrong indices and the
    // descale/forward pair spikes. Replay must stay off on this lattice.
    constexpr std::int32_t width = 64;
    constexpr std::int32_t height = 1080;
    std::vector<float> pixels(static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height));
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                   + static_cast<std::size_t>(x)] = static_cast<float>(
                0.25 + 0.5 * std::sin(0.031 * static_cast<double>(y))
                    * std::cos(0.017 * static_cast<double>(x)));
        }
    }
    const getnative::ConstImageView view{pixels.data(), width, height, width};
    const getnative::MetricSpec metric{2, 2, 2, 2, 0.0F, 1U};
    getnative::CpuWorkspace workspace;
    const auto error_at = [&](std::int32_t native_height) {
        const auto plan = getnative::build_axis_plan(
            {height, native_height, static_cast<double>(native_height), 0.0,
             getnative::Filter::bilinear(), getnative::BorderMode::mirror});
        return getnative::analyze_axis_candidate_f32(
            view, plan, getnative::AnalysisAxes::vertical, metric, workspace);
    };
    const double neighbor = std::max(error_at(839), error_at(841));
    const double tied = error_at(840);
    expect(std::isfinite(tied) && std::isfinite(neighbor),
           "bilinear 7/9 tie roundtrip produced a non-finite error");
    expect(tied <= 10.0 * std::max(neighbor, 1e-12),
           "bilinear 7/9 tie height spiked relative to 839/841");
}

} // namespace

int main() {
    try {
        test_empty_and_stable_deduplication();
        test_exact_plans_for_all_worker_modes();
        test_tap_reuse_is_byte_identical();
        test_bicubic_geometry_reuse_is_byte_identical();
        test_exact_bit_key_distinctions_and_call_isolation();
        test_session_cache_batch_publish_and_ready_reuse();
        test_session_cache_enforces_lru_eviction();
        test_session_cache_failed_batch_publishes_nothing();
        test_worker_bounds_and_peak_concurrency();
        test_lowest_stable_failure_is_rethrown_after_join();
        test_failure_stops_claiming_and_joins_started_builds();
        test_period_replay_repeats_interior_rows();
        test_forward_half_pixel_ties_match_zimg_rounding();
        test_half_pixel_tie_does_not_spike_bilinear_roundtrip();
        std::cout << "axis planner tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "axis planner test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
