#pragma once

#include "aster/cache/core/cacheinspection.h"
#include "aster/cache/core/imagekey.h"

#include <QDateTime>

#include <optional>

namespace aster::cache
{
struct CacheEntryMeta
{
    quint32 schemaVersion = 1;
    qint64 byteSize = 0;
    QDateTime createdAt;
    QDateTime lastAccessAt;
    std::optional<QDateTime> expiresAt;
};

struct CacheStats
{
    quint64 hits = 0;
    quint64 misses = 0;
    quint64 puts = 0;
    quint64 rejections = 0;
    quint64 evictions = 0;
    quint64 expirations = 0;

    qint64 totalBytes = 0;
    qint64 maxBytes = 0;
    qint64 entryCount = 0;
    LookupLatency lookup;
};

class IImageMemoryCache
{
public:
    virtual ~IImageMemoryCache() = default;

    virtual ImageResult get(const RenderKey&, bool allowStale = false) = 0;
    virtual bool put(const RenderKey&, const QImage&, std::optional<QDateTime> expiresAt = {}) = 0;
    virtual bool remove(const RenderKey&) = 0;
    virtual void clear() = 0;

    virtual void trim(qint64 targetBytes) = 0;
    virtual void setMaxCost(qint64 bytes) = 0;

    virtual CacheStats stats() const = 0;

    virtual bool invalidate(const CacheSelector&)
    {
        return false;
    }

    virtual QVector<CacheDebugEntry> debugDump(int = 100) const
    {
        return {};
    }
};
}
