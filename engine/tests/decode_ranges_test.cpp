#include "getnative/decode_ranges.hpp"
#include "getnative/decode_restart.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace getnative::media;
namespace {
void require(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
void deliver(IndexedRangeScheduler &scheduler, IndexedRangeScheduler::Lease lease,
             std::size_t ordinal) {
    require(scheduler.reserve(lease, ordinal) == IndexedRangeScheduler::Delivery::reserved,
            "frame must have one owner");
    scheduler.commit(lease, ordinal);
}
}

int main() {
    const auto h264 = RestartCodec::h264;
    const auto hevc = RestartCodec::hevc;
    require(packet_has_closed_restart(std::array<std::uint8_t, 6>{0, 0, 0, 1, 0x65, 0x80}, h264), "Annex B IDR");
    require(!packet_has_closed_restart(std::array<std::uint8_t, 5>{0, 0, 1, 0x41, 0x80}, h264), "non-IDR slice is not closed");
    require(packet_has_closed_restart(std::array<std::uint8_t, 6>{0, 0, 0, 2, 0x65, 0x80}, h264, 4), "AVCC IDR");
    require(!packet_has_closed_restart(std::array<std::uint8_t, 6>{0, 0, 0, 9, 0x65, 0x80}, h264, 4), "truncated NAL rejected");
    require(packet_has_closed_restart(std::array<std::uint8_t, 5>{0, 0, 1, 0x26, 1}, hevc), "HEVC IDR");
    require(!packet_has_closed_restart(std::array<std::uint8_t, 5>{0, 0, 1, 0x2a, 1}, hevc), "HEVC CRA needs leading-picture policy");
    require(!packet_has_closed_restart(std::array<std::uint8_t, 5>{0, 0, 1, 0xe5, 0}, h264), "forbidden bit rejected");
    const std::array<std::size_t, 3> cuts{4, 8, 12};
    IndexedRangeScheduler scheduler{16, cuts};
    auto first = scheduler.claim().value();
    deliver(scheduler, first.lease, 0);
    require(scheduler.reserve(first.lease, 9) == IndexedRangeScheduler::Delivery::reserved,
            "out-of-order GPU delivery can reserve later ordinal");
    require(scheduler.split_tail(first.lease, 4), "split after issued high-water mark");
    require(scheduler.range(first.lease)->end == 12, "cannot steal reserved frame");
    auto second = scheduler.claim().value();
    require(second.range.begin == 12, "only safe tail reassigned");
    require(scheduler.reserve(first.lease, 12) == IndexedRangeScheduler::Delivery::unavailable,
            "old decoder cannot deliver reassigned tail");
    scheduler.abandon_delivery(first.lease, 9);
    scheduler.release(first.lease);
    auto retry = scheduler.claim().value();
    require(retry.lease != first.lease, "stale leases cannot be reused");
    require(scheduler.reserve(first.lease, 1) == IndexedRangeScheduler::Delivery::unavailable,
            "stale owner rejected");
    require(scheduler.reserve(retry.lease, 0) == IndexedRangeScheduler::Delivery::delivered,
            "partial delivery retained across retry");
    for (std::size_t i = 1; i < 12; ++i) deliver(scheduler, retry.lease, i);
    for (std::size_t i = 12; i < 16; ++i) deliver(scheduler, second.lease, i);
    require(scheduler.complete(retry.lease) && scheduler.complete(second.lease), "complete coverage");
    require(scheduler.delivered() == 16, "exactly once delivery count");

    IndexedRangeScheduler failure{16, cuts};
    auto owner = failure.claim().value();
    require(failure.split_tail(owner.lease, 8), "second session reservation");
    auto failed = failure.claim().value();
    failure.release(failed.lease);
    require(failure.pending_ranges() == 1, "creation failure returns work");
    auto recovered = failure.claim().value();
    require(recovered.range.begin == 8, "returned work preserves interval");
    require(failure.reserve(owner.lease, 0) == IndexedRangeScheduler::Delivery::reserved, "enqueue pending");
    failure.cancel();
    failure.commit(owner.lease, 0);
    require(!failure.claim() && !failure.split_tail(owner.lease, 4), "cancel prevents new work");
    require(failure.reserve(owner.lease, 1) == IndexedRangeScheduler::Delivery::unavailable,
            "cancel prevents delivery reservations");
    failure.release(owner.lease);
    failure.release(recovered.lease);
    require(failure.pending_ranges() == 0, "cancel does not requeue");

    IndexedRangeScheduler concurrent{16, cuts};
    const auto lease = concurrent.claim()->lease;
    std::atomic<int> emitted{0};
    std::array<std::thread, 4> workers;
    for (auto &worker : workers) worker = std::thread([&] {
        for (std::size_t i = 0; i < 16; ++i) {
            if (concurrent.reserve(lease, i) == IndexedRangeScheduler::Delivery::reserved) {
                ++emitted;
                concurrent.commit(lease, i);
            }
        }
    });
    for (auto &worker : workers) worker.join();
    require(emitted == 16 && concurrent.complete(lease), "racing consumers deliver exactly once");
    IndexedRangeScheduler unsplittable{100, {}};
    require(!unsplittable.split_tail(unsplittable.claim()->lease, 50), "no RAP means no split");
    IndexedRangeScheduler empty{0, {}};
    require(!empty.claim(), "empty work does not create sessions");
    bool rejected = false;
    try { IndexedRangeScheduler invalid{16, std::array<std::size_t, 2>{4, 4}}; }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "reject duplicate safe cuts");
    std::cout << "indexed range scheduler tests passed\n";
}
