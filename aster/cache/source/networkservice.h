#pragma once

#include "iimagesourceloader.h"

namespace aster::cache
{
struct NetworkFetchOptions
{
    bool preferCache = true;
    bool reload = false;
    bool cacheOnly = false;
    bool validation = true;
    HttpHeaders headers;
    qint64 maxBytes = 32 * 1024 * 1024;
};

struct NetworkResponse
{
    int status = 0;
    HttpHeaders headers;
    QByteArray body;
};

class INetworkService
{
public:
    virtual ~INetworkService() = default;
    virtual Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions&,
                                          const std::atomic<bool>& cancelled) = 0;
};
}
