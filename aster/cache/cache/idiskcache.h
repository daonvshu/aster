#pragma once

#include "aster/cache/core/cacheinspection.h"
#include "aster/cache/core/imageresult.h"

#include <QJsonObject>

#include <atomic>

namespace aster::cache
{
struct DiskEntry
{
    QByteArray bytes;
    QJsonObject metadata;
};

struct DiskStats
{
    quint64 hits = 0;
    quint64 misses = 0;
    quint64 corruptions = 0;
    quint64 ioErrors = 0;
    quint64 evictions = 0;
    qint64 totalBytes = 0;
    qint64 maxBytes = 0;
    qint64 entryCount = 0;
    bool indexComplete = false;
    LookupLatency lookup;
};

class IDiskCache
{
public:
    virtual ~IDiskCache() = default;
    virtual Result<DiskEntry> get(const QByteArray& digest) = 0;
    virtual bool put(const QByteArray& digest, const DiskEntry&) = 0;
    virtual bool remove(const QByteArray& digest) = 0;
    virtual bool contains(const QByteArray& digest) = 0;
    virtual bool clear(const std::atomic<bool>* cancelled = nullptr) = 0;
    virtual bool trim(qint64 bytes, const std::atomic<bool>* cancelled = nullptr) = 0;
    virtual DiskStats stats() const = 0;

    virtual bool invalidate(const CacheSelector&, const std::atomic<bool>* = nullptr)
    {
        return false;
    }
};
}
