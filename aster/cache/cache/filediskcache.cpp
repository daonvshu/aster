#include "filediskcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSharedPointer>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aster::cache {
namespace {
constexpr qint64 headerSize = 8 + 8 + 32;
constexpr qint64 maxMetadata = 64 * 1024;
const QByteArray magic("ASTERD01", 8);

bool cancelled(const std::atomic<bool>* token) {
    return token && token->load();
}
} // namespace

FileDiskCache::FileDiskCache(QString root, qint64 budget, qint64 maxEntry, QSharedPointer<Clock> clock, WriteCheckpoint checkpoint, QString directoryVersion,
                             bool appendVersionDirectory)
    : root_(appendVersionDirectory ? QDir(root).absoluteFilePath("aster-cache-" + directoryVersion) : QDir(root).absolutePath())
    , maxEntry_(std::max<qint64>(0, std::min<qint64>(maxEntry, std::numeric_limits<int>::max() - maxMetadata - headerSize)))
    , clock_(std::move(clock))
    , checkpoint_(std::move(checkpoint)) {
    if (root.isEmpty() || !clock_ || !QRegularExpression("\\A[A-Za-z0-9_-]{1,64}\\z").match(directoryVersion).hasMatch())
        throw std::invalid_argument("Invalid disk cache configuration");
    if (!QDir().mkpath(root_))
        throw std::invalid_argument("Could not create disk cache directory");
    stats_.maxBytes = std::max<qint64>(0, budget);
    if (!scan(nullptr))
        throw std::invalid_argument("Could not scan disk cache directory");
}

QString FileDiskCache::path(const QByteArray& key) const {
    if (key.size() != 32)
        return {};
    const auto hex = QString::fromLatin1(key.toHex());
    return QDir(root_).filePath(hex.left(2) + "/" + hex + ".entry");
}

void FileDiskCache::remember(const QByteArray& key, qint64 cost, qint64 accessed) {
    const auto old = index_.constFind(key);
    if (old != index_.cend())
        stats_.totalBytes -= old->cost;
    else
        ++stats_.entryCount;
    index_.insert(key, {cost, accessed, true});
    stats_.totalBytes += cost;
}

bool FileDiskCache::erase(const QByteArray& key) {
    const auto file = path(key);
    if (file.isEmpty())
        return false;
    if (QFileInfo::exists(file) && !QFile::remove(file)) {
        ++stats_.ioErrors;
        return false;
    }
    auto found = index_.find(key);
    if (found != index_.end()) {
        stats_.totalBytes -= found->cost;
        --stats_.entryCount;
        index_.erase(found);
    }
    return true;
}

Result<DiskEntry> FileDiskCache::get(const QByteArray& key) {
    const auto started = LookupTimer::Clock::now();
    const auto now = clock_->now().toMSecsSinceEpoch();
    std::lock_guard<std::mutex> lock(mutex_);
    LookupTimer timer(stats_.lookup, started);
    return readLocked(key, now, true);
}

Result<DiskEntry> FileDiskCache::readLocked(const QByteArray& key, qint64 now, bool track) {
    const auto filename = path(key);
    QFile file(filename);
    if (filename.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        if (!filename.isEmpty() && !QFileInfo::exists(filename))
            erase(key);
        else if (!filename.isEmpty())
            ++stats_.ioErrors;
        if (track)
            ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    }

    auto corrupt = [&]() {
        file.close();
        erase(key);
        ++stats_.corruptions;
        if (track)
            ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    };
    const auto size = file.size();
    if (size < headerSize || size > maxEntry_ + maxMetadata + headerSize)
        return corrupt();
    const auto header = file.read(headerSize);
    if (header.size() != headerSize || header.left(8) != magic)
        return corrupt();
    const auto metaSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 8));
    const auto dataSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 12));
    if (metaSize > maxMetadata || dataSize > maxEntry_ || headerSize + metaSize + dataSize != size)
        return corrupt();
    const auto body = file.readAll();
    if (body.size() != size - headerSize || QCryptographicHash::hash(body, QCryptographicHash::Sha256) != header.mid(16, 32))
        return corrupt();
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(body.left(int(metaSize)), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return corrupt();
    auto metadata = doc.object();
    if (metadata.value("key").toString().toLatin1() != key.toHex())
        return corrupt();
    metadata.remove("key");
    if (track) {
        remember(key, size, now);
        ++stats_.hits;
    }
    return Result<DiskEntry>::success({body.mid(int(metaSize)), metadata}, CacheResultSource::RawDisk);
}

bool FileDiskCache::scan(const std::atomic<bool>* token) {
    if (stats_.indexComplete)
        return true;
    QDirIterator files(root_, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        if (cancelled(token))
            return false;
        files.next();
        const auto info = files.fileInfo();
        const auto name = info.fileName();
        if (name.size() != 70 || !name.endsWith(".entry")) {
            // QSaveFile leftovers are disposable inside this dedicated cache directory.
            if (name.contains(".entry."))
                QFile::remove(info.absoluteFilePath());
            continue;
        }
        const auto hex = name.left(64).toLatin1();
        const auto key = QByteArray::fromHex(hex);
        if (key.size() != 32 || key.toHex() != hex || info.absoluteFilePath() != path(key))
            continue;
        if (!index_.contains(key))
            remember(key, info.size(), info.lastModified().toMSecsSinceEpoch());
    }
    stats_.indexComplete = true;
    return true;
}

bool FileDiskCache::trimLocked(qint64 target, const std::atomic<bool>* token) {
    if (!scan(token))
        return false;
    if (stats_.totalBytes <= target)
        return true;
    auto keys = index_.keys();
    std::sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) { return index_.value(a).accessed < index_.value(b).accessed; });
    for (const auto& key : keys) {
        if (cancelled(token))
            return false;
        if (stats_.totalBytes <= target)
            break;
        if (!erase(key))
            return false;
        ++stats_.evictions;
    }
    return stats_.totalBytes <= target;
}

bool FileDiskCache::put(const QByteArray& key, const DiskEntry& entry) {
    const auto now = clock_->now().toMSecsSinceEpoch();
    if (key.size() != 32 || entry.bytes.size() > maxEntry_)
        return false;
    auto metadata = entry.metadata;
    metadata.insert("key", QString::fromLatin1(key.toHex()));
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (json.size() > maxMetadata)
        return false;
    const auto cost = headerSize + qint64(json.size()) + entry.bytes.size();

    std::lock_guard<std::mutex> lock(mutex_);
    if (stats_.maxBytes == 0 || cost > stats_.maxBytes)
        return false;
    if (!scan(nullptr))
        return false;
    const auto filename = path(key);
    if (!QDir().mkpath(QFileInfo(filename).absolutePath())) {
        ++stats_.ioErrors;
        return false;
    }
    QByteArray body = json + entry.bytes;
    QByteArray header = magic;
    char sizes[8];
    qToBigEndian<quint32>(quint32(json.size()), reinterpret_cast<uchar*>(sizes));
    qToBigEndian<quint32>(quint32(entry.bytes.size()), reinterpret_cast<uchar*>(sizes + 4));
    header.append(sizes, 8);
    header += QCryptographicHash::hash(body, QCryptographicHash::Sha256);
    const auto bytes = header + body;
    QSaveFile file(filename);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        ++stats_.ioErrors;
        return false;
    }
    auto checkpoint = [&](WriteStage stage) { return !checkpoint_ || checkpoint_(stage); };
    const qint64 first = bytes.size() / 10;
    const qint64 half = bytes.size() / 2;
    try {
        if (file.write(bytes.constData(), first) != first || !checkpoint(WriteStage::TenPercent) ||
            file.write(bytes.constData() + first, half - first) != half - first || !checkpoint(WriteStage::Half) ||
            file.write(bytes.constData() + half, bytes.size() - half) != bytes.size() - half || !checkpoint(WriteStage::BeforeCommit)) {
            file.cancelWriting();
            ++stats_.ioErrors;
            return false;
        }
    } catch (...) {
        file.cancelWriting();
        ++stats_.ioErrors;
        return false;
    }
    // QSaveFile flushes/syncs and atomically replaces one metadata+payload envelope.
    if (!file.commit()) {
        ++stats_.ioErrors;
        return false;
    }
    remember(key, cost, now);
    return trimLocked(stats_.maxBytes, nullptr);
}

bool FileDiskCache::remove(const QByteArray& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return erase(key);
}

bool FileDiskCache::contains(const QByteArray& key) {
    return bool(get(key));
}

bool FileDiskCache::invalidate(const CacheSelector& selector, const std::atomic<bool>* token) {
    if (!selector.valid())
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!scan(token))
        return false;

    bool success = true;
    const auto keys = index_.keys();
    for (const auto& key : keys) {
        if (cancelled(token))
            return false;

        const auto entry = readLocked(key, 0, false);
        if (!entry) {
            if (QFileInfo::exists(path(key)))
                success = false;
            continue;
        }

        const auto& meta = entry.value->metadata;
        const bool rendered = meta.contains("encoderVersion");
        if ((selector.kind == CacheSelector::Kind::Rendered && !rendered) || (selector.kind == CacheSelector::Kind::Source && rendered))
            continue;

        const SourceKey source{QByteArray::fromHex(meta.value("sourceDigest").toString().toLatin1()),
                               QByteArray::fromHex(meta.value("namespaceDigest").toString().toLatin1())};
        // Legacy image envelopes lack tags; discard them conservatively during maintenance.
        const bool legacy = (meta.contains("http") || meta.contains("encoderVersion")) && (source.digest.size() != 32 || source.namespaceDigest.size() != 32);
        if (legacy || selector.matches(source) || (!selector.sourceDigest.isEmpty() && key == selector.sourceDigest))
            success = erase(key) && success;
    }
    return success;
}

bool FileDiskCache::clear(const std::atomic<bool>* token) {
    return trim(0, token);
}

bool FileDiskCache::trim(qint64 bytes, const std::atomic<bool>* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    return trimLocked(std::max<qint64>(0, bytes), token);
}

void FileDiskCache::setMaxCost(qint64 bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.maxBytes = std::max<qint64>(0, bytes);
    trimLocked(stats_.maxBytes, nullptr);
}

bool FileDiskCache::recover(const std::atomic<bool>* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.indexComplete = false;
    index_.clear();
    stats_.totalBytes = stats_.entryCount = 0;
    return scan(token) && trimLocked(stats_.maxBytes, token);
}

DiskStats FileDiskCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool FileDiskCache::flushAccessTimes(const std::atomic<bool>* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool success = true;
    for (auto it = index_.begin(); it != index_.end(); ++it) {
        if (cancelled(token))
            return false;
        if (!it->dirty)
            continue;
        QFile file(path(it.key()));
        if (!file.open(QIODevice::ReadWrite) || !file.setFileTime(QDateTime::fromMSecsSinceEpoch(it->accessed).toUTC(), QFileDevice::FileModificationTime)) {
            ++stats_.ioErrors;
            success = false;
        } else
            it->dirty = false;
    }
    return success;
}
} // namespace aster::cache
