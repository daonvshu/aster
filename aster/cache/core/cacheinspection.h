#pragma once

#include "imagekey.h"

#include <QVector>

#include <algorithm>
#include <chrono>

namespace aster::cache
{
struct LookupLatency
{
    quint64 samples = 0;
    quint64 totalNs = 0;
    quint64 maxNs = 0;
};

class LookupTimer
{
public:
    using Clock = std::chrono::steady_clock;

    explicit LookupTimer(LookupLatency& stats, Clock::time_point started = Clock::now())
        : stats_(stats), started_(started)
    {
    }

    ~LookupTimer()
    {
        const auto ns = quint64(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_).count());
        ++stats_.samples;
        stats_.totalNs += ns;
        stats_.maxNs = std::max(stats_.maxNs, ns);
    }

    LookupTimer(const LookupTimer&) = delete;
    LookupTimer& operator=(const LookupTimer&) = delete;

private:
    LookupLatency& stats_;
    Clock::time_point started_;
};

struct CacheSelector
{
    enum class Kind
    {
        All,
        Source,
        Rendered
    };

    QByteArray sourceDigest;
    QByteArray namespaceDigest;
    Kind kind = Kind::All;

    bool valid() const
    {
        return (sourceDigest.size() == 32 && namespaceDigest.isEmpty()) ||
               (sourceDigest.isEmpty() && namespaceDigest.size() == 32);
    }

    bool matches(const SourceKey& key) const
    {
        return valid() && (sourceDigest.isEmpty() ? key.namespaceDigest == namespaceDigest
                                                  : key.digest == sourceDigest);
    }
};

struct CacheDebugEntry
{
    QByteArray keyDigest;
    QByteArray sourceDigest;
    QByteArray namespaceDigest;
    qint64 bytes = 0;
    qint64 ageMs = 0;
};
}
