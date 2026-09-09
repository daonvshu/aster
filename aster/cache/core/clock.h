#pragma once

#include <QDateTime>

#include <memory>

namespace aster::cache
{
class Clock
{
public:
    virtual ~Clock() = default;
    virtual QDateTime now() const = 0;
};

class SystemClock final : public Clock
{
public:
    QDateTime now() const override
    {
        return QDateTime::currentDateTimeUtc();
    }
};
}
