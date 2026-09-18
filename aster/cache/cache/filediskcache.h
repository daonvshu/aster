#pragma once

#include "aster/cache/core/clock.h"
#include "idiskcache.h"

#include <QHash>
#include <QSharedPointer>

#include <functional>
#include <mutex>

namespace aster::cache {
class FileDiskCache final : public IDiskCache {
public:
    enum class WriteStage { TenPercent, Half, BeforeCommit };
    using WriteCheckpoint = std::function<bool(WriteStage)>;

    FileDiskCache(QString root, qint64 budget, qint64 maxEntry, QSharedPointer<Clock> clock = QSharedPointer<SystemClock>::create(),
                  WriteCheckpoint checkpoint = {}, QString directoryVersion = "v1", bool appendVersionDirectory = true);
    Result<DiskEntry> get(const QByteArray&) override;
    bool put(const QByteArray&, const DiskEntry&) override;
    bool remove(const QByteArray&) override;
    bool contains(const QByteArray&) override;
    bool clear(const std::atomic<bool>* cancelled = nullptr) override;
    bool trim(qint64 bytes, const std::atomic<bool>* cancelled = nullptr) override;
    DiskStats stats() const override;
    bool invalidate(const CacheSelector&, const std::atomic<bool>* cancelled = nullptr) override;
    void setMaxCost(qint64);
    bool recover(const std::atomic<bool>* cancelled = nullptr);
    bool flushAccessTimes(const std::atomic<bool>* cancelled = nullptr);

private:
    struct IndexEntry {
        qint64 cost;
        qint64 accessed;
        bool dirty = false;
    };

    QString path(const QByteArray&) const;
    Result<DiskEntry> readLocked(const QByteArray&, qint64 now, bool track);
    bool scan(const std::atomic<bool>*);
    bool erase(const QByteArray&);
    bool trimLocked(qint64, const std::atomic<bool>*);
    void remember(const QByteArray&, qint64 cost, qint64 accessed);
    QString root_;
    qint64 maxEntry_;
    QSharedPointer<Clock> clock_;
    WriteCheckpoint checkpoint_;
    mutable std::mutex mutex_;
    QHash<QByteArray, IndexEntry> index_;
    DiskStats stats_;
};
} // namespace aster::cache
