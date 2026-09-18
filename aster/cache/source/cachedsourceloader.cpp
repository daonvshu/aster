#include "cachedsourceloader.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QSharedPointer>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aster::cache {
void CachedSourceLoader::trimMemory(bool critical) {
    if (encoded_)
        encoded_->trim(critical ? 0 : encoded_->stats().maxBytes / 2);
}

namespace {
HttpHeaders normalized(const HttpHeaders& headers) {
    HttpHeaders result;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        result.insert(it.key().toLower(), it.value());
    return result;
}

bool memoryRead(CacheReadPolicy p) {
    return p != CacheReadPolicy::NoCache && p != CacheReadPolicy::BypassMemory;
}

bool diskRead(CacheReadPolicy p) {
    return p != CacheReadPolicy::NoCache && p != CacheReadPolicy::BypassDisk && p != CacheReadPolicy::BypassMemory;
}

bool memoryWrite(CacheWritePolicy p) {
    return p == CacheWritePolicy::Default || p == CacheWritePolicy::MemoryOnly;
}

bool diskWrite(CacheWritePolicy p) {
    return p == CacheWritePolicy::Default || p == CacheWritePolicy::DiskOnly;
}

QMap<QByteArray, QByteArray> directives(const QByteArray& value) {
    QMap<QByteArray, QByteArray> result;
    for (auto part : value.toLower().split(',')) {
        const auto equal = part.indexOf('=');
        result.insert((equal < 0 ? part : part.left(equal)).trimmed(), equal < 0 ? QByteArray() : part.mid(equal + 1).trimmed().replace("\"", ""));
    }
    return result;
}

SourcePayload payload(const QByteArray& bytes, HttpHeaders headers, const QDateTime& now) {
    SourcePayload result;
    result.bytes = bytes;
    result.headers = normalized(headers);
    result.contentType = result.headers.value("content-type").split(';').value(0).trimmed().toLower();
    const auto controls = directives(result.headers.value("cache-control"));
    result.noStore = controls.contains("no-store") || result.headers.value("vary").trimmed() == "*";
    result.mustRevalidate = controls.contains("must-revalidate") || controls.contains("no-cache");
    qint64 ttl = 0;
    bool ok = false;
    const auto maxAge = controls.value("max-age").toLongLong(&ok);
    if (ok && maxAge >= 0) {
        const auto age = std::max<qint64>(0, result.headers.value("age").toLongLong());
        const auto date = QDateTime::fromString(QString::fromLatin1(result.headers.value("date")), Qt::RFC2822Date);
        const auto apparent = date.isValid() ? std::max<qint64>(0, date.secsTo(now)) : 0;
        ttl = std::max<qint64>(0, std::min<qint64>(maxAge, 315360000) - std::max(age, apparent));
    } else {
        const auto expires = QDateTime::fromString(QString::fromLatin1(result.headers.value("expires")), Qt::RFC2822Date);
        if (expires.isValid())
            ttl = std::max<qint64>(0, now.secsTo(expires));
    }
    if (controls.contains("no-cache"))
        ttl = 0;
    result.expiresAt = now.addSecs(ttl);
    return result;
}

DiskEntry serialize(const SourcePayload& value) {
    QJsonObject headers;
    for (auto it = value.headers.cbegin(); it != value.headers.cend(); ++it) {
        if (it.key() == "set-cookie")
            continue;
        headers.insert(QString::fromLatin1(it.key()), QString::fromLatin1(it.value().toBase64()));
    }
    QJsonObject meta;
    meta.insert("http", 1);
    meta.insert("headers", headers);
    meta.insert("expires", QString::number(value.expiresAt->toMSecsSinceEpoch()));
    return {value.bytes, meta};
}

Result<SourcePayload> deserialize(const DiskEntry& entry, const QDateTime& now) {
    if (entry.metadata.value("http").toInt() != 1 || !entry.metadata.value("headers").isObject())
        return Result<SourcePayload>::failure(ImageError::CacheMiss);
    bool ok = false;
    const auto expires = entry.metadata.value("expires").toString().toLongLong(&ok);
    if (!ok)
        return Result<SourcePayload>::failure(ImageError::CacheMiss);
    HttpHeaders headers;
    const auto object = entry.metadata.value("headers").toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        headers.insert(it.key().toLatin1(), QByteArray::fromBase64(it.value().toString().toLatin1()));
    auto result = payload(entry.bytes, headers, now);
    result.expiresAt = QDateTime::fromMSecsSinceEpoch(expires).toUTC();
    return Result<SourcePayload>::success(std::move(result), CacheResultSource::RawDisk);
}
} // namespace

CachedSourceLoader::CachedSourceLoader(QSharedPointer<EncodedMemoryCache> encoded, QSharedPointer<IDiskCache> disk, QSharedPointer<INetworkService> network,
                                       QSharedPointer<Clock> clock, SourceCacheConfig config)
    : encoded_(std::move(encoded))
    , disk_(std::move(disk))
    , network_(std::move(network))
    , clock_(std::move(clock))
    , config_(std::move(config)) {
    if (!clock_)
        throw std::invalid_argument("clock must not be null");
}

bool CachedSourceLoader::encodedEnabled(ImageSource::Kind kind) const {
    return kind == ImageSource::Kind::Network     ? config_.encodedNetwork
            : kind == ImageSource::Kind::Local    ? config_.encodedLocal
            : kind == ImageSource::Kind::Resource ? config_.encodedResource
                                                  : config_.encodedData;
}

Result<SourceKey> CachedSourceLoader::key(const ImageSource& source) const {
    KeyBuilder builder;
    if (source.kind == ImageSource::Kind::Local)
        return builder.localFile(source.url.toLocalFile(), source.context, source.strictLocalIdentity);
    if (source.kind == ImageSource::Kind::Resource) {
        if (source.url.scheme() != "qrc" || !source.url.authority().isEmpty() || source.url.hasQuery() || source.url.hasFragment())
            return Result<SourceKey>::failure(ImageError::InvalidRequest);

        return builder.resource(":" + source.url.path(), source.context);
    }
    if (source.kind == ImageSource::Kind::Network) {
        auto context = source.context;
        QByteArray variants;
        QDataStream stream(&variants, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_12);
        stream << context.contentVariant;
        const auto headers = normalized(source.headers);
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            if (it.key().contains('\r') || it.key().contains('\n') || it.value().contains('\r') || it.value().contains('\n'))
                return Result<SourceKey>::failure(ImageError::InvalidRequest);
            stream << it.key() << it.value();
        }
        context.contentVariant = QCryptographicHash::hash(variants, QCryptographicHash::Sha256);
        return builder.network(source.url, context);
    }

    if (source.kind != ImageSource::Kind::Data)
        return Result<SourceKey>::failure(ImageError::InvalidRequest);

    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_12);
    const auto& c = source.context;
    stream << QByteArray("aster/data/v1") << c.nameSpace << c.tenant << c.authScope << c.contentVariant << c.revision << source.contentType
           << QCryptographicHash::hash(source.data, QCryptographicHash::Sha256);
    return Result<SourceKey>::success({QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), KeyBuilder::namespaceDigest(c.nameSpace)});
}

Result<SourcePayload> CachedSourceLoader::load(const ImageSource& source, const SourceKey& expected, const SourceLoadOptions& requested,
                                               const std::atomic<bool>& cancelled) {
    using R = Result<SourcePayload>;
    auto options = requested;
    const auto requestControls = directives(normalized(source.headers).value("cache-control"));
    if (source.kind == ImageSource::Kind::Network) {
        if (requestControls.contains("no-store")) {
            if (options.cache.read != CacheReadPolicy::CacheOnly)
                options.cache.read = CacheReadPolicy::NoCache;
            options.cache.write = CacheWritePolicy::NoStore;
        } else if (requestControls.contains("no-cache") || requestControls.value("max-age") == "0") {
            if (options.cache.read != CacheReadPolicy::CacheOnly)
                options.cache.read = CacheReadPolicy::BypassMemory;
        }
    }
    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (options.maxBytes <= 0 || options.maxBytes > std::numeric_limits<int>::max() - 1)
        return R::failure(ImageError::InvalidRequest);
    const auto identity = key(source);
    if (!identity)
        return R::failure(identity.error);
    if (!(*identity.value == expected))
        return R::failure(ImageError::SourceChanged);

    const bool enabled = encoded_ && encodedEnabled(source.kind);
    if (enabled && memoryRead(options.cache.read) && !requestControls.contains("no-store")) {
        auto hit = encoded_->get(expected, options.cache.allowStale);
        if (hit && hit.value->bytes.size() <= options.maxBytes)
            return hit;
    }

    R result;
    if (source.kind == ImageSource::Kind::Network)
        result = network(source, expected, options, cancelled);
    else {
        if (options.cache.read == CacheReadPolicy::CacheOnly)
            return R::failure(ImageError::CacheMiss);
        SourcePayload value;
        value.contentType = source.contentType;
        if (source.kind == ImageSource::Kind::Data) {
            if (source.data.size() > options.maxBytes)
                return R::failure(ImageError::InvalidRequest, "Source exceeds byte limit");
            value.bytes = QByteArray(source.data.constData(), source.data.size());
            result = R::success(std::move(value), CacheResultSource::Data);
        } else {
            const bool resource = source.kind == ImageSource::Kind::Resource;
            QFile file(resource ? ":" + source.url.path() : source.url.toLocalFile());
            if (!file.open(QIODevice::ReadOnly))
                return R::failure(ImageError::IoError);
            if (file.size() > options.maxBytes)
                return R::failure(ImageError::InvalidRequest, "Source exceeds byte limit");
            while (!file.atEnd()) {
                if (cancelled.load())
                    return R::failure(ImageError::Cancelled);
                auto chunk = file.read(std::min<qint64>(65536, options.maxBytes + 1 - value.bytes.size()));
                if (file.error() != QFileDevice::NoError || chunk.isEmpty())
                    return R::failure(ImageError::IoError);
                value.bytes += chunk;
                if (value.bytes.size() > options.maxBytes)
                    return R::failure(ImageError::InvalidRequest);
            }
            const auto after = key(source);
            if (!after || !(*after.value == expected))
                return R::failure(ImageError::SourceChanged);
            if (value.contentType.isEmpty())
                value.contentType = QMimeDatabase().mimeTypeForFile(file.fileName()).name().toLatin1();
            result = R::success(std::move(value), resource ? CacheResultSource::Resource : CacheResultSource::LocalFile);
        }
    }

    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (result && result.value->bytes.isEmpty())
        return R::failure(ImageError::CorruptedEntry, "Empty source");
    if (result && requestControls.contains("no-store"))
        result.value->noStore = true;
    if (result && enabled && memoryWrite(options.cache.write) && !config_.excludedMimeTypes.contains(result.value->contentType))
        encoded_->put(expected, *result.value);
    return result;
}

Result<SourcePayload> CachedSourceLoader::network(const ImageSource& source, const SourceKey& key, const SourceLoadOptions& options,
                                                  const std::atomic<bool>& cancelled) {
    using R = Result<SourcePayload>;
    const auto now = clock_->now();
    R cached = R::failure(ImageError::CacheMiss);
    if (disk_ && options.enableSourceDisk && diskRead(options.cache.read)) {
        try {
            auto entry = disk_->get(key.digest);
            if (entry)
                cached = deserialize(*entry.value, now);
        } catch (...) {
            ++diskExceptions_;
            cached = R::failure(ImageError::CacheMiss);
        }
        if (cached && (cached.value->noStore || cached.value->bytes.size() > options.maxBytes))
            cached = R::failure(ImageError::CacheMiss);
        if (cached && cached.value->expiresAt && now < *cached.value->expiresAt)
            return cached;
    }
    if (options.cache.read == CacheReadPolicy::CacheOnly) {
        if (cached && options.cache.allowStale && !cached.value->mustRevalidate)
            return cached;
        return R::failure(ImageError::CacheMiss);
    }
    if (!network_)
        return R::failure(ImageError::IoError, "Network service is not configured");

    NetworkFetchOptions fetch;
    fetch.headers = normalized(source.headers);
    fetch.maxBytes = options.maxBytes;
    fetch.reload = options.cache.read == CacheReadPolicy::BypassMemory || options.cache.read == CacheReadPolicy::NoCache;
    fetch.preferCache = !fetch.reload;
    fetch.validation = bool(cached);
    if (cached) {
        const auto& headers = cached.value->headers;
        if (headers.contains("etag"))
            fetch.headers.insert("if-none-match", headers.value("etag"));
        else if (headers.contains("last-modified"))
            fetch.headers.insert("if-modified-since", headers.value("last-modified"));
    }
    const auto started = LookupTimer::Clock::now();
    Result<NetworkResponse> response;
    try {
        response = network_->fetch(source.url, fetch, cancelled);
    } catch (...) {
        response = Result<NetworkResponse>::failure(ImageError::IoError);
    }
    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        LookupTimer timer(networkStats_.latency, started);
        ++networkStats_.requests;
        if (response) {
            ++networkStats_.responses;
            networkStats_.receivedBytes += quint64(response.value->body.size());
            if (response.value->status == 304)
                ++networkStats_.validations;
        }
        if (!response || (response.value->status != 200 && response.value->status != 304))
            ++networkStats_.errors;
    }
    if (cancelled.load())
        return R::failure(ImageError::Cancelled);
    if (!response || response.value->status >= 500) {
        if (cached && options.cache.allowStale && !cached.value->mustRevalidate)
            return cached;
        return R::failure(response ? ImageError::IoError : response.error, "Network fetch failed");
    }

    SourcePayload value;
    auto origin = CacheResultSource::Network;
    if (response.value->status == 304) {
        if (!cached)
            return R::failure(ImageError::CorruptedEntry, "304 without cached representation");
        auto headers = cached.value->headers;
        headers.remove("age");
        headers.remove("date");
        const auto updates = normalized(response.value->headers);
        for (auto it = updates.cbegin(); it != updates.cend(); ++it)
            headers.insert(it.key(), it.value());
        value = payload(cached.value->bytes, headers, clock_->now());
        origin = CacheResultSource::Validated;
    } else if (response.value->status == 200) {
        if (response.value->body.size() > options.maxBytes)
            return R::failure(ImageError::InvalidRequest, "Source exceeds byte limit");
        value = payload(response.value->body, response.value->headers, clock_->now());
    } else
        return R::failure(ImageError::IoError, "Unexpected HTTP status");

    if (value.bytes.isEmpty())
        return R::failure(ImageError::CorruptedEntry);
    if (value.noStore) {
        if (disk_) {
            try {
                disk_->remove(key.digest);
            } catch (...) {
                ++diskExceptions_;
            }
        }
        if (encoded_)
            encoded_->remove(key);
    } else if (disk_ && options.enableSourceDisk && diskWrite(options.cache.write)) {
        try {
            auto entry = serialize(value);
            entry.metadata.insert("sourceDigest", QString::fromLatin1(key.digest.toHex()));
            entry.metadata.insert("namespaceDigest", QString::fromLatin1(key.namespaceDigest.toHex()));
            disk_->put(key.digest, entry);
        } catch (...) {
            ++diskExceptions_;
        }
    }
    return R::success(std::move(value), origin);
}

SourceCacheStats CachedSourceLoader::cacheStats() const {
    SourceCacheStats result;
    if (encoded_)
        result.encodedMemory = encoded_->stats();
    if (disk_)
        result.rawDisk = disk_->stats();
    result.rawDisk.ioErrors += diskExceptions_.load();
    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        result.network = networkStats_;
    }
    return result;
}

bool CachedSourceLoader::invalidate(const CacheSelector& selector) {
    if (!selector.valid())
        return false;

    bool success = true;
    if (encoded_)
        success = encoded_->invalidate(selector);
    if (disk_) {
        auto source = selector;
        source.kind = CacheSelector::Kind::Source;
        success = disk_->invalidate(source) && success;
    }
    return success;
}

bool CachedSourceLoader::clearDiskCache(const std::atomic<bool>* cancelled) {
    if (!disk_)
        return true;
    try {
        return disk_->clear(cancelled);
    } catch (...) {
        ++diskExceptions_;
        return false;
    }
}
} // namespace aster::cache
