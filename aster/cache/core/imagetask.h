#pragma once

#include <functional>
#include <utility>

namespace aster::cache {
using CancelAction = std::function<void()>;

class Subscription {
public:
    Subscription() = default;

    explicit Subscription(CancelAction cancel)
        : cancel_(std::move(cancel)) {
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : cancel_(std::move(other.cancel_)) {
        other.cancel_ = {};
    }

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            cancel();
            cancel_ = std::move(other.cancel_);
            other.cancel_ = {};
        }
        return *this;
    }

    ~Subscription() {
        cancel();
    }

    void cancel() noexcept {
        auto action = std::move(cancel_);
        cancel_ = {};
        if (action) {
            try {
                action();
            } catch (...) {
            }
        }
    }

private:
    CancelAction cancel_;
};
} // namespace aster::cache
