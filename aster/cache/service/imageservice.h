#pragma once

#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"

#include <QSharedPointer>

namespace aster::cache {
struct ImageServiceConfig {
    ImagePipeline::Renderer renderer;
    int workerCount = 4;
    EventSink events;
    QSharedPointer<Clock> clock = QSharedPointer<SystemClock>::create();

    qint64 renderedMemoryBytes = 0;
    qint64 encodedMemoryBytes = 0;
    qint64 maxEncodedEntryBytes = 32 * 1024 * 1024;
    qint64 activeMemoryBytes = 0;

    SourceCacheConfig sourceCache;
    QSharedPointer<INetworkService> network;
    QSharedPointer<IDiskCache> sourceDisk;
    QSharedPointer<RenderedDiskCache> renderedDisk;
    QSharedPointer<IImageSourceLoader> sourceLoader;
};

class ImageService final {
public:
    ImageService() = delete;

    static void configure(const ImageServiceConfig&);
    static QSharedPointer<ImagePipeline> pipeline();
    static bool isConfigured();
    static void shutdown();
};
} // namespace aster::cache
