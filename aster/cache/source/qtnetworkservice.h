#pragma once

#include "networkservice.h"

namespace aster::cache {
class QtNetworkService final : public INetworkService {
public:
    explicit QtNetworkService(int timeoutMs = 30000);
    Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions&, const std::atomic<bool>&) override;

private:
    int timeoutMs_;
};
} // namespace aster::cache
