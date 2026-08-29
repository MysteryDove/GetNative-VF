#pragma once

#include "getnative/media_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace getnative::cli {

struct RemainingVerifyFrames {
    std::vector<media::FrameIdentity> identities;
    std::vector<std::uint64_t> original_seqs;
};

// Completion-order verify emission does not leave a contiguous selected-frame
// prefix. Resume software fallback from the unfinished identities and keep
// their original seq numbers.
[[nodiscard]] inline RemainingVerifyFrames remaining_verify_frames(
    std::span<const media::FrameIdentity> selected,
    std::span<const char> finished) {
    RemainingVerifyFrames remaining;
    remaining.identities.reserve(selected.size());
    remaining.original_seqs.reserve(selected.size());
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (index < finished.size() && finished[index] != 0) continue;
        remaining.identities.push_back(selected[index]);
        remaining.original_seqs.push_back(static_cast<std::uint64_t>(index));
    }
    return remaining;
}

} // namespace getnative::cli
