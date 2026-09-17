#include "sourceinterceptor.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace aster::cache {
namespace {
class Chain final : public SourceInterceptorChain {
public:
    Chain(const QSharedPointer<IImageSourceLoader>& loader, const QList<QSharedPointer<ISourceInterceptor>>& interceptors, int index)
        : loader_(loader)
        , interceptors_(interceptors)
        , index_(index) {
    }

    Result<SourcePayload> proceed(SourceLoadRequest request, const std::atomic<bool>& cancelled) override {
        if (cancelled.load())
            return Result<SourcePayload>::failure(ImageError::Cancelled);
        if (index_ >= interceptors_.size()) {
            try {
                return loader_->load(request.source, request.key, request.options, cancelled);
            } catch (...) {
                return Result<SourcePayload>::failure(ImageError::IoError, "Source loader threw");
            }
        }

        Chain next(loader_, interceptors_, index_ + 1);
        return interceptors_.at(index_)->intercept(next, std::move(request), cancelled);
    }

private:
    const QSharedPointer<IImageSourceLoader>& loader_;
    const QList<QSharedPointer<ISourceInterceptor>>& interceptors_;
    int index_;
};
} // namespace

SourceInterceptor::SourceInterceptor(Handler handler)
    : handler_(std::move(handler)) {
    if (!handler_)
        throw std::invalid_argument("Source interceptor handler must not be empty");
}

QSharedPointer<SourceInterceptor> SourceInterceptor::create(Handler handler) {
    return QSharedPointer<SourceInterceptor>(new SourceInterceptor(std::move(handler)));
}

Result<SourcePayload> SourceInterceptor::intercept(SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
    return handler_(chain, std::move(request), cancelled);
}

SourceInterceptorLoader::SourceInterceptorLoader(QSharedPointer<IImageSourceLoader> loader, QList<QSharedPointer<ISourceInterceptor>> interceptors)
    : loader_(std::move(loader))
    , interceptors_(std::move(interceptors)) {
    if (!loader_)
        throw std::invalid_argument("source loader must not be null");
    for (const auto& interceptor : interceptors_)
        if (!interceptor)
            throw std::invalid_argument("Source interceptor must not be null");
}

void SourceInterceptorLoader::trimMemory(bool critical) {
    loader_->trimMemory(critical);
}

Result<SourceKey> SourceInterceptorLoader::key(const ImageSource& source) const {
    return loader_->key(source);
}

SourceCacheStats SourceInterceptorLoader::cacheStats() const {
    return loader_->cacheStats();
}

bool SourceInterceptorLoader::invalidate(const CacheSelector& selector) {
    return loader_->invalidate(selector);
}

Result<SourcePayload> SourceInterceptorLoader::load(const ImageSource& source, const SourceKey& key, const SourceLoadOptions& options,
                                                    const std::atomic<bool>& cancelled) {
    using R = Result<SourcePayload>;
    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (options.maxBytes <= 0 || options.maxBytes > std::numeric_limits<int>::max() - 1)
        return R::failure(ImageError::InvalidRequest);

    R result;
    try {
        Chain chain(loader_, interceptors_, 0);
        result = chain.proceed({source, key, options}, cancelled);
    } catch (...) {
        result = R::failure(ImageError::ProcessingError, "Source interceptor threw");
    }
    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (result && result.value->bytes.isEmpty())
        return R::failure(ImageError::CorruptedEntry, "Empty intercepted source");
    if (result && result.value->bytes.size() > options.maxBytes)
        return R::failure(ImageError::InvalidRequest, "Intercepted source exceeds byte limit");
    return result;
}
} // namespace aster::cache
