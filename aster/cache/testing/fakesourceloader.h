#pragma once

#include "aster/cache/source/sourcetask.h"

#include <QSharedPointer>

#include <atomic>
#include <mutex>
#include <vector>

namespace aster::cache::testing {
class FakeSourceLoader {
    struct State {
        mutable std::mutex mutex;
        std::vector<SourceCompletion> pending;
        std::atomic<int> loads{0}, cancels{0};
    };

    QSharedPointer<State> state_ = QSharedPointer<State>::create();

public:
    SourceTask task() const {
        return [state = state_](const SourceKey&, SourceCompletion done) -> CancelAction {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->pending.push_back(std::move(done));
            }
            ++state->loads;
            auto cancelled = QSharedPointer<std::atomic<bool>>::create(false);
            return [state, cancelled] {
                if (!cancelled->exchange(true))
                    ++state->cancels;
            };
        };
    }

    int loadCount() const {
        return state_->loads.load();
    }

    int cancelCount() const {
        return state_->cancels.load();
    }

    std::vector<SourceCompletion> takePending() {
        std::vector<SourceCompletion> pending;
        std::lock_guard<std::mutex> lock(state_->mutex);
        pending.swap(state_->pending);
        return pending;
    }

    void completeAll(Result<QByteArray> result = Result<QByteArray>::success("fake encoded bytes")) {
        for (auto& done : takePending())
            done(result);
    }
};

using FakeNetwork = FakeSourceLoader;
} // namespace aster::cache::testing
