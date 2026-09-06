#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace getnative::media {

struct DecodeRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Selected-frame ordinals are independent of PTS, decode order and source-frame
// gaps. Safe cuts are supplied by the codec/index planner, never inferred here.
class IndexedRangeScheduler {
public:
    using Lease = std::uint64_t;
    enum class Delivery { unavailable, reserved, delivered };

    IndexedRangeScheduler(std::size_t selected_count, std::span<const std::size_t> safe_cuts)
        : states_(selected_count, State::pending), safe_cuts_(safe_cuts.begin(), safe_cuts.end()) {
        if (!std::is_sorted(safe_cuts_.begin(), safe_cuts_.end())
            || std::adjacent_find(safe_cuts_.begin(), safe_cuts_.end()) != safe_cuts_.end()
            || (!safe_cuts_.empty() && (safe_cuts_.front() == 0 || safe_cuts_.back() >= selected_count))) {
            throw std::invalid_argument("decode safe cuts must be unique interior ordinals");
        }
        if (selected_count) pending_.push_back({0, selected_count});
    }

    struct Assignment { Lease lease; DecodeRange range; };

    [[nodiscard]] std::optional<Assignment> claim() {
        const std::scoped_lock lock(mutex_);
        if (cancelled_ || pending_.empty()) return std::nullopt;
        const auto range = pending_.front();
        pending_.pop_front();
        const auto lease = next_lease_++;
        active_.emplace(lease, range);
        std::size_t count = 0;
        for (std::size_t i = range.begin; i < range.end; ++i) if (states_[i] == State::delivered) ++count;
        delivered_by_lease_.emplace(lease, count);
        return Assignment{lease, range};
    }

    // Split only beyond every already reserved or delivered frame. The current
    // owner may finish already-submitted preroll, but cannot deliver the tail.
    [[nodiscard]] bool split_tail(Lease owner, std::size_t preferred_cut,
                                  std::size_t minimum_tail = 1, bool exact_cut = false) {
        const std::scoped_lock lock(mutex_);
        if (cancelled_) return false;
        auto found = active_.find(owner);
        if (found == active_.end()) return false;
        auto &range = found->second;
        std::size_t first_unissued = range.begin;
        for (std::size_t i = range.begin; i < range.end; ++i) {
            if (states_[i] != State::pending) first_unissued = i + 1;
        }
        const auto cut = std::lower_bound(safe_cuts_.begin(), safe_cuts_.end(),
                                         std::max({first_unissued, preferred_cut, range.begin + 1}));
        if (cut == safe_cuts_.end() || *cut >= range.end || range.end - *cut < minimum_tail) return false;
        if (exact_cut && *cut != preferred_cut) return false;
        pending_.push_back({*cut, range.end});
        range.end = *cut;
        return true;
    }

    [[nodiscard]] std::optional<DecodeRange> range(Lease lease) const {
        const std::scoped_lock lock(mutex_);
        const auto found = active_.find(lease);
        if (found == active_.end()) return std::nullopt;
        return found->second;
    }

    // Reserve before invoking the consumer and commit only after successful
    // enqueue. A lease cannot be recycled while a consumer call is outstanding.
    [[nodiscard]] Delivery reserve(Lease lease, std::size_t ordinal) {
        const std::scoped_lock lock(mutex_);
        const auto found = active_.find(lease);
        if (cancelled_ || found == active_.end() || ordinal < found->second.begin
            || ordinal >= found->second.end) return Delivery::unavailable;
        if (states_[ordinal] == State::delivered) return Delivery::delivered;
        if (states_[ordinal] != State::pending) return Delivery::unavailable;
        states_[ordinal] = State::reserved;
        return Delivery::reserved;
    }

    void commit(Lease lease, std::size_t ordinal) {
        const std::scoped_lock lock(mutex_);
        check_reserved(lease, ordinal);
        states_[ordinal] = State::delivered;
        ++delivered_;
        ++delivered_by_lease_.at(lease);
    }

    void abandon_delivery(Lease lease, std::size_t ordinal) {
        const std::scoped_lock lock(mutex_);
        check_reserved(lease, ordinal);
        states_[ordinal] = State::pending;
    }

    [[nodiscard]] bool complete(Lease lease) {
        const std::scoped_lock lock(mutex_);
        const auto found = active_.find(lease);
        if (found == active_.end()) return false;
        for (std::size_t i = found->second.begin; i < found->second.end; ++i) {
            if (states_[i] != State::delivered) return false;
        }
        active_.erase(found);
        delivered_by_lease_.erase(lease);
        return true;
    }

    [[nodiscard]] bool finished(Lease lease) const {
        const std::scoped_lock lock(mutex_);
        const auto found = active_.find(lease);
        return found != active_.end()
            && delivered_by_lease_.at(lease) == found->second.end - found->second.begin;
    }

    [[nodiscard]] std::vector<Assignment> active_ranges() const {
        const std::scoped_lock lock(mutex_);
        std::vector<Assignment> result;
        for (const auto &[lease, range] : active_) result.push_back({lease, range});
        return result;
    }

    // Requeue the original decode interval, preserving the delivery bitmap.
    // Preroll can revisit delivered frames; reserve() suppresses their output.
    void release(Lease lease) {
        const std::scoped_lock lock(mutex_);
        const auto found = active_.find(lease);
        if (found == active_.end()) return;
        for (std::size_t i = found->second.begin; i < found->second.end; ++i) {
            if (states_[i] == State::reserved) throw std::logic_error("decode lease has an outstanding consumer");
        }
        if (!cancelled_) pending_.push_front(found->second);
        active_.erase(found);
        delivered_by_lease_.erase(lease);
    }

    void cancel() {
        const std::scoped_lock lock(mutex_);
        cancelled_ = true;
        pending_.clear();
    }

    [[nodiscard]] std::size_t delivered() const {
        const std::scoped_lock lock(mutex_);
        return delivered_;
    }

    [[nodiscard]] std::size_t pending_ranges() const {
        const std::scoped_lock lock(mutex_);
        return pending_.size();
    }

private:
    enum class State : std::uint8_t { pending, reserved, delivered };
    mutable std::mutex mutex_;
    std::vector<State> states_;
    std::vector<std::size_t> safe_cuts_;
    std::deque<DecodeRange> pending_;
    std::map<Lease, DecodeRange> active_;
    std::map<Lease, std::size_t> delivered_by_lease_;
    Lease next_lease_ = 1;
    std::size_t delivered_ = 0;
    bool cancelled_ = false;

    void check_reserved(Lease lease, std::size_t ordinal) const {
        const auto found = active_.find(lease);
        if (found == active_.end() || ordinal < found->second.begin || ordinal >= found->second.end
            || states_[ordinal] != State::reserved) throw std::logic_error("invalid decode delivery reservation");
    }
};

} // namespace getnative::media
