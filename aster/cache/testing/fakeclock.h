#pragma once

#include "aster/cache/core/clock.h"

#include <atomic>

namespace aster::cache::testing
{
class FakeClock final : public Clock
{
public:
    explicit FakeClock(qint64 ms = 0) : ms_(ms)
    {
    }

    QDateTime now() const override
    {
        return QDateTime::fromMSecsSinceEpoch(ms_.load()).toUTC();
    }

    void advance(qint64 ms)
    {
        ms_.fetch_add(ms);
    }

private:
    std::atomic<qint64> ms_;
};
}
