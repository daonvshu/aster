#include "pipelineinterceptor.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace aster::cache {
struct PipelineInterceptorChain::State {
    QList<QSharedPointer<IPipelineInterceptor>> interceptors;
    Terminal terminal;
};

namespace {
PipelineCompletion guarded(PipelineCompletion completion) {
    auto completed = QSharedPointer<std::atomic<bool>>::create(false);
    return [completed, completion = std::move(completion)](ImageResult result) {
        bool expected = false;
        if (!completed->compare_exchange_strong(expected, true))
            return;
        try {
            if (completion)
                completion(std::move(result));
        } catch (...) {
        }
    };
}
} // namespace

PipelineInterceptorChain::PipelineInterceptorChain(QSharedPointer<State> state, int index)
    : state_(std::move(state))
    , index_(index) {
}

PipelineInterceptorChain::PipelineInterceptorChain(const PipelineInterceptorChain& other) = default;

PipelineInterceptorChain& PipelineInterceptorChain::operator=(const PipelineInterceptorChain& other) = default;

PipelineInterceptorChain::PipelineInterceptorChain(PipelineInterceptorChain&& other) noexcept = default;

PipelineInterceptorChain& PipelineInterceptorChain::operator=(PipelineInterceptorChain&& other) noexcept = default;

PipelineInterceptorChain::~PipelineInterceptorChain() = default;

Subscription PipelineInterceptorChain::start(const QList<QSharedPointer<IPipelineInterceptor>>& interceptors, Terminal terminal, SourceRequest request,
                                             PipelineCompletion completion) {
    auto state = QSharedPointer<State>::create();
    state->interceptors = interceptors;
    state->terminal = std::move(terminal);
    return PipelineInterceptorChain(std::move(state), 0).proceed(std::move(request), guarded(std::move(completion)));
}

Subscription PipelineInterceptorChain::proceed(SourceRequest request, PipelineCompletion completion) const {
    if (!state_ || !state_->terminal) {
        if (completion)
            completion(ImageResult::failure(ImageError::InvalidRequest, "Invalid pipeline interceptor chain"));
        return {};
    }

    try {
        if (index_ >= state_->interceptors.size())
            return state_->terminal(std::move(request), completion);

        auto interceptor = state_->interceptors.at(index_);
        if (!interceptor) {
            if (completion)
                completion(ImageResult::failure(ImageError::InvalidRequest, "Pipeline interceptor must not be null"));
            return {};
        }
        return interceptor->intercept(PipelineInterceptorChain(state_, index_ + 1), std::move(request), completion);
    } catch (...) {
        if (completion)
            completion(ImageResult::failure(ImageError::ProcessingError, "Pipeline interceptor threw"));
        return {};
    }
}

PipelineInterceptor::PipelineInterceptor(Handler handler)
    : handler_(std::move(handler)) {
    if (!handler_)
        throw std::invalid_argument("Pipeline interceptor handler must not be empty");
}

QSharedPointer<PipelineInterceptor> PipelineInterceptor::create(Handler handler) {
    return QSharedPointer<PipelineInterceptor>(new PipelineInterceptor(std::move(handler)));
}

Subscription PipelineInterceptor::intercept(PipelineInterceptorChain chain, SourceRequest request, PipelineCompletion completion) {
    return handler_(std::move(chain), std::move(request), std::move(completion));
}
} // namespace aster::cache
