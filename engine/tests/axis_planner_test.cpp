#include "axis_planner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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
        expect_plan_equal(reused, recomputed);
        expect_plan_equal(getnative::build_axis_plan(request), reused);
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
        expect_plan_equal(*reused_batch.plans[index], *recomputed_batch.plans[index]);
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

    std::jthread caller([&] {
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

} // namespace

int main() {
    try {
        test_empty_and_stable_deduplication();
        test_exact_plans_for_all_worker_modes();
        test_tap_reuse_is_byte_identical();
        test_exact_bit_key_distinctions_and_call_isolation();
        test_worker_bounds_and_peak_concurrency();
        test_lowest_stable_failure_is_rethrown_after_join();
        test_failure_stops_claiming_and_joins_started_builds();
        std::cout << "axis planner tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "axis planner test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
