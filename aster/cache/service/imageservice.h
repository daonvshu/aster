#pragma once

#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/pipeline/pipelineinterceptor.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/cache/source/httpinterceptor.h"
#include "aster/cache/source/sourceinterceptor.h"

#include <QSharedPointer>
#include <QString>

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

    bool enableDefaultDiskCache = true;
    QString diskCacheDirectory;
    qint64 sourceDiskBytes = 256 * 1024 * 1024;
    qint64 renderedDiskBytes = 256 * 1024 * 1024;
    qint64 maxDiskEntryBytes = 32 * 1024 * 1024;

    SourceCacheConfig sourceCache;
    QSharedPointer<INetworkService> network;
    QList<QSharedPointer<IHttpInterceptor>> httpInterceptors;
    QList<QSharedPointer<ISourceInterceptor>> sourceInterceptors;
    QList<QSharedPointer<IPipelineInterceptor>> pipelineInterceptors;
    QVector<QSharedPointer<IImageDecoder>> decoders;
    QSharedPointer<IDiskCache> sourceDisk;
    QSharedPointer<RenderedDiskCache> renderedDisk;
    QSharedPointer<IImageSourceLoader> sourceLoader;

    /**
     * @brief Adds an HTTP interceptor. Interceptors run in registration order.
     * @param interceptor Interceptor to add. It must not be null.
     * @return This configuration for chained calls.
     */
    ImageServiceConfig& addInterceptor(QSharedPointer<IHttpInterceptor> interceptor);

    /**
     * @brief Adds a source interceptor. Interceptors run after source cache lookup and in registration order.
     * @param interceptor Interceptor to add. It must not be null.
     * @return This configuration for chained calls.
     */
    ImageServiceConfig& addInterceptor(QSharedPointer<ISourceInterceptor> interceptor);

    /**
     * @brief Adds a complete-pipeline interceptor. Interceptors run outside request merging and caches.
     * @param interceptor Interceptor to add. It must not be null.
     * @return This configuration for chained calls.
     */
    ImageServiceConfig& addInterceptor(QSharedPointer<IPipelineInterceptor> interceptor);

    /**
     * @brief Adds a custom image decoder. Decoders match in registration order before the Qt decoder fallback.
     * @param decoder Decoder to add. It must not be null.
     * @return This configuration for chained calls.
     */
    ImageServiceConfig& addDecoder(QSharedPointer<IImageDecoder> decoder);
};

class ImageService final {
public:
    ImageService() = delete;

    /**
     * @brief Creates an independent image pipeline without changing the global service state.
     * @param config Pipeline dependencies, cache settings, and interceptors.
     * @return Independently owned image pipeline.
     */
    static QSharedPointer<ImagePipeline> createPipeline(const ImageServiceConfig& config);

    static void configure(const ImageServiceConfig&);
    static QSharedPointer<ImagePipeline> pipeline();
    static bool isConfigured();
    static void shutdown();
};
} // namespace aster::cache
