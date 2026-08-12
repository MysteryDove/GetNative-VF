#pragma once

#include "getnative/types.hpp"

#include <vector>

namespace getnative {

[[nodiscard]] std::vector<Candidate> generate_candidates(
    const CandidateGridSpec& spec,
    GridSemantics semantics);

[[nodiscard]] std::vector<Candidate> generate_candidate_range(
    const CandidateRangeSpec& spec,
    GridSemantics semantics);

} // namespace getnative
