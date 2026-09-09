#include "qtnetworkservice.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <stdexcept>

namespace aster::cache
{
QtNetworkService::QtNetworkService(int timeoutMs) : timeoutMs_(timeoutMs)
{
    if (timeoutMs <= 0)
        throw std::invalid_argument("timeout must be positive");
}

Result<NetworkResponse> QtNetworkService::fetch(const QUrl& url, const NetworkFetchOptions& options,
                                                const std::atomic<bool>& cancelled)
{
    using R = Result<NetworkResponse>;
    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (options.cacheOnly)
        return R::failure(ImageError::CacheMiss);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") ||
        options.maxBytes <= 0)
        return R::failure(ImageError::InvalidRequest);

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    // Redirects need application-level authorization/header policy; do not leak credentials.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    for (auto it = options.headers.cbegin(); it != options.headers.cend(); ++it)
        request.setRawHeader(it.key(), it.value());

    QEventLoop loop;
    QTimer timeout, cancellation;
    timeout.setSingleShot(true);
    cancellation.setInterval(10);
    auto* reply = manager.get(request);
    reply->setReadBufferSize(65536);
    NetworkResponse response;
    bool tooLarge = false;
    bool timedOut = false;
    const auto consume = [&]
    {
        const auto available = reply->bytesAvailable();
        if (available > options.maxBytes - response.body.size())
        {
            tooLarge = true;
            reply->abort();
            return;
        }
        response.body += reply->readAll();
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [&]
                     {
                         timedOut = true;
                         reply->abort();
                     });
    QObject::connect(&cancellation, &QTimer::timeout, &loop,
                     [&]
                     {
                         if (cancelled.load())
                             reply->abort();
                     });
    timeout.start(timeoutMs_);
    cancellation.start();
    if (!reply->isFinished())
        loop.exec();
    consume();
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    for (const auto& header : reply->rawHeaderPairs())
    {
        const auto name = header.first.toLower();
        if (response.headers.contains(name))
            response.headers[name] += ", " + header.second;
        else
            response.headers.insert(name, header.second);
    }
    const auto error = reply->error();
    delete reply;
    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (tooLarge)
        return R::failure(ImageError::InvalidRequest, "Response exceeds byte limit");
    if (timedOut || (error != QNetworkReply::NoError && response.status < 400))
        return R::failure(ImageError::IoError);
    return R::success(std::move(response), CacheResultSource::Network);
}
}
