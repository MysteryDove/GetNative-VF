#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(__cpp_lib_jthread)
namespace std {
namespace getnative_stop_detail {
struct state {
    std::mutex mutex;
    bool requested = false;
    std::size_t next_id = 1U;
    std::unordered_map<std::size_t, std::function<void()>> callbacks;
};
} // namespace getnative_stop_detail

class stop_token {
public:
    stop_token() noexcept = default;
    [[nodiscard]] bool stop_requested() const noexcept {
        if (!state_) return false;
        const std::scoped_lock lock(state_->mutex);
        return state_->requested;
    }
private:
    explicit stop_token(std::shared_ptr<getnative_stop_detail::state> state)
        : state_(std::move(state)) {}
    std::shared_ptr<getnative_stop_detail::state> state_;
    friend class stop_source;
    template <typename> friend class stop_callback;
};

class stop_source {
public:
    stop_source() : state_(std::make_shared<getnative_stop_detail::state>()) {}
    [[nodiscard]] stop_token get_token() const noexcept { return stop_token{state_}; }
    [[nodiscard]] bool request_stop() noexcept {
        std::vector<std::function<void()>> callbacks;
        {
            const std::scoped_lock lock(state_->mutex);
            if (state_->requested) return false;
            state_->requested = true;
            callbacks.reserve(state_->callbacks.size());
            for (auto &[id, callback] : state_->callbacks) {
                (void)id;
                callbacks.push_back(std::move(callback));
            }
            state_->callbacks.clear();
        }
        for (auto &callback : callbacks) callback();
        return true;
    }
    [[nodiscard]] bool stop_requested() const noexcept { return get_token().stop_requested(); }
private:
    std::shared_ptr<getnative_stop_detail::state> state_;
};

template <typename Callback>
class stop_callback {
public:
    stop_callback(const stop_token &token, Callback callback)
        : state_(token.state_), callback_(std::move(callback)) {
        if (!state_) return;
        bool invoke = false;
        {
            const std::scoped_lock lock(state_->mutex);
            if (state_->requested) invoke = true;
            else {
                id_ = state_->next_id++;
                state_->callbacks.emplace(id_, callback_);
            }
        }
        if (invoke) callback_();
    }
    stop_callback(const stop_callback &) = delete;
    stop_callback &operator=(const stop_callback &) = delete;
    ~stop_callback() {
        if (!state_ || id_ == 0U) return;
        const std::scoped_lock lock(state_->mutex);
        state_->callbacks.erase(id_);
    }
private:
    std::shared_ptr<getnative_stop_detail::state> state_;
    Callback callback_;
    std::size_t id_ = 0U;
};
} // namespace std
#endif
