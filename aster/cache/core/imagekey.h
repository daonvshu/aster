#pragma once

#include "imagerendergeometry.h"
#include "imageresult.h"

#include <QByteArray>
#include <QSize>
#include <QUrl>
#include <QVector>

#include <functional>

namespace aster::cache
{
struct SourceKey
{
    QByteArray digest;
    QByteArray namespaceDigest;

    bool operator==(const SourceKey& other) const
    {
        return digest == other.digest;
    }
};

struct RenderKey
{
    SourceKey source;
    QByteArray digest;

    bool operator==(const RenderKey& other) const
    {
        return digest == other.digest && source == other.source;
    }
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
inline size_t qHash(const SourceKey& key, size_t seed = 0)
{
    return ::qHash(key.digest, seed);
}

inline size_t qHash(const RenderKey& key, size_t seed = 0)
{
    return ::qHash(key.digest, seed);
}
#else
inline uint qHash(const SourceKey& key, uint seed = 0)
{
    return ::qHash(key.digest, seed);
}

inline uint qHash(const RenderKey& key, uint seed = 0)
{
    return ::qHash(key.digest, seed);
}
#endif

struct KeyContext
{
    QByteArray nameSpace = "aster/v1";
    QByteArray tenant;
    QByteArray authScope;
    QByteArray contentVariant;
    QByteArray revision;
};

struct LocalFingerprint
{
    QString canonicalPath;
    qint64 byteSize = 0;
    qint64 modifiedMs = 0;
    QByteArray contentHash;
};

struct ProcessorIdentity
{
    QByteArray identifier;
    quint32 version = 1;
    QByteArray parameters;
};

struct RenderOptions
{
    QSize physicalTargetSize;
    double dpr = 1.0;
    QByteArray fitMode = "contain";
    QVector<ProcessorIdentity> processors;
    quint32 schemaVersion = 1;
    ImageScaleAlgorithm scaleAlgorithm = ImageScaleAlgorithm::QtSmooth;
    quint32 resamplerVersion = 1;
};

class KeyBuilder
{
public:
    using UrlNormalizer = std::function<QUrl(const QUrl&)>;

    explicit KeyBuilder(UrlNormalizer normalizer = {});

    static QByteArray namespaceDigest(const QByteArray& nameSpace);

    Result<SourceKey> network(const QUrl&, const KeyContext& = {}) const;
    Result<SourceKey> localFile(const QString&, const KeyContext& = {}, bool strict = false) const;
    Result<SourceKey> resource(const QString&, const KeyContext& = {}) const;
    Result<SourceKey> local(const LocalFingerprint&, const KeyContext& = {}) const;
    Result<RenderKey> render(const SourceKey&, const RenderOptions&) const;

private:
    UrlNormalizer normalizer_;
};
}
