#include "aster/cache/cache/sqlitediskcache.h"
#include "aster/cache/testing/fakeclock.h"

#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aster::cache;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray digest(const QByteArray& value) {
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256);
}

DiskEntry entry(const QByteArray& source, const QByteArray& nameSpace, const QByteArray& bytes = "payload") {
    return {bytes,
            {{"http", 1}, {"sourceDigest", QString::fromLatin1(digest(source).toHex())}, {"namespaceDigest", QString::fromLatin1(digest(nameSpace).toHex())}}};
}

void sqliteDiskCacheTests() {
    QTemporaryDir directory;
    require(directory.isValid(), "SQLite test directory unavailable");
    const auto databasePath = directory.filePath("cache.sqlite");
    auto clock = QSharedPointer<testing::FakeClock>::create(1000);
    const auto firstKey = digest("first-key");
    const auto secondKey = digest("second-key");

    {
        SqliteDiskCache cache(databasePath, 1024, 256, clock);
        require(cache.stats().indexComplete, "SQLite cache did not initialize its index");
        require(cache.put(firstKey, entry("source-a", "namespace-a")), "SQLite cache put failed");
        const auto first = cache.get(firstKey);
        require(first && first.source == CacheResultSource::RawDisk, "SQLite cache round trip failed");
        require(cache.put(secondKey, entry("source-b", "namespace-b", QByteArray(200, 'x'))), "SQLite cache second put failed");
        require(cache.stats().evictions == 0, "SQLite cache evicted below its budget unexpectedly");

        std::atomic<bool> cancelled{true};
        require(!cache.trim(0, &cancelled), "SQLite trim ignored cancellation");
        require(bool(cache.get(firstKey)), "Cancelled SQLite trim changed entries");
    }

    {
        SqliteDiskCache reopened(databasePath, 1024, 256, clock);
        require(bool(reopened.get(firstKey)), "SQLite cache did not persist entries across instances");
        const auto sourceSelector = CacheSelector{digest("source-a"), {}, CacheSelector::Kind::Source};
        require(reopened.invalidate(sourceSelector), "SQLite source invalidation failed");
        require(!reopened.get(firstKey), "SQLite source invalidation left a matching entry");
        require(bool(reopened.get(secondKey)), "SQLite source invalidation removed an unrelated entry");
    }

    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "aster_sqlite_corruption_test");
        database.setDatabaseName(databasePath);
        require(database.open(), "Could not open SQLite database for corruption test");
        QSqlQuery query(database);
        query.prepare("UPDATE entries SET bytes = ? WHERE key = ?");
        query.addBindValue("corrupted");
        query.addBindValue(secondKey);
        require(query.exec(), "Could not corrupt SQLite cache entry");
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase("aster_sqlite_corruption_test");
    }

    {
        SqliteDiskCache recovered(databasePath, 1024, 256, clock);
        require(!recovered.get(secondKey), "SQLite cache accepted a checksum-corrupted entry");
        require(recovered.stats().corruptions == 1, "SQLite cache did not count a corrupted entry");
    }

    {
        SqliteDiskCache concurrent(directory.filePath("concurrent.sqlite"), 1024 * 1024, 4096, clock);
        std::vector<std::thread> workers;
        std::atomic<bool> good{true};
        for (int thread = 0; thread < 4; ++thread) {
            workers.emplace_back([&, thread] {
                for (int i = 0; i < 50; ++i) {
                    const auto key = digest(QByteArray::number(thread) + QByteArray::number(i));
                    if (!concurrent.put(key, {QByteArray::number(i), {}}) || !concurrent.get(key))
                        good = false;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        require(good, "Concurrent SQLite operation failed");
        require(concurrent.stats().entryCount == 200, "Concurrent SQLite entry count is incorrect");
        require(concurrent.clear(), "SQLite clear failed");
        require(concurrent.stats().entryCount == 0, "SQLite clear left entries");
    }
}
} // namespace

void sqliteDiskCacheTestsEntry() {
    sqliteDiskCacheTests();
    std::cout << "PASS SQLite disk cache persistence, integrity, invalidation and concurrency\n";
}
