#include "renderedmemorycache.h"

#include <QSharedPointer>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aster::cache
{
RenderedMemoryCache::RenderedMemoryCache(qint64 maxBytes, QSharedPointer<Clock> clock)
    : clock_(std::move(clock))
{
    if (!clock_)
        throw std::invalid_argument("clock must not be null");

    stats_.maxBytes = std::max<qint64>(0, maxBytes);
}

qint64 RenderedMemoryCache::costOf(const QImage& image)
{
    if (image.isNull())
        return 0;

    const auto bytes = static_cast<quint64>(image.sizeInBytes());
    constexpr qint64 overhead = sizeof(Entry) + 64 + 4 * sizeof(void*);

    if (bytes > quint64(std::numeric_limits<qint64>::max() - overhead))
        return std::numeric_limits<qint64>::max();

    return qint64(bytes) + overhead;
}

void RenderedMemoryCache::erase(Entries::iterator it)
{
    stats_.totalBytes -= it->meta.byteSize;
    index_.remove(it->key);
    entries_.erase(it);
    --stats_.entryCount;
}

void RenderedMemoryCache::trimLocked(qint64 target)
{
    while (stats_.totalBytes > target && !entries_.empty())
    {
        erase(std::prev(entries_.end()));
        ++stats_.evictions;
    }
}

ImageResult RenderedMemoryCache::get(const RenderKey& key, bool allowStale)
{
    const auto started = LookupTimer::Clock::now();
    const auto now = clock_->now();
    std::lock_guard<std::mutex> lock(mutex_);
    LookupTimer timer(stats_.lookup, started);
    const auto found = index_.find(key);

    if (found == index_.end())
    {
        ++stats_.misses;
        return ImageResult::failure(ImageError::CacheMiss);
    }

    const auto it = found.value();
    if (!allowStale && it->meta.expiresAt && now >= *it->meta.expiresAt)
    {
        erase(it);
        ++stats_.expirations;
        ++stats_.misses;
        return ImageResult::failure(ImageError::CacheMiss);
    }

    entries_.splice(entries_.begin(), entries_, it);
    it->meta.lastAccessAt = now;
    ++stats_.hits;

    return ImageResult::success(it->image, CacheResultSource::RenderedMemory);
}

bool RenderedMemoryCache::put(const RenderKey& key, const QImage& image,
                              std::optional<QDateTime> expires)
{
    // Own pixels even when the caller constructed QImage over external storage.
    const auto cost = costOf(image);
    const auto now = clock_->now();
    if (key.digest.size() != 32 || key.source.digest.size() != 32 || cost == 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.rejections;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto old = index_.find(key);
    if (old != index_.end())
        erase(old.value());

    if (stats_.maxBytes == 0 || cost > stats_.maxBytes)
    {
        ++stats_.rejections;
        return false;
    }

    QImage owned = image.copy();
    if (owned.isNull())
    {
        ++stats_.rejections;
        return false;
    }

    trimLocked(stats_.maxBytes - cost);
    entries_.push_front({key, std::move(owned), {1, cost, now, now, std::move(expires)}});
    try
    {
        index_.insert(key, entries_.begin());
    }
    catch (...)
    {
        entries_.pop_front();
        throw;
    }

    stats_.totalBytes += cost;
    ++stats_.entryCount;
    ++stats_.puts;

    return true;
}

bool RenderedMemoryCache::remove(const RenderKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end())
        return false;

    erase(it.value());

    return true;
}

void RenderedMemoryCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    index_.clear();
    entries_.clear();
    stats_.entryCount = 0;
    stats_.totalBytes = 0;
}

void RenderedMemoryCache::trim(qint64 bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    trimLocked(std::max<qint64>(0, bytes));
}

void RenderedMemoryCache::setMaxCost(qint64 bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.maxBytes = std::max<qint64>(0, bytes);
    trimLocked(stats_.maxBytes);
}

CacheStats RenderedMemoryCache::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool RenderedMemoryCache::invalidate(const CacheSelector& selector)
{
    if (!selector.valid())
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        auto current = it++;
        if (selector.matches(current->key.source))
            erase(current);
    }
    return true;
}

QVector<CacheDebugEntry> RenderedMemoryCache::debugDump(int limit) const
{
    const auto now = clock_->now();
    std::lock_guard<std::mutex> lock(mutex_);
    QVector<CacheDebugEntry> result;
    for (const auto& entry : entries_)
    {
        if (result.size() >= std::max(0, limit))
            break;

        result.push_back({entry.key.digest, entry.key.source.digest,
                          entry.key.source.namespaceDigest, entry.meta.byteSize,
                          std::max<qint64>(0, entry.meta.createdAt.msecsTo(now))});
    }
    return result;
}
}
