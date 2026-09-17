#pragma once

#include "networkservice.h"

#include <QList>
#include <QSharedPointer>

#include <functional>

namespace aster::cache {
struct HttpRequest {
    QUrl url;
    NetworkFetchOptions options;
};

class HttpInterceptorChain {
public:
    virtual ~HttpInterceptorChain() = default;

    /**
     * @brief Continues the HTTP interceptor chain with the supplied request.
     * @param request Request passed to the next interceptor or network service.
     * @param cancelled Shared cancellation state for the image request.
     * @return Network response or failure.
     */
    virtual Result<NetworkResponse> proceed(HttpRequest request, const std::atomic<bool>& cancelled) = 0;
};

class IHttpInterceptor {
public:
    virtual ~IHttpInterceptor() = default;

    /**
     * @brief Intercepts an HTTP request and optionally continues the chain.
     * @param chain Remaining interceptor chain.
     * @param request Current request. It may be modified before calling proceed().
     * @param cancelled Shared cancellation state for the image request.
     * @return Network response or failure.
     */
    virtual Result<NetworkResponse> intercept(HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) = 0;
};

class HttpInterceptor final : public IHttpInterceptor {
public:
    using Handler = std::function<Result<NetworkResponse>(HttpInterceptorChain&, HttpRequest, const std::atomic<bool>&)>;

    /**
     * @brief Creates an HTTP interceptor backed by a callable.
     * @param handler Callable invoked for each intercepted HTTP request. It must not be empty.
     * @return Shared interceptor instance.
     */
    static QSharedPointer<HttpInterceptor> create(Handler handler);

    /**
     * @brief Invokes the configured interceptor callable.
     * @param chain Remaining interceptor chain.
     * @param request Current HTTP request.
     * @param cancelled Shared cancellation state for the image request.
     * @return Network response or failure.
     */
    Result<NetworkResponse> intercept(HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) override;

private:
    /**
     * @brief Stores a validated interceptor callable.
     * @param handler Callable invoked for intercepted requests.
     */
    explicit HttpInterceptor(Handler handler);

    Handler handler_;
};

class HttpInterceptorNetworkService final : public INetworkService {
public:
    /**
     * @brief Creates a network service that runs interceptors in registration order.
     * @param network Terminal network service. It must not be null.
     * @param interceptors HTTP interceptors. Entries must not be null.
     */
    HttpInterceptorNetworkService(QSharedPointer<INetworkService> network, QList<QSharedPointer<IHttpInterceptor>> interceptors);

    /**
     * @brief Runs the configured interceptor chain and terminal network service.
     * @param url Request URL.
     * @param options Network fetch options.
     * @param cancelled Shared cancellation state for the image request.
     * @return Network response or failure.
     */
    Result<NetworkResponse> fetch(const QUrl& url, const NetworkFetchOptions& options, const std::atomic<bool>& cancelled) override;

private:
    QSharedPointer<INetworkService> network_;
    QList<QSharedPointer<IHttpInterceptor>> interceptors_;
};
} // namespace aster::cache
