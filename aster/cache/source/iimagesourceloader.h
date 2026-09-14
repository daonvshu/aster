#pragma once

#include "aster/cache/core/imagecachepolicy.h"
#include "aster/cache/core/imagekey.h"
#include "sourcecachestats.h"

#include <QDateTime>
#include <QMap>

#include <atomic>

namespace aster::cache {
using HttpHeaders = QMap<QByteArray, QByteArray>;

struct ImageSource {
    enum class Kind { Local, Network, Data, Resource };
    Kind kind = Kind::Data;
    QUrl url;
    QByteArray data;
    QByteArray contentType;
    KeyContext context;
    HttpHeaders headers;
    bool strictLocalIdentity = false;

    static Result<ImageSource> fromString(const QString&, const KeyContext& = {});
};

struct SourcePayload {
    QByteArray bytes;
    QByteArray contentType;
    HttpHeaders headers;
    std::optional<QDateTime> expiresAt;
    bool noStore = false;
    bool mustRevalidate = false;
};

struct SourceLoadOptions {
    ImageCachePolicy cache;
    qint64 maxBytes = 32 * 1024 * 1024;
    bool enableSourceDisk = true;
};

class IImageSourceLoader {
public:
    virtual ~IImageSourceLoader() = default;

    virtual void trimMemory(bool) {
    }

    virtual Result<SourceKey> key(const ImageSource&) const = 0;

    virtual SourceCacheStats cacheStats() const {
        return {};
    }

    virtual bool invalidate(const CacheSelector&) {
        return false;
    }

    virtual Result<SourcePayload> load(const ImageSource&, const SourceKey&, const SourceLoadOptions&, const std::atomic<bool>&) = 0;
};
} // namespace aster::cache
