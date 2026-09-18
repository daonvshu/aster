#pragma once

#include "aster/cache/core/clock.h"
#include "idiskcache.h"

#ifndef ASTER_ENABLE_SQLITE_DISK_CACHE
#define ASTER_ENABLE_SQLITE_DISK_CACHE 0
#endif

#if ASTER_ENABLE_SQLITE_DISK_CACHE
#include <QSharedPointer>
#include <QSqlDatabase>

#include <mutex>

namespace aster::cache {
class SqliteDiskCache final : public IDiskCache {
public:
    SqliteDiskCache(QString databasePath, qint64 budget, qint64 maxEntry, QSharedPointer<Clock> clock = QSharedPointer<SystemClock>::create());

    Result<DiskEntry> get(const QByteArray&) override;
    bool put(const QByteArray&, const DiskEntry&) override;
    bool remove(const QByteArray&) override;
    bool contains(const QByteArray&) override;
    bool clear(const std::atomic<bool>* cancelled = nullptr) override;
    bool trim(qint64 bytes, const std::atomic<bool>* cancelled = nullptr) override;
    DiskStats stats() const override;
    bool invalidate(const CacheSelector&, const std::atomic<bool>* cancelled = nullptr) override;

    void setMaxCost(qint64 bytes);
    bool recover();

private:
    bool trimLocked(QSqlDatabase&, qint64 target, const std::atomic<bool>*, quint64* evictions);
    bool removeLocked(QSqlDatabase&, const QByteArray& key, bool countError = true);
    bool refreshStatsLocked(QSqlDatabase&) const;

    QString databasePath_;
    qint64 maxEntry_;
    QSharedPointer<Clock> clock_;
    mutable std::mutex mutex_;
    mutable DiskStats stats_;
    qint64 maxBytes_ = 0;
};
#endif
} // namespace aster::cache
