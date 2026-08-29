#pragma once

// Cooked plan serialization for the cold plan store (E4).
//
// Lossless structural pre-transform measured in
// docs/cold-plan-cache-evaluation.md §3.1: uniform forward offsets dropped,
// forward indices reduced to per-row anchors, CSR offset/index arrays
// delta-varint encoded. Every cooked blob is self-describing: the header
// carries the full plan-key fields, so a store reader can reconstruct both
// the AxisPlan and the originating AxisPlanRequest.

#include "getnative/axis_plan.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace getnative::detail {

class PlanStoreError : public std::runtime_error {
public:
    explicit PlanStoreError(const std::string &message) : std::runtime_error(message) {}
};

[[nodiscard]] std::vector<std::byte> serialize_plan_cooked(
    const AxisPlan &plan, const AxisPlanRequest &request);

// Throws PlanStoreError on any structural inconsistency. When request_out is
// non-null, the originating request is reconstructed from the header.
[[nodiscard]] AxisPlan deserialize_plan_cooked(
    std::span<const std::byte> payload, AxisPlanRequest *request_out = nullptr);

} // namespace getnative::detail
