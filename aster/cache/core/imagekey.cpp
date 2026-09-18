#include "imagekey.h"

#include "resampleconfig.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cmath>

namespace aster::cache {
namespace {
void configure(QDataStream& stream) {
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

void context(QDataStream& stream, const KeyContext& c) {
    stream << c.nameSpace << c.tenant << c.authScope << c.contentVariant << c.revision;
}

QByteArray digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
} // namespace

KeyBuilder::KeyBuilder(UrlNormalizer normalizer)
    : normalizer_(std::move(normalizer)) {
}

QByteArray KeyBuilder::namespaceDigest(const QByteArray& nameSpace) {
    return digest(QByteArray("aster/namespace/v1:") + nameSpace);
}

Result<SourceKey> KeyBuilder::network(const QUrl& input, const KeyContext& c) const {
    QUrl url = normalizer_ ? normalizer_(input) : input;
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https"))
        return Result<SourceKey>::failure(ImageError::InvalidRequest, "Invalid HTTP source");

    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    configure(stream);
    stream << QByteArray("aster/source/network/v1");
    context(stream, c);
    stream << url.toEncoded(QUrl::FullyEncoded);

    return Result<SourceKey>::success({digest(bytes), namespaceDigest(c.nameSpace)});
}

Result<SourceKey> KeyBuilder::local(const LocalFingerprint& f, const KeyContext& c) const {
    if (f.canonicalPath.isEmpty() || f.byteSize < 0)
        return Result<SourceKey>::failure(ImageError::InvalidRequest);

    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    configure(stream);
    stream << QByteArray("aster/source/local/v1");
    context(stream, c);
    stream << f.canonicalPath.toUtf8() << f.byteSize << f.modifiedMs << f.contentHash;

    return Result<SourceKey>::success({digest(bytes), namespaceDigest(c.nameSpace)});
}

Result<SourceKey> KeyBuilder::localFile(const QString& path, const KeyContext& c, bool strict) const {
    const QFileInfo before(path);
    if (!before.exists() || !before.isFile())
        return Result<SourceKey>::failure(ImageError::IoError, "Source file is unavailable");

    LocalFingerprint f{before.canonicalFilePath(), before.size(), before.lastModified().toMSecsSinceEpoch(), {}};
    if (strict) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return Result<SourceKey>::failure(ImageError::IoError);

        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
            return Result<SourceKey>::failure(ImageError::IoError);
        f.contentHash = hash.result();
    }

    const QFileInfo after(path);
    if (!after.exists() || after.canonicalFilePath() != f.canonicalPath || after.size() != f.byteSize ||
        after.lastModified().toMSecsSinceEpoch() != f.modifiedMs)
        return Result<SourceKey>::failure(ImageError::SourceChanged);

    return local(f, c);
}

Result<RenderKey> KeyBuilder::render(const SourceKey& source, const RenderOptions& o) const {
    if (source.digest.size() != 32 || o.physicalTargetSize.width() <= 0 || o.physicalTargetSize.height() <= 0 || !std::isfinite(o.dpr) || o.dpr <= 0 ||
        o.fitMode.isEmpty() || o.schemaVersion == 0 || o.resamplerVersion == 0 || int(o.scaleAlgorithm) < int(ImageScaleAlgorithm::QtFast) ||
        int(o.scaleAlgorithm) > int(ImageScaleAlgorithm::Lanczos4))
        return Result<RenderKey>::failure(ImageError::InvalidRequest);

    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    configure(stream);
    stream << QByteArray("aster/render/v3") << source.digest << qint32(o.physicalTargetSize.width()) << qint32(o.physicalTargetSize.height()) << o.dpr
           << o.fitMode << o.schemaVersion << quint32(o.scaleAlgorithm) << o.resamplerVersion << quint32(o.processors.size());

    for (const auto& p : o.processors) {
        if (p.identifier.isEmpty() || p.version == 0)
            return Result<RenderKey>::failure(ImageError::InvalidRequest);
        stream << p.identifier << p.version << p.parameters;
    }

    stream << quint32(o.transformations.size());
    for (const auto& transformation : o.transformations) {
        if (!transformation)
            return Result<RenderKey>::failure(ImageError::InvalidRequest);
        const auto& identity = transformation->identity();
        stream << identity.identifier << identity.version << identity.parameters;
    }

#if ASTER_LANCZOS_USE_LUT
    if (o.scaleAlgorithm == ImageScaleAlgorithm::Lanczos3 || o.scaleAlgorithm == ImageScaleAlgorithm::Lanczos4)
        stream << QByteArray("lanczos-lut4096-q30-v1");
#endif
    return Result<RenderKey>::success({source, digest(bytes)});
}

Result<SourceKey> KeyBuilder::resource(const QString& path, const KeyContext& c) const {
    const auto normalized = QDir::cleanPath(path);
    if (!normalized.startsWith(":/"))
        return Result<SourceKey>::failure(ImageError::InvalidRequest);

    const QFileInfo file(normalized);
    if (!file.exists() || !file.isFile())
        return Result<SourceKey>::failure(ImageError::IoError, "Resource is unavailable");

    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    configure(stream);
    stream << QByteArray("aster/source/resource/v1");
    context(stream, c);
    stream << normalized << file.size() << file.lastModified().toMSecsSinceEpoch();
    return Result<SourceKey>::success({digest(bytes), namespaceDigest(c.nameSpace)});
}
} // namespace aster::cache
