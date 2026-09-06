#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace getnative::media {

// Cumulative counters use milliseconds summed across waiting threads. The
// sampler divides their deltas by the corresponding thread-time exposure.
struct DecodeDemandSnapshot {
    double elapsed_ms = 0.0;
    std::uint64_t completed = 0;
    double starvation_thread_ms = 0.0;
    double capacity_thread_ms = 0.0;
    // Filled by the media scheduler, which knows active session lifetimes.
    double producer_thread_ms = 0.0;
    double queue_frame_ms = 0.0;
    std::size_t analysis_threads = 1;
    std::size_t inflight = 0;
};

struct DecodeWindow {
    double throughput = 0.0;
    double starvation = 0.0;
    double backpressure = 0.0;
    double queue_occupancy = 0.0;
};

class DecodeDemandSampler {
public:
    explicit DecodeDemandSampler(DecodeDemandSnapshot initial) : previous_(initial) {}

    std::optional<DecodeWindow> sample(const DecodeDemandSnapshot &next) {
        const double elapsed = next.elapsed_ms - previous_.elapsed_ms;
        if (elapsed < 1000.0) return std::nullopt;
        if (next.completed < previous_.completed
            || next.analysis_threads != previous_.analysis_threads) {
            previous_ = next;
            return std::nullopt;
        }
        const double producer_time = next.producer_thread_ms - previous_.producer_thread_ms;
        const double analysis_time = elapsed * static_cast<double>(next.analysis_threads);
        DecodeWindow result{
            1000.0 * static_cast<double>(next.completed - previous_.completed) / elapsed,
            ratio(next.starvation_thread_ms - previous_.starvation_thread_ms, analysis_time),
            ratio(next.capacity_thread_ms - previous_.capacity_thread_ms, producer_time),
            ratio(next.queue_frame_ms - previous_.queue_frame_ms, analysis_time)};
        previous_ = next;
        return result;
    }

private:
    DecodeDemandSnapshot previous_;
    static double ratio(double numerator, double denominator) {
        return denominator > 0.0 ? std::clamp(numerator / denominator, 0.0, 1.0) : 0.0;
    }
};

struct DecodeResources {
    std::optional<std::uint64_t> available_bytes;
    std::uint64_t analysis_reserved_bytes = 0;
    std::uint64_t existing_session_bytes = 0;
    // Backend estimate includes profile minimum DPB and internal surfaces.
    std::uint64_t session_bytes = 0;
    std::size_t safe_partitions = 1;
    std::size_t analysis_concurrency = 1;
};

struct DecodeBudget {
    std::uint64_t extra_bytes = 0;
    std::uint64_t session_bytes = 0;
    std::size_t maximum_sessions = 1;
};

inline DecodeBudget decode_budget(const DecodeResources &r) {
    DecodeBudget result;
    if (!r.available_bytes || r.session_bytes == 0) return result;
    const auto allowance = std::min(*r.available_bytes / 4, std::uint64_t{1} << 30);
    if (r.analysis_reserved_bytes >= allowance
        || r.existing_session_bytes >= allowance - r.analysis_reserved_bytes) return result;
    result.extra_bytes = allowance - r.analysis_reserved_bytes - r.existing_session_bytes;
    const auto margin = r.session_bytes / 4 + (r.session_bytes % 4 != 0 ? 1U : 0U);
    if (r.session_bytes > UINT64_MAX - margin) return result;
    result.session_bytes = r.session_bytes + margin;
    const auto extra = std::min<std::uint64_t>(3, result.extra_bytes / result.session_bytes);
    const auto limit = std::min({std::size_t{4}, r.analysis_concurrency,
                                 r.safe_partitions, static_cast<std::size_t>(extra + 1)});
    result.maximum_sessions = limit >= 4 ? 4 : limit >= 2 ? 2 : 1;
    return result;
}

enum class DecodeChangeReason {
    none, starvation_probe, capacity_probe, gain_retained, probe_reverted,
    resource_limit,
};

inline const char *decode_change_reason(DecodeChangeReason reason) {
    switch (reason) {
    case DecodeChangeReason::none: return "none";
    case DecodeChangeReason::starvation_probe: return "starvation_probe";
    case DecodeChangeReason::capacity_probe: return "capacity_probe";
    case DecodeChangeReason::gain_retained: return "gain_retained";
    case DecodeChangeReason::probe_reverted: return "probe_reverted";
    case DecodeChangeReason::resource_limit: return "resource_limit";
    }
    return "unknown";
}

struct DecodeDecision {
    std::size_t sessions = 1;
    DecodeChangeReason reason = DecodeChangeReason::none;
};

// Pure, job-local state machine. Called with complete one-second windows;
// session management and clocks belong to the caller. A changed tier must be
// acknowledged only after new-session output / retiring-session completion.
class AdaptiveDecodeController {
public:
    explicit AdaptiveDecodeController(std::size_t maximum)
        : maximum_(maximum >= 4 ? 4 : maximum >= 2 ? 2 : 1) {}

    [[nodiscard]] std::size_t sessions() const { return target_; }
    [[nodiscard]] bool probing() const { return phase_ != Phase::stable; }

    void tier_ready() {
        if (phase_ == Phase::settling) {
            reset_stable();
            return;
        }
        if (phase_ == Phase::warming) {
            phase_ = Phase::measuring;
            sample_count_ = 0;
        }
    }

    DecodeDecision resource_limit(std::size_t maximum) {
        maximum_ = std::min(maximum_, maximum >= 4 ? std::size_t{4}
                                    : maximum >= 2 ? std::size_t{2} : std::size_t{1});
        target_ = std::min(target_, maximum_);
        previous_tier_ = target_;
        reset_stable();
        return {target_, DecodeChangeReason::resource_limit};
    }

    DecodeDecision observe(const DecodeWindow &w, double remaining_seconds,
                           double initialization_seconds) {
        if (!std::isfinite(w.throughput) || w.throughput < 0.0
            || !std::isfinite(w.starvation) || !std::isfinite(w.backpressure)) return {target_};
        if (phase_ == Phase::warming || phase_ == Phase::settling) return {target_};
        if (phase_ == Phase::measuring) {
            samples_[sample_count_++] = w.throughput;
            if (sample_count_ < 3) return {target_};
            const double measured = median(samples_);
            const bool up = target_ > previous_tier_;
            const bool retain = baseline_ > 0.0
                && measured >= baseline_ * (up ? 1.08 : 0.97);
            if (!retain) {
                // Freeze this edge in both directions after a failed probe.
                frozen_[edge(std::max(target_, previous_tier_))] = true;
                target_ = previous_tier_;
            }
            reset_stable();
            if (!retain) phase_ = Phase::settling;
            return {target_, retain ? DecodeChangeReason::gain_retained
                                    : DecodeChangeReason::probe_reverted};
        }
        history_[history_count_++ % 3] = w.throughput;
        shortage_ = w.starvation >= 0.15 && w.backpressure <= 0.05 ? shortage_ + 1 : 0;
        capacity_ = w.backpressure >= 0.30 && w.starvation <= 0.05 ? capacity_ + 1 : 0;
        if (history_count_ < 2 || w.throughput <= 0.0
            || !std::isfinite(remaining_seconds)
            || !std::isfinite(initialization_seconds) || initialization_seconds < 0.0
            || remaining_seconds < initialization_seconds + 3.0) return {target_};
        // remaining_seconds is measured at the current tier. An expansion can
        // consume the tail twice as fast; reserve three full windows at that
        // prospective rate, after initialization, before admitting the probe.
        if (shortage_ >= 2 && target_ < maximum_ && !frozen_[edge(target_ * 2)]
            && remaining_seconds >= initialization_seconds + 6.0) {
            return begin(target_ * 2, DecodeChangeReason::starvation_probe);
        }
        if (capacity_ >= 3 && target_ > 1 && !frozen_[edge(target_)]) {
            return begin(target_ / 2, DecodeChangeReason::capacity_probe);
        }
        return {target_};
    }

private:
    enum class Phase { stable, warming, measuring, settling };
    Phase phase_ = Phase::stable;
    std::size_t maximum_, target_ = 1, previous_tier_ = 1;
    std::array<bool, 2> frozen_{};
    std::array<double, 3> history_{}, samples_{};
    std::size_t history_count_ = 0, sample_count_ = 0, shortage_ = 0, capacity_ = 0;
    double baseline_ = 0.0;

    static std::size_t edge(std::size_t upper) { return upper == 4 ? 1 : 0; }
    static double median(std::array<double, 3> values) {
        std::sort(values.begin(), values.end());
        return values[1];
    }
    void reset_stable() {
        phase_ = Phase::stable;
        history_count_ = sample_count_ = shortage_ = capacity_ = 0;
    }
    DecodeDecision begin(std::size_t next, DecodeChangeReason reason) {
        baseline_ = history_count_ == 2 ? (history_[0] + history_[1]) / 2.0
                                       : median(history_);
        previous_tier_ = target_;
        target_ = next;
        phase_ = Phase::warming;
        return {target_, reason};
    }
};

} // namespace getnative::media
