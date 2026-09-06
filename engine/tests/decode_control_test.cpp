#include "getnative/decode_control.hpp"

#include <cstdlib>
#include <iostream>

using namespace getnative::media;

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
DecodeDecision windows(AdaptiveDecodeController &c, DecodeWindow w, int count,
                       double remaining = 30.0) {
    DecodeDecision result;
    for (int i = 0; i < count; ++i) result = c.observe(w, remaining, 0.25);
    return result;
}
void expand(AdaptiveDecodeController &c, double throughput = 100.0) {
    require(windows(c, {throughput, 0.2, 0.0}, 2).reason
                == DecodeChangeReason::starvation_probe, "expand after two windows");
    require(windows(c, {1.0, 0.9, 0.0}, 3).reason
                == DecodeChangeReason::none, "initialization excluded from measurement");
    c.tier_ready();
}
}

int main() {
    AdaptiveDecodeController c{4};
    expand(c);
    require(c.sessions() == 2, "first tier is two");
    windows(c, {110.0, 0.2, 0.0}, 1);
    windows(c, {1000.0, 0.2, 0.0}, 1);
    require(windows(c, {110.0, 0.2, 0.0}, 1).reason
                == DecodeChangeReason::gain_retained, "use median of three stable windows");
    expand(c, 110.0);
    require(c.sessions() == 4, "second tier is four");
    require(windows(c, {115.0, 0.2, 0.0}, 3).reason
                == DecodeChangeReason::probe_reverted, "less than eight percent reverts");
    require(c.probing(), "rollback waits for sessions to retire");
    c.tier_ready();
    require(windows(c, {110.0, 0.2, 0.0}, 8).sessions == 2, "failed expansion frozen");
    require(windows(c, {110.0, 0.0, 0.4}, 3).reason
                == DecodeChangeReason::capacity_probe, "backpressure causes shrink");
    c.tier_ready();
    require(windows(c, {107.0, 0.0, 0.4}, 3).reason
                == DecodeChangeReason::gain_retained, "shrink within three percent retained");
    require(c.sessions() == 1, "shrink to one");

    AdaptiveDecodeController regression{2};
    expand(regression);
    windows(regression, {150.0, 0.2, 0.0}, 3);
    windows(regression, {150.0, 0.0, 0.4}, 3);
    regression.tier_ready();
    require(windows(regression, {100.0, 0.0, 0.4}, 3).sessions == 2,
            "slow shrink restores previous tier");
    regression.tier_ready();
    require(windows(regression, {150.0, 0.0, 0.4}, 8).sessions == 2,
            "failed shrink frozen");

    AdaptiveDecodeController short_job{4};
    require(windows(short_job, {100.0, 0.5, 0.0}, 5, 3.0).sessions == 1,
            "short job and tail do not probe");
    require(windows(short_job, {100.0, 0.5, 0.0}, 5, 6.0).sessions == 1,
            "expansion must leave three windows at prospective doubled throughput");
    require(windows(short_job, {100.0, 0.5, 0.0}, 1, 6.25).sessions == 2,
            "expansion admits enough work for initialization and three windows");
    AdaptiveDecodeController limited{4};
    expand(limited);
    require(limited.resource_limit(1).sessions == 1, "creation failure cancels probe");
    require(windows(limited, {100.0, 0.5, 0.0}, 10).sessions == 1,
            "resource cap remains for job");
    require(decode_budget({}).maximum_sessions == 1, "unknown budget means one");
    constexpr std::uint64_t mib = 1024 * 1024;
    DecodeResources resources{4096 * mib, 0, 128 * mib, 128 * mib, 10, 8};
    require(decode_budget(resources).maximum_sessions == 4, "ample budget permits four");
    require(decode_budget(resources).extra_bytes == 896 * mib,
            "existing session deducted from capped allowance");
    resources.analysis_concurrency = 3;
    require(decode_budget(resources).maximum_sessions == 2, "tiers capped by analysis");
    resources.analysis_concurrency = 8;
    resources.safe_partitions = 1;
    require(decode_budget(resources).maximum_sessions == 1, "no safe partition means one");
    resources.safe_partitions = 10;
    resources.analysis_reserved_bytes = 4000 * mib;
    require(decode_budget(resources).maximum_sessions == 1, "reservations exhaust budget");
    resources.analysis_reserved_bytes = 0;
    resources.session_bytes = UINT64_MAX;
    require(decode_budget(resources).maximum_sessions == 1, "estimate overflow fails closed");

    DecodeDemandSampler sampler{{0.0, 0, 0.0, 0.0, 0.0, 0.0, 8, 0}};
    require(!sampler.sample({500.0, 50, 0.0, 0.0, 0.0, 0.0, 8, 0}), "one second cadence");
    const auto sample = sampler.sample({1000.0, 100, 1600.0, 100.0, 2000.0, 4000.0, 8, 4});
    require(sample && sample->throughput == 100.0 && sample->starvation == 0.2
                && sample->backpressure == 0.05 && sample->queue_occupancy == 0.5,
            "normalize by thread-time, not wall time");
    std::cout << "decode controller tests passed\n";
}
