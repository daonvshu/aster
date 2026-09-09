#pragma once

#include "aster/cache/core/clock.h"
#include "aster/cache/source/iimagesourceloader.h"
#include "iimagememorycache.h"

#include <QHash>

#include <list>
#include <mutex>

namespace aster::cache
{
class EncodedMemoryCache
{
public:
    EncodedMemoryCache(qint64 budget, qint64 maxEntry,
                       std::shared_ptr<Clock> clock = std::make_shared<SystemClock>());
    Result<SourcePayload> get(const SourceKey&, bool allowStale = false);
    bool put(const SourceKey&, const SourcePayload&);
    bool remove(const SourceKey&);
    void clear();
    void trim(qint64);
    void setMaxCost(qint64);
    CacheStats stats() const;
    bool invalidate(const CacheSelector&);
    static qint64 costOf(const SourcePayload&);

private:
    struct Entry
    {
        SourceKey key;
        SourcePayload value;
        qint64 cost;
    };

    using Entries = std::list<Entry>;
    void erase(Entries::iterator);
    void trimLocked(qint64);
    mutable std::mutex mutex_;
    Entries entries_;
    QHash<SourceKey, Entries::iterator> index_;
    CacheStats stats_;
    qint64 maxEntry_;
    std::shared_ptr<Clock> clock_;
};
}
