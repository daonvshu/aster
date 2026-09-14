#pragma once

#include "aster/cache/core/imagekey.h"
#include "aster/cache/core/imagetask.h"

namespace aster::cache {
struct SourceLoadKey {
    SourceKey source;

    bool operator==(const SourceLoadKey& other) const {
        return source == other.source;
    }
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
inline size_t qHash(const SourceLoadKey& key, size_t seed = 0) {
    return qHash(key.source, seed);
}
#else
inline uint qHash(const SourceLoadKey& key, uint seed = 0) {
    return qHash(key.source, seed);
}
#endif

using SourceCompletion = std::function<void(Result<QByteArray>)>;
using SourceTask = std::function<CancelAction(const SourceKey&, SourceCompletion)>;
} // namespace aster::cache
