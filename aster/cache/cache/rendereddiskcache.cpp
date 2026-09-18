#include "rendereddiskcache.h"

#include "aster/cache/decoder/boundedimagedecoder.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QImageReader>
#include <QImageWriter>
#include <QSharedPointer>

#include <cmath>
#include <stdexcept>

namespace aster::cache {
RenderedDiskCache::RenderedDiskCache(QSharedPointer<IDiskCache> disk, RenderedDiskConfig config)
    : disk_(std::move(disk))
    , config_(std::move(config)) {
    config_.format = config_.format.toLower();
    if (!disk_ || config_.maxPixels <= 0 || config_.encoderVersion == 0 || config_.quality < -1 || config_.maxSide <= 0 || config_.maxDecodedBytes <= 0 ||
        config_.maxEncodedBytes <= 0 || config_.quality > 100 || !QImageWriter::supportedImageFormats().contains(config_.format))
        throw std::invalid_argument("Invalid rendered disk configuration");
}

QByteArray RenderedDiskCache::storageKey(const RenderKey& key) const {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_12);
    stream << QByteArray("aster/rendered-disk/v1") << key.source.digest << key.digest << config_.format << qint32(config_.quality) << config_.encoderVersion;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

ImageResult RenderedDiskCache::get(const RenderKey& key) {
    const auto started = LookupTimer::Clock::now();
    auto result = read(key);
    std::lock_guard<std::mutex> lock(metricsMutex_);
    LookupTimer timer(metrics_.lookup, started);
    if (result)
        ++metrics_.hits;
    else
        ++metrics_.misses;
    return result;
}

ImageResult RenderedDiskCache::read(const RenderKey& key) {
    if (key.digest.size() != 32 || key.source.digest.size() != 32)
        return ImageResult::failure(ImageError::CacheMiss);

    try {
        const auto stored = storageKey(key);
        auto entry = disk_->get(stored);
        if (!entry)
            return ImageResult::failure(ImageError::CacheMiss);
        auto invalid = [&] {
            disk_->remove(stored);
            std::lock_guard<std::mutex> lock(metricsMutex_);
            ++metrics_.corruptions;
            return ImageResult::failure(ImageError::CacheMiss);
        };
        const auto& meta = entry.value->metadata;
        const QSize size(meta.value("width").toInt(), meta.value("height").toInt());
        const auto dpr = meta.value("dpr").toDouble(0);
        if (meta.value("schema").toInt() != 1 || meta.value("encoderVersion").toDouble() != double(config_.encoderVersion) ||
            meta.value("format").toString().toLatin1() != config_.format || size.width() <= 0 || size.height() <= 0 ||
            qint64(size.width()) * size.height() > config_.maxPixels || !std::isfinite(dpr) || dpr <= 0)
            return invalid();

        DecodeLimits limits;
        limits.maxEncodedBytes = config_.maxEncodedBytes;
        limits.maxSide = config_.maxSide;
        limits.maxPixels = config_.maxPixels;
        limits.maxDecodedBytes = config_.maxDecodedBytes;
        limits.allowedFormats = {config_.format == "jpg" ? QByteArray("jpeg") : config_.format};
        const std::atomic<bool> cancelled{false};
        auto decoded = BoundedImageDecoder(limits).decode(entry.value->bytes, cancelled, size);
        if (!decoded)
            return invalid();
        auto image = std::move(*decoded.value);
        image.setDevicePixelRatio(dpr);
        return ImageResult::success(std::move(image), CacheResultSource::RenderedDisk);
    } catch (...) {
        return ImageResult::failure(ImageError::CacheMiss);
    }
}

bool RenderedDiskCache::put(const RenderKey& key, const QImage& image) {
    if (key.digest.size() != 32 || key.source.digest.size() != 32 || image.isNull() || image.width() > config_.maxSide || image.height() > config_.maxSide ||
        qint64(image.width()) * image.height() > config_.maxPixels || qint64(image.width()) * image.height() > config_.maxDecodedBytes / 16 ||
        !std::isfinite(image.devicePixelRatio()) || image.devicePixelRatio() <= 0)
        return false;

    try {
        DiskEntry entry;
        QBuffer buffer(&entry.bytes);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, config_.format);
        writer.setQuality(config_.quality);
        if (!writer.write(image) || entry.bytes.size() > config_.maxEncodedBytes)
            return false;
        entry.metadata = {{"schema", 1},
                          {"sourceDigest", QString::fromLatin1(key.source.digest.toHex())},
                          {"namespaceDigest", QString::fromLatin1(key.source.namespaceDigest.toHex())},
                          {"format", QString::fromLatin1(config_.format)},
                          {"width", image.width()},
                          {"height", image.height()},
                          {"dpr", image.devicePixelRatio()},
                          {"encoderVersion", double(config_.encoderVersion)}};
        return disk_->put(storageKey(key), entry);
    } catch (...) {
        return false;
    }
}

bool RenderedDiskCache::remove(const RenderKey& key) {
    try {
        return disk_->remove(storageKey(key));
    } catch (...) {
        return false;
    }
}

DiskStats RenderedDiskCache::stats() const {
    auto result = disk_->stats();
    std::lock_guard<std::mutex> lock(metricsMutex_);
    result.hits = metrics_.hits;
    result.misses = metrics_.misses;
    result.corruptions += metrics_.corruptions;
    result.lookup = metrics_.lookup;
    return result;
}

bool RenderedDiskCache::invalidate(const CacheSelector& selector) {
    auto rendered = selector;
    rendered.kind = CacheSelector::Kind::Rendered;
    return disk_->invalidate(rendered);
}

bool RenderedDiskCache::clear(const std::atomic<bool>* cancelled) {
    try {
        return disk_->clear(cancelled);
    } catch (...) {
        return false;
    }
}
} // namespace aster::cache
