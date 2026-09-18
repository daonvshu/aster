#include "sqlitediskcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace aster::cache {
namespace {
constexpr qint64 maxMetadata = 64 * 1024;

bool cancelled(const std::atomic<bool>* token) {
    return token && token->load();
}

QString connectionName() {
    static std::atomic<quint64> next{0};
    return QString("aster_sqlite_%1").arg(++next);
}

class ScopedDatabase {
public:
    explicit ScopedDatabase(const QString& path)
        : name_(connectionName())
        , database_(QSqlDatabase::addDatabase("QSQLITE", name_)) {
        database_.setDatabaseName(path);
        opened_ = database_.open();
    }

    ~ScopedDatabase() {
        if (database_.isOpen())
            database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    ScopedDatabase(const ScopedDatabase&) = delete;
    ScopedDatabase& operator=(const ScopedDatabase&) = delete;

    bool isOpen() const {
        return opened_;
    }

    QSqlDatabase& database() {
        return database_;
    }

private:
    QString name_;
    QSqlDatabase database_;
    bool opened_ = false;
};

bool exec(QSqlDatabase& database, const QString& sql) {
    QSqlQuery query(database);
    return query.exec(sql);
}

bool createSchema(QSqlDatabase& database) {
    return exec(database, "PRAGMA journal_mode=WAL") && exec(database, "PRAGMA synchronous=NORMAL") &&
            exec(database,
                 "CREATE TABLE IF NOT EXISTS entries ("
                 "key BLOB PRIMARY KEY NOT NULL,"
                 "bytes BLOB NOT NULL,"
                 "metadata TEXT NOT NULL,"
                 "checksum BLOB NOT NULL,"
                 "cost INTEGER NOT NULL,"
                 "accessed INTEGER NOT NULL)") &&
            exec(database, "CREATE INDEX IF NOT EXISTS entries_accessed ON entries(accessed)");
}

bool validKey(const QByteArray& key) {
    return key.size() == 32;
}
} // namespace

SqliteDiskCache::SqliteDiskCache(QString databasePath, qint64 budget, qint64 maxEntry, QSharedPointer<Clock> clock)
    : databasePath_(QFileInfo(databasePath).absoluteFilePath())
    , maxEntry_(std::max<qint64>(0, std::min<qint64>(maxEntry, std::numeric_limits<int>::max())))
    , clock_(std::move(clock))
    , maxBytes_(std::max<qint64>(0, budget)) {
    if (databasePath.isEmpty() || !clock_)
        throw std::invalid_argument("Invalid SQLite disk cache configuration");
    if (!QDir().mkpath(QFileInfo(databasePath_).absolutePath()))
        throw std::invalid_argument("Could not create SQLite disk cache directory");
    ScopedDatabase database(databasePath_);
    if (!database.isOpen() || !createSchema(database.database()))
        throw std::invalid_argument("Could not initialize SQLite disk cache");
    refreshStatsLocked(database.database());
}

bool SqliteDiskCache::refreshStatsLocked(QSqlDatabase& database) const {
    QSqlQuery query(database);
    if (!query.exec("SELECT COUNT(*), COALESCE(SUM(cost), 0) FROM entries") || !query.next())
        return false;
    stats_.entryCount = query.value(0).toLongLong();
    stats_.totalBytes = query.value(1).toLongLong();
    stats_.maxBytes = maxBytes_;
    stats_.indexComplete = true;
    return true;
}

Result<DiskEntry> SqliteDiskCache::get(const QByteArray& key) {
    const auto started = LookupTimer::Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    LookupTimer timer(stats_.lookup, started);
    ScopedDatabase database(databasePath_);
    if (!database.isOpen()) {
        ++stats_.ioErrors;
        ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    }
    if (!validKey(key)) {
        ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    }

    QSqlQuery query(database.database());
    query.prepare("SELECT bytes, metadata, checksum, cost FROM entries WHERE key = ?");
    query.addBindValue(key);
    if (!query.exec()) {
        ++stats_.ioErrors;
        ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    }
    if (!query.next()) {
        ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    }

    const auto bytes = query.value(0).toByteArray();
    const auto metadataBytes = query.value(1).toString().toUtf8();
    const auto checksum = query.value(2).toByteArray();
    const auto cost = query.value(3).toLongLong();
    QJsonParseError parseError;
    const auto metadataDocument = QJsonDocument::fromJson(metadataBytes, &parseError);
    const bool valid = parseError.error == QJsonParseError::NoError && metadataDocument.isObject() && cost >= 0 &&
            cost == qint64(bytes.size()) + metadataBytes.size() + 32 && QCryptographicHash::hash(metadataBytes + bytes, QCryptographicHash::Sha256) == checksum;
    if (!valid) {
        removeLocked(database.database(), key);
        ++stats_.corruptions;
        ++stats_.misses;
        return Result<DiskEntry>::failure(ImageError::CacheMiss);
    }

    QSqlQuery touch(database.database());
    touch.prepare("UPDATE entries SET accessed = ? WHERE key = ?");
    touch.addBindValue(clock_->now().toMSecsSinceEpoch());
    touch.addBindValue(key);
    if (!touch.exec())
        ++stats_.ioErrors;
    ++stats_.hits;
    return Result<DiskEntry>::success({bytes, metadataDocument.object()}, CacheResultSource::RawDisk);
}

bool SqliteDiskCache::removeLocked(QSqlDatabase& database, const QByteArray& key, bool countError) {
    QSqlQuery query(database);
    query.prepare("DELETE FROM entries WHERE key = ?");
    query.addBindValue(key);
    if (!query.exec()) {
        if (countError)
            ++stats_.ioErrors;
        return false;
    }
    return true;
}

bool SqliteDiskCache::put(const QByteArray& key, const DiskEntry& entry) {
    if (!validKey(key) || entry.bytes.size() > maxEntry_)
        return false;
    const auto metadataBytes = QJsonDocument(entry.metadata).toJson(QJsonDocument::Compact);
    if (metadataBytes.size() > maxMetadata)
        return false;
    const auto cost = qint64(entry.bytes.size()) + metadataBytes.size() + 32;
    if (maxBytes_ == 0 || cost > maxBytes_)
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    ScopedDatabase database(databasePath_);
    if (!database.isOpen()) {
        ++stats_.ioErrors;
        return false;
    }
    auto& sql = database.database();
    if (!sql.transaction()) {
        ++stats_.ioErrors;
        return false;
    }
    QSqlQuery query(sql);
    query.prepare("INSERT OR REPLACE INTO entries(key, bytes, metadata, checksum, cost, accessed) VALUES(?, ?, ?, ?, ?, ?)");
    query.addBindValue(key);
    query.addBindValue(entry.bytes);
    query.addBindValue(QString::fromUtf8(metadataBytes));
    query.addBindValue(QCryptographicHash::hash(metadataBytes + entry.bytes, QCryptographicHash::Sha256));
    query.addBindValue(cost);
    query.addBindValue(clock_->now().toMSecsSinceEpoch());
    quint64 evictions = 0;
    if (!query.exec() || !trimLocked(sql, maxBytes_, nullptr, &evictions) || !sql.commit()) {
        sql.rollback();
        ++stats_.ioErrors;
        return false;
    }
    stats_.evictions += evictions;
    refreshStatsLocked(sql);
    return true;
}

bool SqliteDiskCache::trimLocked(QSqlDatabase& database, qint64 target, const std::atomic<bool>* token, quint64* evictions) {
    QSqlQuery query(database);
    if (!query.exec("SELECT key FROM entries ORDER BY accessed ASC"))
        return false;
    while (query.next()) {
        if (cancelled(token))
            return false;
        QSqlQuery total(database);
        if (!total.exec("SELECT COALESCE(SUM(cost), 0) FROM entries") || !total.next())
            return false;
        if (total.value(0).toLongLong() <= target)
            return true;
        if (!removeLocked(database, query.value(0).toByteArray()))
            return false;
        if (evictions)
            ++*evictions;
    }
    QSqlQuery total(database);
    return total.exec("SELECT COALESCE(SUM(cost), 0) FROM entries") && total.next() && total.value(0).toLongLong() <= target;
}

bool SqliteDiskCache::remove(const QByteArray& key) {
    if (!validKey(key))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    ScopedDatabase database(databasePath_);
    if (!database.isOpen()) {
        ++stats_.ioErrors;
        return false;
    }
    if (!removeLocked(database.database(), key))
        return false;
    refreshStatsLocked(database.database());
    return true;
}

bool SqliteDiskCache::contains(const QByteArray& key) {
    return bool(get(key));
}

bool SqliteDiskCache::clear(const std::atomic<bool>* token) {
    return trim(0, token);
}

bool SqliteDiskCache::trim(qint64 bytes, const std::atomic<bool>* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    ScopedDatabase database(databasePath_);
    if (!database.isOpen()) {
        ++stats_.ioErrors;
        return false;
    }
    auto& sql = database.database();
    if (!sql.transaction()) {
        ++stats_.ioErrors;
        return false;
    }
    quint64 evictions = 0;
    if (!trimLocked(sql, std::max<qint64>(0, bytes), token, &evictions) || !sql.commit()) {
        sql.rollback();
        return false;
    }
    stats_.evictions += evictions;
    refreshStatsLocked(sql);
    return true;
}

DiskStats SqliteDiskCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ScopedDatabase database(databasePath_);
    if (database.isOpen())
        refreshStatsLocked(database.database());
    else
        ++stats_.ioErrors;
    return stats_;
}

void SqliteDiskCache::setMaxCost(qint64 bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxBytes_ = std::max<qint64>(0, bytes);
    stats_.maxBytes = maxBytes_;
    ScopedDatabase database(databasePath_);
    if (database.isOpen()) {
        auto& sql = database.database();
        quint64 evictions = 0;
        if (sql.transaction() && trimLocked(sql, maxBytes_, nullptr, &evictions) && sql.commit()) {
            stats_.evictions += evictions;
            refreshStatsLocked(sql);
        } else {
            sql.rollback();
        }
    }
}

bool SqliteDiskCache::recover() {
    std::lock_guard<std::mutex> lock(mutex_);
    ScopedDatabase database(databasePath_);
    if (!database.isOpen() || !createSchema(database.database()))
        return false;
    return refreshStatsLocked(database.database());
}

bool SqliteDiskCache::invalidate(const CacheSelector& selector, const std::atomic<bool>* token) {
    if (!selector.valid())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    ScopedDatabase database(databasePath_);
    if (!database.isOpen())
        return false;
    auto& sql = database.database();
    if (!sql.transaction())
        return false;
    QSqlQuery query(sql);
    if (!query.exec("SELECT key, metadata FROM entries")) {
        sql.rollback();
        return false;
    }
    while (query.next()) {
        if (cancelled(token)) {
            sql.rollback();
            return false;
        }
        QJsonParseError error;
        const auto metadata = QJsonDocument::fromJson(query.value(1).toString().toUtf8(), &error).object();
        if (error.error != QJsonParseError::NoError)
            continue;
        const bool rendered = metadata.contains("encoderVersion");
        if ((selector.kind == CacheSelector::Kind::Rendered && !rendered) || (selector.kind == CacheSelector::Kind::Source && rendered))
            continue;
        const SourceKey source{QByteArray::fromHex(metadata.value("sourceDigest").toString().toLatin1()),
                               QByteArray::fromHex(metadata.value("namespaceDigest").toString().toLatin1())};
        const bool legacy = (metadata.contains("http") || rendered) && (source.digest.size() != 32 || source.namespaceDigest.size() != 32);
        if (legacy || selector.matches(source))
            removeLocked(sql, query.value(0).toByteArray());
    }
    if (!sql.commit())
        return false;
    refreshStatsLocked(sql);
    return true;
}
} // namespace aster::cache
