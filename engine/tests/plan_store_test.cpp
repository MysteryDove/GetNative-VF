// Unit tests for the cold plan store (E4): cooked serialization roundtrip,
// GNPK pack write/read, fingerprint and corruption gating, eviction, and
// the single-flight LRU admission policy of AxisPlanCache.

#include "getnative/axis_plan.hpp"
#include "getnative/plan_store.hpp"
#include "getnative/utf8_path.hpp"

#include "plan_serialize.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *name, const std::string &detail = "") {
    if (condition) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s %s\n", name, detail.c_str());
        ++failures;
    }
}

getnative::AxisPlanRequest make_request(std::int32_t source, double destination,
                                        const getnative::Filter &filter,
                                        getnative::BorderMode border) {
    getnative::AxisPlanRequest request;
    request.source_size = source;
    request.destination_size = static_cast<std::int32_t>(destination);
    request.active_length = destination;
    request.shift = 0.0;
    request.filter = filter;
    request.border = border;
    return request;
}

bool plan_bytes_equal(const getnative::AxisPlan &lhs, const getnative::AxisPlan &rhs) {
    const auto scalars_equal = lhs.source_size == rhs.source_size
        && lhs.destination_size == rhs.destination_size
        && lhs.support == rhs.support
        && lhs.half_bandwidth == rhs.half_bandwidth
        && lhs.forward_width == rhs.forward_width
        && std::bit_cast<std::uint64_t>(lhs.active_length)
            == std::bit_cast<std::uint64_t>(rhs.active_length)
        && std::bit_cast<std::uint64_t>(lhs.shift) == std::bit_cast<std::uint64_t>(rhs.shift);
    return scalars_equal
        && lhs.forward_offsets == rhs.forward_offsets
        && lhs.forward_indices == rhs.forward_indices
        && lhs.forward_weights == rhs.forward_weights
        && lhs.transpose_offsets == rhs.transpose_offsets
        && lhs.transpose_indices == rhs.transpose_indices
        && lhs.transpose_weights == rhs.transpose_weights
        && lhs.lower_ld == rhs.lower_ld
        && lhs.upper_l == rhs.upper_l
        && lhs.inverse_diagonal == rhs.inverse_diagonal;
}

bool request_equal(const getnative::AxisPlanRequest &lhs,
                   const getnative::AxisPlanRequest &rhs) {
    return lhs.source_size == rhs.source_size
        && lhs.destination_size == rhs.destination_size
        && std::bit_cast<std::uint64_t>(lhs.active_length)
            == std::bit_cast<std::uint64_t>(rhs.active_length)
        && std::bit_cast<std::uint64_t>(lhs.shift) == std::bit_cast<std::uint64_t>(rhs.shift)
        && lhs.filter.type == rhs.filter.type
        && std::bit_cast<std::uint64_t>(lhs.filter.b)
            == std::bit_cast<std::uint64_t>(rhs.filter.b)
        && std::bit_cast<std::uint64_t>(lhs.filter.c)
            == std::bit_cast<std::uint64_t>(rhs.filter.c)
        && lhs.filter.taps == rhs.filter.taps
        && lhs.border == rhs.border;
}

void test_serialize_roundtrip() {
    const std::vector<getnative::Filter> filters = {
        getnative::Filter::bilinear(),
        getnative::Filter::bicubic(0.0, 0.5),
        getnative::Filter::bicubic(0.2, 0.7),
        getnative::Filter::lanczos(3),
        getnative::Filter::lanczos(8),
        getnative::Filter::spline16(),
        getnative::Filter::spline36(),
        getnative::Filter::spline64(),
    };
    const std::vector<getnative::BorderMode> borders = {
        getnative::BorderMode::mirror,
        getnative::BorderMode::repeat,
        getnative::BorderMode::zero,
    };
    const std::vector<std::pair<std::int32_t, double>> shapes = {
        {1080, 810.0},
        {1080, 810.5},
        {240, 200.0},
        {1080, 1079.0},
    };
    bool all_equal = true;
    std::size_t cases = 0;
    for (const auto &filter : filters) {
        for (const auto border : borders) {
            for (const auto &[source, destination] : shapes) {
                const auto request = make_request(source, destination, filter, border);
                const getnative::AxisPlan plan = getnative::build_axis_plan(request);
                const std::vector<std::byte> cooked =
                    getnative::detail::serialize_plan_cooked(plan, request);
                getnative::AxisPlanRequest reconstructed;
                const getnative::AxisPlan decoded =
                    getnative::detail::deserialize_plan_cooked(cooked, &reconstructed);
                all_equal = all_equal && plan_bytes_equal(plan, decoded)
                    && request_equal(request, reconstructed);
                ++cases;
            }
        }
    }
    check(all_equal, "serialize-roundtrip", std::to_string(cases) + " cases");
}

void test_serialize_rejects_garbage() {
    const auto request = make_request(1080, 810.0, getnative::Filter::bicubic(0.0, 0.5),
                                      getnative::BorderMode::mirror);
    const getnative::AxisPlan plan = getnative::build_axis_plan(request);
    const std::vector<std::byte> cooked = getnative::detail::serialize_plan_cooked(plan, request);
    bool threw_on_truncation = false;
    try {
        [[maybe_unused]] const getnative::AxisPlan discarded =
            getnative::detail::deserialize_plan_cooked({cooked.data(), cooked.size() / 2U});
    } catch (const getnative::detail::PlanStoreError &) {
        threw_on_truncation = true;
    }
    bool threw_on_corruption = false;
    std::vector<std::byte> corrupted = cooked;
    corrupted[4] ^= std::byte{0x7FU}; // destination_size → insane
    try {
        [[maybe_unused]] const getnative::AxisPlan discarded =
            getnative::detail::deserialize_plan_cooked(corrupted);
    } catch (const getnative::detail::PlanStoreError &) {
        threw_on_corruption = true;
    }
    check(threw_on_truncation && threw_on_corruption, "serialize-rejects-garbage");
}

struct ScratchDir {
    ScratchDir() {
        path = std::filesystem::temp_directory_path()
            / ("getnative-plan-store-test-" + std::to_string(
                std::hash<std::thread::id>{}(std::this_thread::get_id())));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

std::vector<getnative::AxisPlanRequest> grid_requests(
    std::int32_t source, std::int32_t first, std::size_t count,
    const getnative::Filter &filter) {
    std::vector<getnative::AxisPlanRequest> requests;
    for (std::size_t index = 0; index < count; ++index) {
        requests.push_back(make_request(
            source, static_cast<double>(first) + static_cast<double>(index), filter,
            getnative::BorderMode::mirror));
    }
    return requests;
}

std::vector<std::shared_ptr<const getnative::AxisPlan>> build_all(
    const std::vector<getnative::AxisPlanRequest> &requests) {
    std::vector<std::shared_ptr<const getnative::AxisPlan>> plans;
    plans.reserve(requests.size());
    for (const auto &request : requests) {
        plans.push_back(std::make_shared<const getnative::AxisPlan>(
            getnative::build_axis_plan(request)));
    }
    return plans;
}

void test_pack_roundtrip() {
    ScratchDir scratch;
    const auto requests = grid_requests(1080, 700, 67, getnative::Filter::lanczos(3));
    const auto plans = build_all(requests);
    const std::uint64_t grid = getnative::PlanStore::grid_hash(requests);

    getnative::PlanStore store(scratch.path);
    check(store.publish_grid(grid, requests, plans), "pack-publish");
    // No-replace policy: a second publish of the same grid is refused.
    check(!store.publish_grid(grid, requests, plans), "pack-no-replace");

    const std::optional<getnative::StoredGrid> loaded = store.read_grid(grid);
    // The on-disk index is key-hash ordered, so compare as sets: every
    // loaded request must pair with the byte-identical original plan.
    bool equal = loaded.has_value() && loaded->requests.size() == requests.size();
    if (equal) {
        for (std::size_t index = 0; index < requests.size(); ++index) {
            bool matched = false;
            for (std::size_t original = 0; original < requests.size(); ++original) {
                if (request_equal(loaded->requests[index], requests[original])) {
                    matched = plan_bytes_equal(*loaded->plans[index], *plans[original]);
                    break;
                }
            }
            equal = equal && matched;
        }
    }
    check(equal, "pack-roundtrip-67-plans");
}

void test_pack_roundtrip_in_unicode_directory() {
    ScratchDir scratch;
    const std::string directory_name =
        "\xe4\xb8\xad\xe6\x96\x87-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e-\xed\x95\x9c\xea\xb8\x80";
    const std::filesystem::path directory =
        scratch.path / getnative::path_from_utf8(directory_name);
    const auto requests = grid_requests(240, 200, 3, getnative::Filter::bilinear());
    const auto plans = build_all(requests);
    const std::uint64_t grid = getnative::PlanStore::grid_hash(requests);

    getnative::PlanStore store(directory);
    const bool published = store.publish_grid(grid, requests, plans);
    const auto loaded = store.read_grid(grid);
    check(published && loaded.has_value() && loaded->plans.size() == plans.size(),
          "pack-roundtrip-unicode-directory");
}

void test_pack_gating() {
    ScratchDir scratch;
    const auto requests = grid_requests(240, 200, 4, getnative::Filter::bicubic(0.0, 0.5));
    const auto plans = build_all(requests);
    const std::uint64_t grid = getnative::PlanStore::grid_hash(requests);

    getnative::PlanStore::Limits limits;
    getnative::PlanStore store(scratch.path, limits);
    store.publish_grid(grid, requests, plans);

    // Fingerprint mismatch: flip the fingerprint field (bytes 16..24) in
    // place → the reader must reject and remove the pack.
    std::filesystem::path pack;
    for (const auto &entry : std::filesystem::directory_iterator(scratch.path)) {
        if (entry.path().extension() == ".gnpk") pack = entry.path();
    }
    check(!pack.empty(), "pack-file-exists");
    {
        std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(16);
        const char flip = '\x7F';
        stream.write(&flip, 1);
    }
    check(!store.read_grid(grid).has_value(), "pack-fingerprint-mismatch");
    check(!std::filesystem::exists(pack), "pack-deleted-after-mismatch");

    // Chunk corruption: rebuild a pack, flip a byte inside the final chunk
    // frame → zstd frame checksum must catch it.
    store.publish_grid(grid, requests, plans);
    pack.clear();
    for (const auto &entry : std::filesystem::directory_iterator(scratch.path)) {
        if (entry.path().extension() == ".gnpk") pack = entry.path();
    }
    const auto pack_size = std::filesystem::file_size(pack);
    {
        std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(static_cast<std::streamoff>(pack_size - 3));
        char byte = 0;
        stream.read(&byte, 1);
        byte ^= '\x40';
        stream.seekp(static_cast<std::streamoff>(pack_size - 3));
        stream.write(&byte, 1);
    }
    check(!store.read_grid(grid).has_value(), "pack-chunk-corruption");
    check(!std::filesystem::exists(pack), "pack-deleted-after-corruption");
}

void test_pack_eviction() {
    ScratchDir scratch;
    getnative::PlanStore::Limits limits;
    limits.maximum_bytes = 0; // everything is over budget
    getnative::PlanStore store(scratch.path, limits);

    const auto requests = grid_requests(240, 200, 3, getnative::Filter::bilinear());
    const auto plans = build_all(requests);
    const std::uint64_t grid = getnative::PlanStore::grid_hash(requests);
    store.publish_grid(grid, requests, plans);
    // Sweep with a zero budget deletes every pack.
    store.sweep();
    bool any_left = false;
    for (const auto &entry : std::filesystem::directory_iterator(scratch.path)) {
        if (entry.path().extension() == ".gnpk") any_left = true;
    }
    check(!any_left, "pack-eviction-sweep");
}

void test_lru_policy() {
    getnative::AxisPlanCacheLimits limits;
    limits.maximum_entries = 3;
    limits.maximum_resident_bytes = 1ULL << 30;
    getnative::AxisPlanCache cache(limits);
    const auto filter = getnative::Filter::bicubic(0.0, 0.5);
    const auto a = make_request(1080, 810.0, filter, getnative::BorderMode::mirror);
    const auto b = make_request(1080, 811.0, filter, getnative::BorderMode::mirror);
    const auto c = make_request(1080, 812.0, filter, getnative::BorderMode::mirror);
    const auto d = make_request(1080, 813.0, filter, getnative::BorderMode::mirror);
    [[maybe_unused]] const auto keep_a = cache.get_or_build(a);
    [[maybe_unused]] const auto keep_b = cache.get_or_build(b);
    [[maybe_unused]] const auto keep_c = cache.get_or_build(c);
    // Touch a, then admit d → the LRU victim is b, not a.
    [[maybe_unused]] const auto touch_a = cache.get_or_build(a);
    [[maybe_unused]] const auto keep_d = cache.get_or_build(d);
    getnative::AxisPlanCacheBatchResult probe = cache.get_or_build_batch(
        std::vector<getnative::AxisPlanRequest>{a, b, c, d});
    check(probe.ready_hit_count == 3 && probe.physical_build_count == 1,
          "lru-evicts-least-recent",
          "hits=" + std::to_string(probe.ready_hit_count)
              + " builds=" + std::to_string(probe.physical_build_count));

    // A repeated scan larger than the entry cap retains everything that
    // fits: with the cap raised, 1,000 unique plans all stay resident.
    getnative::AxisPlanCacheLimits wide;
    wide.maximum_entries = 1024;
    wide.maximum_resident_bytes = 1ULL << 30;
    getnative::AxisPlanCache scan_cache(wide);
    const auto grid = grid_requests(1080, 700, 1000, getnative::Filter::lanczos(6));
    [[maybe_unused]] const getnative::AxisPlanCacheBatchResult first_scan =
        scan_cache.get_or_build_batch(grid);
    getnative::AxisPlanCacheBatchResult rescan = scan_cache.get_or_build_batch(grid);
    check(rescan.ready_hit_count == 1000 && rescan.physical_build_count == 0,
          "lru-retains-1000-plan-scan",
          "hits=" + std::to_string(rescan.ready_hit_count));
}

void test_single_flight() {
    getnative::AxisPlanCache cache;
    const auto request = make_request(1080, 810.0, getnative::Filter::lanczos(4),
                                      getnative::BorderMode::mirror);
    std::vector<std::shared_ptr<const getnative::AxisPlan>> results(8);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < results.size(); ++index) {
        threads.emplace_back([&, index] {
            results[index] = cache.get_or_build(request);
        });
    }
    for (auto &thread : threads) thread.join();
    bool same_instance = true;
    for (const auto &result : results) {
        same_instance = same_instance && result == results.front();
    }
    check(same_instance, "single-flight-same-instance");
}

} // namespace

int main() {
    test_serialize_roundtrip();
    test_serialize_rejects_garbage();
    test_pack_roundtrip();
    test_pack_roundtrip_in_unicode_directory();
    test_pack_gating();
    test_pack_eviction();
    test_lru_policy();
    test_single_flight();
    if (failures != 0) {
        std::printf("%d plan store test(s) failed\n", failures);
        return 1;
    }
    std::printf("plan store tests passed\n");
    return 0;
}
