#pragma once

#include "aster/cache/core/clock.h"
#include "iimagememorycache.h"

#include <QHash>

#include <list>
#include <mutex>

namespace aster::cache
{
class RenderedMemoryCache final : public IImageMemoryCache
{
public:
    explicit RenderedMemoryCache(qint64 maxBytes,
                                 std::shared_ptr<Clock> clock = std::make_shared<SystemClock>());

    ImageResult get(const RenderKey&, bool allowStale = false) override;
    bool put(const RenderKey&, const QImage&, std::optional<QDateTime> expiresAt = {}) override;
    bool remove(const RenderKey&) override;
    void clear() override;

    void trim(qint64 targetBytes) override;
    void setMaxCost(qint64 bytes) override;

    CacheStats stats() const override;
    bool invalidate(const CacheSelector&) override;
    QVector<CacheDebugEntry> debugDump(int limit = 100) const override;
    static qint64 costOf(const QImage&);

private:
    struct Entry
    {
        RenderKey key;
        QImage image;
        CacheEntryMeta meta;
    };

    using Entries = std::list<Entry>;

    void erase(Entries::iterator);
    void trimLocked(qint64 targetBytes);

    std::shared_ptr<Clock> clock_;
    mutable std::mutex mutex_;
    Entries entries_;
    QHash<RenderKey, Entries::iterator> index_;
    CacheStats stats_;
};
}
