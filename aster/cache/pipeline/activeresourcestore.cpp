#include "activeresourcestore.h"

#include <algorithm>
#include <stdexcept>

namespace aster::cache
{
ActiveResourceStore::ActiveResourceStore(qint64 budget, std::shared_ptr<Clock> clock)
    : state_(std::make_shared<State>()), clock_(std::move(clock))
{
    if (!clock_)
        throw std::invalid_argument("clock must not be null");
    state_->stats.maxBytes = std::max<qint64>(0, budget);
}

ImageHandle ActiveResourceStore::find(const RenderKey& key)
{
    std::lock_guard<std::recursive_mutex> lock(state_->mutex);
    auto it = state_->entries.constFind(key);
    auto handle = it == state_->entries.cend() ? ImageHandle{} : ImageHandle(it->image.lock());
    if (handle)
        ++state_->stats.hits;
    else
        ++state_->stats.misses;
    return handle;
}

ImageHandle ActiveResourceStore::acquire(const RenderKey& key, const QImage& image)
{
    if (image.isNull() || key.digest.size() != 32 || key.source.digest.size() != 32)
        return {};
    const auto state = state_;
    const auto created = clock_->now().toMSecsSinceEpoch();
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    auto old = state->entries.find(key);
    if (old != state->entries.end())
    {
        if (auto existing = old->image.lock())
            return ImageHandle(std::move(existing));
        state->stats.bytes -= old->cost;
        state->entries.erase(old);
    }

    const qint64 cost = qint64(image.sizeInBytes()) + sizeof(QImage) + 128;
    if (cost > state->stats.maxBytes - state->stats.bytes)
    {
        ++state->stats.rejected;
        state->stats.entries = state->entries.size();
        return ImageHandle(std::make_shared<const QImage>(image));
    }

    const auto generation = ++state->generation;
    // Copy the lightweight Qt image handle, never copy its pixel allocation.
    auto value = std::shared_ptr<const QImage>(
        new QImage(image),
        [weak = std::weak_ptr<State>(state), key, generation](const QImage* ptr)
        {
            delete ptr;
            if (auto current = weak.lock())
            {
                std::lock_guard<std::recursive_mutex> guard(current->mutex);
                auto it = current->entries.find(key);
                if (it != current->entries.end() && it->generation == generation)
                {
                    current->stats.bytes -= it->cost;
                    current->entries.erase(it);
                    current->stats.entries = current->entries.size();
                }
            }
        });
    state->entries.insert(key, {value, cost, generation, created});
    state->stats.bytes += cost;
    state->stats.entries = state->entries.size();
    return ImageHandle(std::move(value));
}

void ActiveResourceStore::setMaxCost(qint64 budget)
{
    std::lock_guard<std::recursive_mutex> lock(state_->mutex);
    state_->stats.maxBytes = std::max<qint64>(0, budget);
    while (state_->stats.bytes > state_->stats.maxBytes && !state_->entries.isEmpty())
    {
        auto it = state_->entries.begin();
        state_->stats.bytes -= it->cost;
        state_->entries.erase(it);
    }
    state_->stats.entries = state_->entries.size();
}

ActiveResourceStats ActiveResourceStore::stats() const
{
    const auto now = clock_->now().toMSecsSinceEpoch();
    std::lock_guard<std::recursive_mutex> lock(state_->mutex);
    auto stats = state_->stats;
    for (const auto& entry : state_->entries)
        stats.oldestAgeMs = std::max(stats.oldestAgeMs, now - entry.createdMs);
    return stats;
}

bool ActiveResourceStore::invalidate(const CacheSelector& selector)
{
    if (!selector.valid())
        return false;

    std::lock_guard<std::recursive_mutex> lock(state_->mutex);
    for (auto it = state_->entries.begin(); it != state_->entries.end();)
    {
        if (selector.matches(it.key().source))
        {
            state_->stats.bytes -= it->cost;
            it = state_->entries.erase(it);
        }
        else
            ++it;
    }
    state_->stats.entries = state_->entries.size();
    return true;
}
}
