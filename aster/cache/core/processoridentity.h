#pragma once

#include <QByteArray>

namespace aster::cache {
struct ProcessorIdentity {
    QByteArray identifier;
    quint32 version = 1;
    QByteArray parameters;

    bool operator==(const ProcessorIdentity& other) const {
        return identifier == other.identifier && version == other.version && parameters == other.parameters;
    }
};
} // namespace aster::cache
