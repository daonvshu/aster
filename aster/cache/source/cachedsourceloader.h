#pragma once

#include "aster/cache/cache/encodedmemorycache.h"
#include "aster/cache/cache/idiskcache.h"
#include "networkservice.h"

#include <QSharedPointer>

namespace aster::cache {
struct SourceCacheConfig {
    bool encodedNetwork = true;
    bool encodedLocal = false;
    bool encodedData = false;
    QList<QByteArray> excludedMimeTypes;
    bool encodedResource = true;
};

class CachedSourceLoader final : public IImageSourceLoader {
public:
    CachedSourceLoader(QSharedPointer<EncodedMemoryCache> encoded, QSharedPointer<IDiskCache> disk = {}, QSharedPointer<INetworkService> network = {},
                       QSharedPointer<Clock> clock = QSharedPointer<SystemClock>::create(), SourceCacheConfig config = {});
    Result<SourceKey> key(const ImageSource&) const override;
    void trimMemory(bool critical) override;
    SourceCacheStats cacheStats() const override;
    bool invalidate(const CacheSelector&) override;
    bool clearDiskCache(const std::atomic<bool>* cancelled = nullptr) override;
    Result<SourcePayload> load(const ImageSource&, const SourceKey&, const SourceLoadOptions&, const std::atomic<bool>&) override;

private:
    Result<SourcePayload> network(const ImageSource&, const SourceKey&, const SourceLoadOptions&, const std::atomic<bool>&);
    bool encodedEnabled(ImageSource::Kind) const;
    QSharedPointer<EncodedMemoryCache> encoded_;
    QSharedPointer<IDiskCache> disk_;
    QSharedPointer<INetworkService> network_;
    QSharedPointer<Clock> clock_;
    SourceCacheConfig config_;
    mutable std::mutex metricsMutex_;
    NetworkStats networkStats_;
    std::atomic<quint64> diskExceptions_{0};
};
} // namespace aster::cache
