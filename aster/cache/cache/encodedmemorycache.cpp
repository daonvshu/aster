#include "encodedmemorycache.h"

#include <QSharedPointer>

#include <algorithm>
#include <stdexcept>

namespace aster::cache {
EncodedMemoryCache::EncodedMemoryCache(qint64 budget, qint64 maxEntry, QSharedPointer<Clock> clock)
    : maxEntry_(std::max<qint64>(0, maxEntry))
    , clock_(std::move(clock)) {
    if (!clock_)
        throw std::invalid_argument("clock must not be null");
    stats_.maxBytes = std::max<qint64>(0, budget);
}

qint64 EncodedMemoryCache::costOf(const SourcePayload& payload) {
    qint64 cost = sizeof(Entry) + 64 + payload.bytes.size() + payload.contentType.size();
    for (auto it = payload.headers.cbegin(); it != payload.headers.cend(); ++it)
        cost += it.key().size() + it.value().size() + 64;
    return cost;
}

void EncodedMemoryCache::erase(Entries::iterator it) {
    stats_.totalBytes -= it->cost;
    --stats_.entryCount;
    index_.remove(it->key);
    entries_.erase(it);
}

void EncodedMemoryCache::trimLocked(qint64 target) {
    while (stats_.totalBytes > target && !entries_.empty()) {
        erase(std::prev(entries_.end()));
        ++stats_.evictions;
    }
}

Result<SourcePayload> EncodedMemoryCache::get(const SourceKey& key, bool allowStale) {
    const auto started = LookupTimer::Clock::now();
    const auto now = clock_->now();
    std::lock_guard<std::mutex> lock(mutex_);
    LookupTimer timer(stats_.lookup, started);
    auto found = index_.find(key);
    if (found == index_.end()) {
        ++stats_.misses;
        return Result<SourcePayload>::failure(ImageError::CacheMiss);
    }

    auto it = found.value();
    if (it->value.expiresAt && now >= *it->value.expiresAt && (!allowStale || it->value.mustRevalidate)) {
        erase(it);
        ++stats_.misses;
        ++stats_.expirations;
        return Result<SourcePayload>::failure(ImageError::CacheMiss);
    }

    entries_.splice(entries_.begin(), entries_, it);
    ++stats_.hits;
    return Result<SourcePayload>::success(it->value, CacheResultSource::EncodedMemory);
}

bool EncodedMemoryCache::put(const SourceKey& key, const SourcePayload& value) {
    const auto cost = costOf(value);
    std::lock_guard<std::mutex> lock(mutex_);
    auto old = index_.find(key);
    if (old != index_.end())
        erase(old.value());
    if (key.digest.size() != 32 || value.noStore || value.bytes.isEmpty() || value.bytes.size() > maxEntry_ || cost > stats_.maxBytes || stats_.maxBytes == 0) {
        ++stats_.rejections;
        return false;
    }

    SourcePayload owned = value;
    owned.bytes = QByteArray(value.bytes.constData(), value.bytes.size());
    trimLocked(stats_.maxBytes - cost);
    entries_.push_front({key, std::move(owned), cost});
    try {
        index_.insert(key, entries_.begin());
    } catch (...) {
        entries_.pop_front();
        throw;
    }
    stats_.totalBytes += cost;
    ++stats_.entryCount;
    ++stats_.puts;
    return true;
}

bool EncodedMemoryCache::remove(const SourceKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end())
        return false;
    erase(it.value());
    return true;
}

void EncodedMemoryCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.clear();
    entries_.clear();
    stats_.entryCount = stats_.totalBytes = 0;
}

void EncodedMemoryCache::trim(qint64 target) {
    std::lock_guard<std::mutex> lock(mutex_);
    trimLocked(std::max<qint64>(0, target));
}

void EncodedMemoryCache::setMaxCost(qint64 budget) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.maxBytes = std::max<qint64>(0, budget);
    trimLocked(stats_.maxBytes);
}

CacheStats EncodedMemoryCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool EncodedMemoryCache::invalidate(const CacheSelector& selector) {
    if (!selector.valid())
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto current = it++;
        if (selector.matches(current->key))
            erase(current);
    }
    return true;
}
} // namespace aster::cache
