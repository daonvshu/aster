#include "httpinterceptor.h"

#include <stdexcept>
#include <utility>

namespace aster::cache {
namespace {
class Chain final : public HttpInterceptorChain {
public:
    Chain(const QSharedPointer<INetworkService>& network, const QList<QSharedPointer<IHttpInterceptor>>& interceptors, int index)
        : network_(network)
        , interceptors_(interceptors)
        , index_(index) {
    }

    Result<NetworkResponse> proceed(HttpRequest request, const std::atomic<bool>& cancelled) override {
        if (cancelled.load())
            return Result<NetworkResponse>::failure(ImageError::Cancelled);
        if (index_ >= interceptors_.size())
            return network_->fetch(request.url, request.options, cancelled);

        Chain next(network_, interceptors_, index_ + 1);
        return interceptors_.at(index_)->intercept(next, std::move(request), cancelled);
    }

private:
    const QSharedPointer<INetworkService>& network_;
    const QList<QSharedPointer<IHttpInterceptor>>& interceptors_;
    int index_;
};
} // namespace

HttpInterceptor::HttpInterceptor(Handler handler)
    : handler_(std::move(handler)) {
    if (!handler_)
        throw std::invalid_argument("HTTP interceptor handler must not be empty");
}

QSharedPointer<HttpInterceptor> HttpInterceptor::create(Handler handler) {
    return QSharedPointer<HttpInterceptor>(new HttpInterceptor(std::move(handler)));
}

Result<NetworkResponse> HttpInterceptor::intercept(HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
    return handler_(chain, std::move(request), cancelled);
}

HttpInterceptorNetworkService::HttpInterceptorNetworkService(QSharedPointer<INetworkService> network, QList<QSharedPointer<IHttpInterceptor>> interceptors)
    : network_(std::move(network))
    , interceptors_(std::move(interceptors)) {
    if (!network_)
        throw std::invalid_argument("network service must not be null");
    for (const auto& interceptor : interceptors_)
        if (!interceptor)
            throw std::invalid_argument("HTTP interceptor must not be null");
}

Result<NetworkResponse> HttpInterceptorNetworkService::fetch(const QUrl& url, const NetworkFetchOptions& options, const std::atomic<bool>& cancelled) {
    if (cancelled.load())
        return Result<NetworkResponse>::failure(ImageError::Cancelled);

    Result<NetworkResponse> result;
    try {
        Chain chain(network_, interceptors_, 0);
        result = chain.proceed({url, options}, cancelled);
    } catch (...) {
        result = Result<NetworkResponse>::failure(ImageError::IoError, "HTTP interceptor threw");
    }
    if (cancelled.load())
        return Result<NetworkResponse>::failure(ImageError::Cancelled);
    return result;
}
} // namespace aster::cache
