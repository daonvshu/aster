#pragma once

#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/cache/source/httpinterceptor.h"

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
    QList<QSharedPointer<IHttpInterceptor>> httpInterceptors;
    QSharedPointer<IDiskCache> sourceDisk;
    QSharedPointer<RenderedDiskCache> renderedDisk;
    QSharedPointer<IImageSourceLoader> sourceLoader;

    /**
     * @brief Adds an HTTP interceptor. Interceptors run in registration order.
     * @param interceptor Interceptor to add. It must not be null.
     * @return This configuration for chained calls.
     */
    ImageServiceConfig& addInterceptor(QSharedPointer<IHttpInterceptor> interceptor);
};

class ImageService final {
public:
    ImageService() = delete;

    /**
     * @brief Creates an independent image pipeline without changing the global service state.
     * @param config Pipeline dependencies, cache settings, and HTTP interceptors.
     * @return Independently owned image pipeline.
     */
    static QSharedPointer<ImagePipeline> createPipeline(const ImageServiceConfig& config);

    static void configure(const ImageServiceConfig&);
    static QSharedPointer<ImagePipeline> pipeline();
    static bool isConfigured();
    static void shutdown();
};
} // namespace aster::cache
