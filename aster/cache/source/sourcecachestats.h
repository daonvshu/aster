#pragma once

#include "aster/cache/cache/idiskcache.h"
#include "aster/cache/cache/iimagememorycache.h"

namespace aster::cache {
struct NetworkStats {
    quint64 requests = 0;
    quint64 responses = 0;
    quint64 validations = 0;
    quint64 errors = 0;
    quint64 receivedBytes = 0;
    LookupLatency latency;
};

struct SourceCacheStats {
    CacheStats encodedMemory;
    DiskStats rawDisk;
    NetworkStats network;
};
} // namespace aster::cache
