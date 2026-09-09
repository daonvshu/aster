#pragma once

#include "aster/cache/core/cacheinspection.h"
#include "aster/cache/core/clock.h"
#include "aster/cache/core/imagehandle.h"
#include "aster/cache/core/imagekey.h"

#include <QHash>

#include <memory>
#include <mutex>

namespace aster::cache
{
struct ActiveResourceStats
{
    qint64 bytes = 0;
    qint64 maxBytes = 0;
    qint64 entries = 0;
    quint64 rejected = 0;
    qint64 oldestAgeMs = 0;
    quint64 hits = 0;
    quint64 misses = 0;
};

class ActiveResourceStore
{
public:
    explicit ActiveResourceStore(qint64 maxBytes,
                                 std::shared_ptr<Clock> clock = std::make_shared<SystemClock>());
    ImageHandle acquire(const RenderKey&, const QImage&);
    ImageHandle find(const RenderKey&);
    void setMaxCost(qint64);
    ActiveResourceStats stats() const;
    bool invalidate(const CacheSelector&);

private:
    struct Entry
    {
        std::weak_ptr<const QImage> image;
        qint64 cost;
        quint64 generation;
        qint64 createdMs;
    };

    struct State
    {
        mutable std::recursive_mutex mutex;
        QHash<RenderKey, Entry> entries;
        ActiveResourceStats stats;
        quint64 generation = 0;
    };

    std::shared_ptr<State> state_;
    std::shared_ptr<Clock> clock_;
};
}
