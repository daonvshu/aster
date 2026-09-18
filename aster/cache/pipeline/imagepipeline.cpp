#include "imagepipeline.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QPointer>
#include <QRunnable>
#include <QSharedPointer>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <stdexcept>
#include <thread>

namespace aster::cache {
namespace {
struct RenderTaskKey {
    RenderKey render;
    CacheReadPolicy read;
    CacheWritePolicy write;
    bool allowStale;

    bool operator==(const RenderTaskKey& other) const {
        return render == other.render && read == other.read && write == other.write && allowStale == other.allowStale;
    }
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
size_t qHash(const RenderTaskKey& key, size_t seed = 0) {
    return aster::cache::qHash(key.render, seed);
}
#else
uint qHash(const RenderTaskKey& key, uint seed = 0) {
    return aster::cache::qHash(key.render, seed);
}
#endif

bool readMemory(CacheReadPolicy p) {
    return p == CacheReadPolicy::Default || p == CacheReadPolicy::BypassDisk || p == CacheReadPolicy::CacheOnly;
}

bool writeMemory(CacheWritePolicy p) {
    return p == CacheWritePolicy::Default || p == CacheWritePolicy::MemoryOnly;
}

void deliver(const ImagePipeline::Completion& callback, ImageResult result) noexcept {
    try {
        if (callback)
            callback(std::move(result));
    } catch (...) {
    }
}
} // namespace

struct ImagePipeline::State {
    QSharedPointer<IImageMemoryCache> cache;
    SourceTask source;
    Renderer renderer;
    EventSink sink;
    InFlightRegistry<SourceLoadKey, QByteArray> sources;
    InFlightRegistry<RenderTaskKey, QImage> renders;
    QSharedPointer<IImageSourceLoader> loader;
    QSharedPointer<QThreadPool> workers;
    PipelineResources resources;
    QList<QSharedPointer<IPipelineInterceptor>> interceptors;
    QVector<QSharedPointer<IImageDecoder>> decoders;
    InFlightRegistry<QByteArray, SourcePayload> payloads;
    InFlightRegistry<QByteArray, QImage> loadedRenders;
    std::atomic<bool> accepting{true};

    void event(PipelineEvent::Kind kind, const QByteArray& digest, ImageError error = ImageError::None,
               CacheResultSource origin = CacheResultSource::Unknown) const noexcept {
        try {
            if (sink)
                sink({kind, digest, error, origin});
        } catch (...) {
        }
    }
};

ImagePipeline::ImagePipeline(QSharedPointer<IImageMemoryCache> cache, SourceTask source, Renderer renderer, EventSink sink)
    : state_(QSharedPointer<State>::create()) {
    if (!cache || !source || !renderer)
        throw std::invalid_argument("Pipeline dependencies must not be empty");

    state_->cache = std::move(cache);
    state_->source = std::move(source);
    state_->renderer = std::move(renderer);
    state_->sink = std::move(sink);
}

ImagePipeline::~ImagePipeline() {
    state_->accepting.store(false);
    state_->loadedRenders.shutdown();
    state_->payloads.shutdown();
    state_->renders.shutdown();
    state_->sources.shutdown();
    // Completion can destroy its pipeline on a worker. Reap the pool off-worker.
    auto pool = std::move(state_->workers);
    if (pool)
        std::thread([pool] { pool->waitForDone(); }).detach();
}

Subscription ImagePipeline::request(const ImageRequest& request, Completion callback) {
    const auto state = state_;
    if (!state->source) {
        deliver(callback, ImageResult::failure(ImageError::InvalidRequest));
        return {};
    }
    auto keyResult = KeyBuilder().render(request.source, request.render);
    if (!keyResult) {
        deliver(callback, ImageResult::failure(keyResult.error));
        return {};
    }

    const auto key = *keyResult.value;
    if (readMemory(request.cache.read)) {
        auto cached = state->cache->get(key, request.cache.allowStale);
        if (cached) {
            state->event(PipelineEvent::Kind::MemoryHit, key.digest);
            deliver(callback, std::move(cached));
            return {};
        }
    }

    if (request.cache.read == CacheReadPolicy::CacheOnly) {
        deliver(callback, ImageResult::failure(ImageError::CacheMiss));
        return {};
    }

    RenderTaskKey taskKey{key, request.cache.read, request.cache.write, request.cache.allowStale};
    return state->renders.subscribe(
            taskKey,
            [state, key, request](auto done) -> CancelAction {
                if (readMemory(request.cache.read)) {
                    auto cached = state->cache->get(key, request.cache.allowStale);
                    if (cached) {
                        done(std::move(cached));
                        return {};
                    }
                }

                auto cancelled = QSharedPointer<std::atomic<bool>>::create(false);
                auto sourceSubscription = QSharedPointer<Subscription>::create();
                *sourceSubscription = state->sources.subscribe(
                        SourceLoadKey{request.source},
                        [state, request](SourceCompletion loaded) {
                            state->event(PipelineEvent::Kind::SourceStarted, request.source.digest);
                            return state->source(request.source, std::move(loaded));
                        },
                        [state, key, request, done, cancelled](Result<QByteArray> bytes) {
                            if (cancelled->load()) {
                                done(ImageResult::failure(ImageError::Cancelled));
                                return;
                            }

                            if (!bytes) {
                                done(ImageResult::failure(bytes.error, bytes.message));
                                return;
                            }

                            ImageResult result;
                            try {
                                state->event(PipelineEvent::Kind::RenderStarted, key.digest);
                                result = state->renderer(*bytes.value, request.render, *cancelled);
                                if (cancelled->load()) {
                                    done(ImageResult::failure(ImageError::Cancelled));
                                    return;
                                }

                                if (result && result.value->isNull())
                                    result = ImageResult::failure(ImageError::ProcessingError);

                                if (result) {
                                    result.value->setDevicePixelRatio(request.render.dpr);
                                    result.source = CacheResultSource::Loaded;
                                    if (writeMemory(request.cache.write))
                                        state->cache->put(key, *result.value);
                                }
                            } catch (...) {
                                result = ImageResult::failure(ImageError::ProcessingError, "Renderer threw");
                            }

                            done(std::move(result));
                        });

                return [sourceSubscription, cancelled] {
                    cancelled->store(true);
                    sourceSubscription->cancel();
                };
            },
            [state, key, callback = std::move(callback)](ImageResult result) {
                state->event(PipelineEvent::Kind::Completed, key.digest, result.error, result.source);
                deliver(callback, std::move(result));
            });
}

PipelineStats ImagePipeline::stats() const {
    return {state_->sources.count() + state_->payloads.count(), state_->renders.count() + state_->loadedRenders.count()};
}

ImagePipeline::ImagePipeline(QSharedPointer<IImageMemoryCache> cache, QSharedPointer<IImageSourceLoader> loader, Renderer renderer, int workerCount,
                             EventSink sink, PipelineResources resources, QList<QSharedPointer<IPipelineInterceptor>> interceptors,
                             QVector<QSharedPointer<IImageDecoder>> decoders)
    : state_(QSharedPointer<State>::create()) {
    if (!cache || !loader || !renderer || workerCount <= 0)
        throw std::invalid_argument("Invalid pipeline dependencies");

    state_->cache = std::move(cache);
    state_->loader = std::move(loader);
    state_->resources = std::move(resources);
    state_->interceptors = std::move(interceptors);
    for (const auto& interceptor : state_->interceptors)
        if (!interceptor)
            throw std::invalid_argument("Pipeline interceptor must not be null");
    state_->decoders = std::move(decoders);
    for (const auto& decoder : state_->decoders)
        if (!decoder || decoder->identity().identifier.isEmpty() || decoder->identity().version == 0)
            throw std::invalid_argument("Image decoder must have a valid identity");
    state_->renderer = std::move(renderer);
    state_->sink = std::move(sink);
    state_->workers = QSharedPointer<QThreadPool>::create();
    state_->workers->setMaxThreadCount(workerCount);
}

Subscription ImagePipeline::request(const SourceRequest& input, Completion callback) {
    const auto state = state_;
    auto request = input;
    request.render.decoders += state->decoders;
    if (state->interceptors.isEmpty())
        return requestSource(state, request, std::move(callback));

    return PipelineInterceptorChain::start(
            state->interceptors, [state](SourceRequest request, PipelineCompletion completion) { return requestSource(state, request, std::move(completion)); },
            std::move(request), std::move(callback));
}

Subscription ImagePipeline::requestSource(const QSharedPointer<State>& state, const SourceRequest& input, Completion callback) {
    auto request = input;
    if (request.render.contentTypeHint.isEmpty())
        request.render.contentTypeHint = request.source.contentType;
    request.load.enableSourceDisk = input.load.enableSourceDisk &&
            (input.diskStrategy == DiskCacheStrategy::SourceOnly || input.diskStrategy == DiskCacheStrategy::All ||
             (input.diskStrategy == DiskCacheStrategy::Automatic && input.source.kind == ImageSource::Kind::Network));
    const auto pool = state->workers;
    if (!state->accepting.load() || !pool) {
        deliver(callback, ImageResult::failure(ImageError::Cancelled));
        return {};
    }
    if (!state->loader) {
        deliver(callback, ImageResult::failure(ImageError::InvalidRequest));
        return {};
    }

    Result<SourceKey> source;
    try {
        source = state->loader->key(request.source);
    } catch (...) {
        deliver(callback, ImageResult::failure(ImageError::IoError));
        return {};
    }
    if (!source) {
        deliver(callback, ImageResult::failure(source.error));
        return {};
    }
    const auto rendered = KeyBuilder().render(*source.value, request.render);
    if (!rendered) {
        deliver(callback, ImageResult::failure(rendered.error));
        return {};
    }
    const auto key = *rendered.value;
    const auto policy = request.load.cache;

    QByteArray sourceTask;
    QDataStream stream(&sourceTask, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_12);
    stream << source.value->digest << qint32(policy.read) << qint32(policy.write) << policy.allowStale << request.load.maxBytes
           << request.load.enableSourceDisk;
    QByteArray renderTask = sourceTask + key.digest;
    QDataStream renderStream(&renderTask, QIODevice::Append);
    renderStream << qint32(request.diskStrategy) << request.expensiveProcessing;

    return state->loadedRenders.subscribe(
            renderTask,
            [state, pool, request, sourceKey = *source.value, sourceTask, key](auto done) -> CancelAction {
                auto token = QSharedPointer<std::atomic<bool>>::create(false);
                auto subscription = QSharedPointer<Subscription>::create();
                *subscription = state->payloads.subscribe(
                        sourceTask,
                        [state, pool, request, sourceKey](auto completed) -> CancelAction {
                            auto cancelled = QSharedPointer<std::atomic<bool>>::create(false);
                            auto loader = state->loader;
                            state->event(PipelineEvent::Kind::SourceStarted, sourceKey.digest);
                            pool->start(QRunnable::create([state, loader, request, sourceKey, cancelled, completed] {
                                Result<SourcePayload> result;
                                try {
                                    result = loader->load(request.source, sourceKey, request.load, *cancelled);
                                } catch (...) {
                                    result = Result<SourcePayload>::failure(ImageError::IoError, "Source loader threw");
                                }
                                if (cancelled->load())
                                    result = Result<SourcePayload>::failure(ImageError::Cancelled);
                                state->event(PipelineEvent::Kind::SourceCompleted, sourceKey.digest, result.error, result.source);
                                completed(std::move(result));
                            }));
                            return [cancelled] { cancelled->store(true); };
                        },
                        [state, request, key, token, done](Result<SourcePayload> payload) {
                            try {
                                if (token->load()) {
                                    done(ImageResult::failure(ImageError::Cancelled));
                                    return;
                                }
                                if (!payload) {
                                    done(ImageResult::failure(payload.error, payload.message));
                                    return;
                                }
                                const auto& p = *payload.value;
                                const auto contentType =
                                        (p.contentType.isEmpty() ? request.render.contentTypeHint : p.contentType).split(';').value(0).trimmed().toLower();
                                RenderKey contentKey = key;
                                contentKey.digest = QCryptographicHash::hash(
                                        key.digest + QCryptographicHash::hash(p.bytes, QCryptographicHash::Sha256) + contentType, QCryptographicHash::Sha256);

                                const bool renderedDisk = state->resources.renderedDisk &&
                                        (request.diskStrategy == DiskCacheStrategy::RenderedOnly || request.diskStrategy == DiskCacheStrategy::All ||
                                         (request.diskStrategy == DiskCacheStrategy::Automatic &&
                                          (request.source.kind == ImageSource::Kind::Local || request.source.kind == ImageSource::Kind::Resource) &&
                                          request.expensiveProcessing));
                                auto finish = [&](ImageResult result) {
                                    if (token->load()) {
                                        done(ImageResult::failure(ImageError::Cancelled));
                                        return;
                                    }
                                    if (result && !p.noStore && writeMemory(request.load.cache.write) && state->resources.active) {
                                        auto shared = state->cache->get(contentKey, true);
                                        if (shared)
                                            result.value = std::move(shared.value);
                                        result.handle = state->resources.active->acquire(contentKey, *result.value);
                                    }
                                    done(std::move(result));
                                };

                                if (!p.noStore && readMemory(request.load.cache.read)) {
                                    if (state->resources.active) {
                                        auto handle = state->resources.active->find(contentKey);
                                        if (handle) {
                                            auto result = ImageResult::success(handle.image(), CacheResultSource::ActiveResource);
                                            result.handle = std::move(handle);
                                            finish(std::move(result));
                                            return;
                                        }
                                    }
                                    auto hit = state->cache->get(contentKey, request.load.cache.allowStale && !p.mustRevalidate);
                                    if (hit) {
                                        finish(std::move(hit));
                                        return;
                                    }
                                }

                                const auto read = request.load.cache.read;
                                if (!p.noStore && renderedDisk && read != CacheReadPolicy::NoCache && read != CacheReadPolicy::BypassDisk &&
                                    read != CacheReadPolicy::BypassMemory) {
                                    auto hit = state->resources.renderedDisk->get(contentKey);
                                    if (hit) {
                                        if (writeMemory(request.load.cache.write))
                                            state->cache->put(contentKey, *hit.value, p.expiresAt);
                                        finish(std::move(hit));
                                        return;
                                    }
                                }
                                ImageResult result;
                                try {
                                    state->event(PipelineEvent::Kind::RenderStarted, key.digest);
                                    auto renderOptions = request.render;
                                    renderOptions.contentTypeHint = contentType;
                                    result = state->renderer(p.bytes, renderOptions, *token);
                                    if (token->load()) {
                                        done(ImageResult::failure(ImageError::Cancelled));
                                        return;
                                    }
                                    if (result && result.value->isNull())
                                        result = ImageResult::failure(ImageError::ProcessingError);
                                    if (result) {
                                        result.value->setDevicePixelRatio(request.render.dpr);
                                        result.source = payload.source;
                                        if (p.noStore || payload.source == CacheResultSource::Network)
                                            state->cache->remove(contentKey);
                                        if (!p.noStore && writeMemory(request.load.cache.write))
                                            state->cache->put(contentKey, *result.value, p.expiresAt);
                                        const auto write = request.load.cache.write;
                                        if (!p.noStore && renderedDisk && (write == CacheWritePolicy::Default || write == CacheWritePolicy::DiskOnly))
                                            state->resources.renderedDisk->put(contentKey, *result.value);
                                    }
                                } catch (...) {
                                    result = ImageResult::failure(ImageError::ProcessingError);
                                }
                                finish(std::move(result));
                            } catch (...) {
                                done(ImageResult::failure(ImageError::ProcessingError, "Result handling failed"));
                            }
                        });
                return [token, subscription] {
                    token->store(true);
                    subscription->cancel();
                };
            },
            [state, key, callback = std::move(callback)](ImageResult result) {
                state->event(PipelineEvent::Kind::Completed, key.digest, result.error, result.source);
                deliver(callback, std::move(result));
            });
}

void ImagePipeline::trimMemory(MemoryPressure pressure) {
    const auto stats = state_->cache->stats();
    state_->cache->trim(pressure == MemoryPressure::Background ? stats.maxBytes / 2 : 0);
    if (state_->loader)
        state_->loader->trimMemory(pressure != MemoryPressure::Background);
    if (pressure == MemoryPressure::Critical && state_->resources.active)
        state_->resources.active->setMaxCost(0);
}

ImageCacheStats ImagePipeline::cacheStats() const {
    ImageCacheStats result;
    result.renderedMemory = state_->cache->stats();
    if (state_->loader) {
        const auto source = state_->loader->cacheStats();
        result.encodedMemory = source.encodedMemory;
        result.rawDisk = source.rawDisk;
        result.network = source.network;
    }
    if (state_->resources.renderedDisk)
        result.renderedDisk = state_->resources.renderedDisk->stats();
    if (state_->resources.active)
        result.active = state_->resources.active->stats();
    return result;
}

bool ImagePipeline::waitForIdle(int timeoutMs) {
    const auto pool = state_->workers;
    if (timeoutMs < 0 || (pool && pool->contains(QThread::currentThread())))
        return false;

    if (pool && !pool->waitForDone(timeoutMs))
        return false;

    const auto pending = stats();
    return pending.sourceInFlight == 0 && pending.renderInFlight == 0;
}

QVector<CacheDebugEntry> ImagePipeline::debugDump(int limit) const {
    return state_->cache->debugDump(limit);
}

bool ImagePipeline::invalidate(const CacheSelector& selector, bool includeSource) {
    if (!selector.valid())
        return false;

    bool success = true;
    auto attempt = [&](auto operation) {
        try {
            success = operation() && success;
        } catch (...) {
            success = false;
        }
    };
    attempt([&] { return state_->cache->invalidate(selector); });
    if (state_->resources.active)
        attempt([&] { return state_->resources.active->invalidate(selector); });
    if (state_->resources.renderedDisk)
        attempt([&] { return state_->resources.renderedDisk->invalidate(selector); });
    if (includeSource && state_->loader)
        attempt([&] { return state_->loader->invalidate(selector); });
    return success;
}

bool ImagePipeline::removeSource(const SourceKey& source) {
    return invalidate({source.digest, {}}, true);
}

bool ImagePipeline::removeRenderVariants(const SourceKey& source) {
    return invalidate({source.digest, {}}, false);
}

bool ImagePipeline::clearNamespace(const QByteArray& nameSpace) {
    return invalidate({{}, KeyBuilder::namespaceDigest(nameSpace)}, true);
}

Subscription ImagePipeline::request(const QString& source, const RenderOptions& render, Completion callback) {
    auto parsed = ImageSource::fromString(source);
    if (!parsed) {
        deliver(callback, ImageResult::failure(parsed.error, parsed.message));
        return {};
    }

    SourceRequest input;
    input.source = std::move(*parsed.value);
    input.render = render;
    return request(input, std::move(callback));
}

ImageSubscription* ImagePipeline::request(const SourceRequest& input, QObject* parent) {
    auto* subscription = new ImageSubscription(parent);
    QPointer<ImageSubscription> guard(subscription);
    const auto completed = subscription->completion();
    try {
        auto task = request(input, completed);
        if (guard)
            guard->attach(std::move(task));
    } catch (...) {
        completed(ImageResult::failure(ImageError::ProcessingError));
    }
    return guard.data();
}

ImageSubscription* ImagePipeline::request(const QString& source, const RenderOptions& render, QObject* parent) {
    auto parsed = ImageSource::fromString(source);
    if (!parsed) {
        auto* subscription = new ImageSubscription(parent);
        subscription->completion()(ImageResult::failure(parsed.error, parsed.message));
        return subscription;
    }

    SourceRequest input;
    input.source = std::move(*parsed.value);
    input.render = render;
    return request(input, parent);
}
} // namespace aster::cache
