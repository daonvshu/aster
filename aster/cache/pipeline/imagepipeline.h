#pragma once

#include "activeresourcestore.h"
#include "aster/cache/cache/iimagememorycache.h"
#include "aster/cache/cache/rendereddiskcache.h"
#include "aster/cache/core/imagerequest.h"
#include "aster/cache/source/sourcetask.h"
#include "imagesubscription.h"
#include "inflightregistry.h"
#include "sourcerequest.h"

#include <QSharedPointer>

namespace aster::cache
{
struct PipelineEvent
{
    enum class Kind
    {
        MemoryHit,
        SourceStarted,
        RenderStarted,
        Completed,
        SourceCompleted
    };
    Kind kind;
    QByteArray keyDigest;
    ImageError error = ImageError::None;
    CacheResultSource source = CacheResultSource::Unknown;
};

using EventSink = std::function<void(const PipelineEvent&)>;

struct PipelineStats
{
    size_t sourceInFlight = 0;
    size_t renderInFlight = 0;
};

struct PipelineResources
{
    QSharedPointer<RenderedDiskCache> renderedDisk;
    QSharedPointer<ActiveResourceStore> active;
};

struct ImageCacheStats
{
    CacheStats renderedMemory;
    CacheStats encodedMemory;
    DiskStats renderedDisk;
    DiskStats rawDisk;
    NetworkStats network;
    ActiveResourceStats active;
};

enum class MemoryPressure
{
    Background,
    Low,
    Critical
};

class ImagePipeline
{
public:
    using Renderer = std::function<ImageResult(const QByteArray&, const RenderOptions&,
                                               const std::atomic<bool>&)>;
    using Completion = std::function<void(ImageResult)>;

    ImagePipeline(QSharedPointer<IImageMemoryCache>, SourceTask, Renderer, EventSink = {});
    ImagePipeline(QSharedPointer<IImageMemoryCache>, QSharedPointer<IImageSourceLoader>, Renderer,
                  int workerCount = 4, EventSink = {}, PipelineResources = {});
    ~ImagePipeline();

    ImagePipeline(const ImagePipeline&) = delete;
    ImagePipeline& operator=(const ImagePipeline&) = delete;

    Subscription request(const ImageRequest&, Completion);
    Subscription request(const SourceRequest&, Completion);
    Subscription request(const QString& source, const RenderOptions&, Completion);

    ImageSubscription* request(const SourceRequest&, QObject* parent = nullptr);
    ImageSubscription* request(const QString& source, const RenderOptions&,
                               QObject* parent = nullptr);
    PipelineStats stats() const;
    void trimMemory(MemoryPressure);
    bool waitForIdle(int timeoutMs = 30000);
    ImageCacheStats cacheStats() const;
    QVector<CacheDebugEntry> debugDump(int limit = 100) const;
    bool removeSource(const SourceKey&);
    bool removeRenderVariants(const SourceKey&);
    bool clearNamespace(const QByteArray& nameSpace);

private:
    bool invalidate(const CacheSelector&, bool includeSource);
    struct State;
    QSharedPointer<State> state_;
};
}
