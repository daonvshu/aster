#pragma once

#include "aster/cache/core/imageresult.h"
#include "aster/cache/core/imagetask.h"
#include "sourcerequest.h"

#include <QList>
#include <QSharedPointer>

#include <functional>

namespace aster::cache {
using PipelineCompletion = std::function<void(ImageResult)>;

class IPipelineInterceptor;
class ImagePipeline;

class PipelineInterceptorChain {
public:
    /**
     * @brief Copies a pipeline interceptor chain handle.
     * @param other Chain handle to copy.
     */
    PipelineInterceptorChain(const PipelineInterceptorChain& other);

    /**
     * @brief Replaces this handle with a copied pipeline interceptor chain.
     * @param other Chain handle to copy.
     * @return This chain handle.
     */
    PipelineInterceptorChain& operator=(const PipelineInterceptorChain& other);

    /**
     * @brief Moves a pipeline interceptor chain handle.
     * @param other Chain handle to move.
     */
    PipelineInterceptorChain(PipelineInterceptorChain&& other) noexcept;

    /**
     * @brief Replaces this handle with a moved pipeline interceptor chain.
     * @param other Chain handle to move.
     * @return This chain handle.
     */
    PipelineInterceptorChain& operator=(PipelineInterceptorChain&& other) noexcept;

    /** @brief Destroys this chain handle without cancelling downstream work. */
    ~PipelineInterceptorChain();

    /**
     * @brief Continues the pipeline interceptor chain with the supplied request.
     * @param request Request passed to the next interceptor or image pipeline.
     * @param completion Final result callback. It may be called asynchronously.
     * @return Subscription that cancels the downstream request.
     */
    Subscription proceed(SourceRequest request, PipelineCompletion completion) const;

private:
    using Terminal = std::function<Subscription(SourceRequest, PipelineCompletion)>;

    struct State;

    /**
     * @brief Creates a chain handle at a specific interceptor index.
     * @param state Shared interceptor chain state.
     * @param index Next interceptor index.
     */
    PipelineInterceptorChain(QSharedPointer<State> state, int index);

    /**
     * @brief Starts a validated pipeline interceptor chain.
     * @param interceptors Interceptors in registration order.
     * @param terminal Terminal image pipeline request function.
     * @param request Initial image request.
     * @param completion Final result callback.
     * @return Subscription that cancels work started by the chain.
     */
    static Subscription start(const QList<QSharedPointer<IPipelineInterceptor>>& interceptors, Terminal terminal, SourceRequest request,
                              PipelineCompletion completion);

    QSharedPointer<State> state_;
    int index_ = 0;

    friend class ImagePipeline;
};

class IPipelineInterceptor {
public:
    virtual ~IPipelineInterceptor() = default;

    /**
     * @brief Intercepts a complete image request and its final result.
     * @param chain Copyable remaining interceptor chain. It may be retained for asynchronous retry or fallback.
     * @param request Current image request. It may be modified before calling proceed().
     * @param completion Final result callback.
     * @return Subscription that cancels work started by this interceptor.
     */
    virtual Subscription intercept(PipelineInterceptorChain chain, SourceRequest request, PipelineCompletion completion) = 0;
};

class PipelineInterceptor final : public IPipelineInterceptor {
public:
    using Handler = std::function<Subscription(PipelineInterceptorChain, SourceRequest, PipelineCompletion)>;

    /**
     * @brief Creates a pipeline interceptor backed by a callable.
     * @param handler Callable invoked for each complete image request. It must not be empty.
     * @return Shared interceptor instance.
     */
    static QSharedPointer<PipelineInterceptor> create(Handler handler);

    /**
     * @brief Invokes the configured pipeline interceptor callable.
     * @param chain Copyable remaining interceptor chain.
     * @param request Current image request.
     * @param completion Final result callback.
     * @return Subscription that cancels work started by the callable.
     */
    Subscription intercept(PipelineInterceptorChain chain, SourceRequest request, PipelineCompletion completion) override;

private:
    /**
     * @brief Stores a validated pipeline interceptor callable.
     * @param handler Callable invoked for complete image requests.
     */
    explicit PipelineInterceptor(Handler handler);

    Handler handler_;
};
} // namespace aster::cache
