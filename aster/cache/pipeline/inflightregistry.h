#pragma once

#include "aster/cache/core/imageresult.h"
#include "aster/cache/core/imagetask.h"

#include <QHash>
#include <QSharedPointer>

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aster::cache {
template <class Key, class Value>
class InFlightRegistry {
public:
    using Completion = std::function<void(Result<Value>)>;
    using Starter = std::function<CancelAction(Completion)>;

private:
    struct Slot {
        std::atomic<bool> delivered{false};
        Completion callback;

        void deliver(const Result<Value>& result) noexcept {
            if (delivered.exchange(true))
                return;

            auto notify = std::move(callback);
            try {
                if (notify)
                    notify(result);
            } catch (...) {
            }
        }
    };

    struct Task {
        std::unordered_map<quint64, QSharedPointer<Slot>> subscribers;
        CancelAction cancel;
        bool finished = false;
        bool cancelled = false;
    };

    struct State {
        std::mutex mutex;
        QHash<Key, QSharedPointer<Task>> tasks;
        quint64 nextId = 0;
        bool closed = false;
    };

    QSharedPointer<State> state_ = QSharedPointer<State>::create();

    static void invoke(CancelAction action) noexcept {
        try {
            if (action)
                action();
        } catch (...) {
        }
    }

    static void complete(const QSharedPointer<State>& state, const Key& key, const QSharedPointer<Task>& task, Result<Value> result) {
        std::unordered_map<quint64, QSharedPointer<Slot>> subscribers;
        CancelAction retired;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (task->finished)
                return;

            task->finished = true;
            const auto it = state->tasks.find(key);
            if (it != state->tasks.end() && it.value() == task)
                state->tasks.erase(it);

            subscribers.swap(task->subscribers);
            retired = std::move(task->cancel);
        }

        for (const auto& slot : subscribers)
            slot.second->deliver(result);
        // Destroy captured handles outside the registry lock (they may re-enter).
    }

public:
    InFlightRegistry() = default;
    InFlightRegistry(const InFlightRegistry&) = delete;
    InFlightRegistry& operator=(const InFlightRegistry&) = delete;

    ~InFlightRegistry() {
        shutdown();
    }

    Subscription subscribe(const Key& key, Starter starter, Completion callback) {
        const auto state = state_;
        auto slot = QSharedPointer<Slot>::create();
        slot->callback = std::move(callback);
        QSharedPointer<Task> task;
        quint64 id = 0;
        bool start = false;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->closed) {
                auto it = state->tasks.find(key);
                if (it == state->tasks.end()) {
                    task = QSharedPointer<Task>::create();
                    state->tasks.insert(key, task);
                    start = true;
                } else {
                    task = it.value();
                }

                id = ++state->nextId;
                task->subscribers.emplace(id, slot);
            }
        }

        if (!task) {
            slot->deliver(Result<Value>::failure(ImageError::Cancelled));
            return {};
        }

        Subscription subscription([state, task, key, id, slot] {
            CancelAction cancel;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                task->subscribers.erase(id);
                if (!task->finished && task->subscribers.empty()) {
                    task->finished = true;
                    task->cancelled = true;
                    const auto it = state->tasks.find(key);
                    if (it != state->tasks.end() && it.value() == task)
                        state->tasks.erase(it);
                    cancel = std::move(task->cancel);
                }
            }

            slot->deliver(Result<Value>::failure(ImageError::Cancelled));
            invoke(std::move(cancel));
        });

        if (start) {
            auto done = [state, task, key](Result<Value> result) { complete(state, key, task, std::move(result)); };

            try {
                auto action = starter(done);
                bool cancelNow = false;

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    cancelNow = task->cancelled;
                    if (!task->finished)
                        task->cancel = std::move(action);
                }

                if (cancelNow)
                    invoke(std::move(action));
            } catch (...) {
                done(Result<Value>::failure(ImageError::ProcessingError, "Task starter threw"));
            }
        }

        return subscription;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return size_t(state_->tasks.size());
    }

    void shutdown() noexcept {
        QHash<Key, QSharedPointer<Task>> tasks;

        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed = true;
            tasks.swap(state_->tasks);
            for (auto it = tasks.begin(); it != tasks.end(); ++it) {
                it.value()->finished = true;
                it.value()->cancelled = true;
            }
        }

        for (auto it = tasks.begin(); it != tasks.end(); ++it) {
            std::unordered_map<quint64, QSharedPointer<Slot>> subscribers;
            CancelAction action;

            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                subscribers.swap(it.value()->subscribers);
                action = std::move(it.value()->cancel);
            }

            for (const auto& slot : subscribers)
                slot.second->deliver(Result<Value>::failure(ImageError::Cancelled));
            invoke(std::move(action));
        }
    }
};
} // namespace aster::cache
