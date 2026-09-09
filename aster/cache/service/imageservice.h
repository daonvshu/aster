#pragma once

#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"

namespace aster::cache
{
struct ImageServiceConfig
{
    ImagePipeline::Renderer renderer;
    int workerCount = 4;
    EventSink events;
    std::shared_ptr<Clock> clock = std::make_shared<SystemClock>();

    qint64 renderedMemoryBytes = 0;
    qint64 encodedMemoryBytes = 0;
    qint64 maxEncodedEntryBytes = 32 * 1024 * 1024;
    qint64 activeMemoryBytes = 0;

    SourceCacheConfig sourceCache;
    std::shared_ptr<INetworkService> network;
    std::shared_ptr<IDiskCache> sourceDisk;
    std::shared_ptr<RenderedDiskCache> renderedDisk;
    std::shared_ptr<IImageSourceLoader> sourceLoader;
};

class ImageService final
{
public:
    ImageService() = delete;

    static void configure(const ImageServiceConfig&);
    static std::shared_ptr<ImagePipeline> pipeline();
    static bool isConfigured();
    static void shutdown();
};
}
