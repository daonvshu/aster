#pragma once

#include "aster/cache/core/imagekey.h"
#include "idiskcache.h"

#include <QSharedPointer>

#include <mutex>

namespace aster::cache
{
struct RenderedDiskConfig
{
    QByteArray format = "png";
    int quality = -1;
    quint32 encoderVersion = 1;
    qint64 maxPixels = 16 * 1024 * 1024;
    int maxSide = 16384;
    qint64 maxDecodedBytes = 256 * 1024 * 1024;
    qint64 maxEncodedBytes = 32 * 1024 * 1024;
};

class RenderedDiskCache
{
public:
    explicit RenderedDiskCache(QSharedPointer<IDiskCache>, RenderedDiskConfig = {});
    ImageResult get(const RenderKey&);
    bool put(const RenderKey&, const QImage&);
    bool remove(const RenderKey&);
    QByteArray storageKey(const RenderKey&) const;
    DiskStats stats() const;
    bool invalidate(const CacheSelector&);

private:
    ImageResult read(const RenderKey&);
    QSharedPointer<IDiskCache> disk_;
    RenderedDiskConfig config_;
    mutable std::mutex metricsMutex_;
    DiskStats metrics_;
};
}
