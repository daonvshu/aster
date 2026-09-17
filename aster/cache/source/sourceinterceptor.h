#pragma once

#include "iimagesourceloader.h"

#include <QList>
#include <QSharedPointer>

#include <functional>

namespace aster::cache {
struct SourceLoadRequest {
    ImageSource source;
    SourceKey key;
    SourceLoadOptions options;
};

class SourceInterceptorChain {
public:
    virtual ~SourceInterceptorChain() = default;

    /**
     * @brief Continues the source interceptor chain with the supplied request.
     * @param request Source, identity, and load options passed to the next interceptor or loader.
     * @param cancelled Shared cancellation state for the image request.
     * @return Source payload or failure.
     */
    virtual Result<SourcePayload> proceed(SourceLoadRequest request, const std::atomic<bool>& cancelled) = 0;
};

class ISourceInterceptor {
public:
    virtual ~ISourceInterceptor() = default;

    /**
     * @brief Intercepts a source load and optionally continues the chain.
     * @param chain Remaining source interceptor chain.
     * @param request Current source load request. The source and key must remain consistent.
     * @param cancelled Shared cancellation state for the image request.
     * @return Source payload or failure.
     */
    virtual Result<SourcePayload> intercept(SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) = 0;
};

class SourceInterceptor final : public ISourceInterceptor {
public:
    using Handler = std::function<Result<SourcePayload>(SourceInterceptorChain&, SourceLoadRequest, const std::atomic<bool>&)>;

    /**
     * @brief Creates a source interceptor backed by a callable.
     * @param handler Callable invoked for each intercepted source load. It must not be empty.
     * @return Shared interceptor instance.
     */
    static QSharedPointer<SourceInterceptor> create(Handler handler);

    /**
     * @brief Invokes the configured source interceptor callable.
     * @param chain Remaining source interceptor chain.
     * @param request Current source load request.
     * @param cancelled Shared cancellation state for the image request.
     * @return Source payload or failure.
     */
    Result<SourcePayload> intercept(SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) override;

private:
    /**
     * @brief Stores a validated source interceptor callable.
     * @param handler Callable invoked for intercepted source loads.
     */
    explicit SourceInterceptor(Handler handler);

    Handler handler_;
};

class SourceInterceptorLoader final : public IImageSourceLoader {
public:
    /**
     * @brief Creates a source loader that runs interceptors in registration order.
     * @param loader Terminal source loader. It must not be null.
     * @param interceptors Source interceptors. Entries must not be null.
     */
    SourceInterceptorLoader(QSharedPointer<IImageSourceLoader> loader, QList<QSharedPointer<ISourceInterceptor>> interceptors);

    /** @brief Forwards memory trimming to the terminal source loader. */
    void trimMemory(bool critical) override;

    /**
     * @brief Resolves source identity through the terminal source loader.
     * @param source Source whose identity is required.
     * @return Stable source key or failure.
     */
    Result<SourceKey> key(const ImageSource& source) const override;

    /** @brief Returns statistics from the terminal source loader. */
    SourceCacheStats cacheStats() const override;

    /**
     * @brief Invalidates matching entries in the terminal source loader.
     * @param selector Cache selector to invalidate.
     * @return Whether invalidation completed successfully.
     */
    bool invalidate(const CacheSelector& selector) override;

    /**
     * @brief Runs the configured interceptor chain and terminal source loader.
     * @param source Image source to load.
     * @param key Expected identity of the image source.
     * @param options Source loading and cache options.
     * @param cancelled Shared cancellation state for the image request.
     * @return Intercepted source payload or failure.
     */
    Result<SourcePayload> load(const ImageSource& source, const SourceKey& key, const SourceLoadOptions& options, const std::atomic<bool>& cancelled) override;

private:
    QSharedPointer<IImageSourceLoader> loader_;
    QList<QSharedPointer<ISourceInterceptor>> interceptors_;
};
} // namespace aster::cache
