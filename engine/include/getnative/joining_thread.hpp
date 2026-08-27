#pragma once

#include <thread>
#include <utility>

namespace getnative {

// C++20 std::jthread is unavailable in the libc++ shipped with some
// supported Xcode releases. This small owner preserves its join-on-destruction
// behavior while keeping the engine portable across those standard libraries.
class JoiningThread {
public:
    JoiningThread() noexcept = default;

    template <typename Function, typename... Arguments>
    explicit JoiningThread(Function &&function, Arguments &&...arguments)
        : thread_(std::forward<Function>(function),
                  std::forward<Arguments>(arguments)...) {}

    JoiningThread(JoiningThread &&) noexcept = default;
    JoiningThread &operator=(JoiningThread &&other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) thread_.join();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    JoiningThread(const JoiningThread &) = delete;
    JoiningThread &operator=(const JoiningThread &) = delete;

    ~JoiningThread() {
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
    void join() { thread_.join(); }

private:
    std::thread thread_;
};

} // namespace getnative
