#pragma once

// Cold (persistent) plan store — L2 of the two-level plan cache (E4).
//
// One GNPK file per (candidate grid x build fingerprint): plans grouped
// 64-per-chunk as independent zstd-1 frames behind a sorted key-hash index,
// with the cooked structural pre-transform from plan_serialize. Layout and
// measured codec choices: docs/cold-plan-cache-evaluation.md §3.2.
//
// Plans are content-independent of image pixels (the key is geometry +
// kernel), so packs are reused across images, sessions, and machines that
// share the build fingerprint. Every failure mode degrades to "miss and
// rebuild"; a corrupt store can never fail a job.

#include "getnative/axis_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace getnative {

struct StoredGrid {
    std::vector<AxisPlanRequest> requests;
    std::vector<std::shared_ptr<const AxisPlan>> plans;
};

class PlanStore {
public:
    struct Limits {
        std::size_t maximum_bytes = 1024U * 1024U * 1024U; // 1 GiB
    };

    PlanStore(std::filesystem::path directory, Limits limits);
    explicit PlanStore(std::filesystem::path directory);
    ~PlanStore();
    PlanStore(const PlanStore &) = delete;
    PlanStore &operator=(const PlanStore &) = delete;

    // Canonical grid identity: sorted, deduplicated plan keys, FNV-1a.
    static std::uint64_t grid_hash(std::span<const AxisPlanRequest> requests);
    // Compiler + planner FP mode + structure flags + content version.
    static std::uint64_t build_fingerprint();

    // Open, validate (magic, version, fingerprint, header checksum, zstd
    // frame checksums, structural plan validation), and decode every plan.
    // Any failure deletes the file and returns nullopt.
    [[nodiscard]] std::optional<StoredGrid> read_grid(std::uint64_t grid_hash);

    // Sparse read: resolve individual requests through the sorted key-hash
    // index, decompressing only the chunks that contain them (~0.4 ms per
    // 64-plan chunk). Returns plans parallel to `requests`; any failure
    // (missing pack, missing key, validation error) yields nullopt and
    // deletes a corrupt pack. This is the read path for over-capacity grids
    // where whole-pack preheat would thrash the L1 byte cap.
    [[nodiscard]] std::optional<std::vector<std::shared_ptr<const AxisPlan>>>
    read_plans(std::uint64_t grid_hash, std::span<const AxisPlanRequest> requests);

    // Atomic tmp+rename publish; never replaces an existing pack (concurrent
    // writers are safe). Returns false when the pack already existed or the
    // write failed — publishing is best-effort by contract.
    bool publish_grid(std::uint64_t grid_hash,
                      std::span<const AxisPlanRequest> requests,
                      std::span<const std::shared_ptr<const AxisPlan>> plans);

    // mtime-LRU eviction down to the byte cap. Called once at construction.
    void sweep();

private:
    [[nodiscard]] std::filesystem::path pack_path(std::uint64_t grid_hash) const;

    std::filesystem::path directory_;
    Limits limits_;
};

} // namespace getnative
