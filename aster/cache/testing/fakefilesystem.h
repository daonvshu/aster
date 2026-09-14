#pragma once

#include "aster/cache/core/imagekey.h"

#include <QCryptographicHash>
#include <QHash>

#include <mutex>

namespace aster::cache::testing {
class FakeFileSystem {
public:
    void write(const QString& path, QByteArray bytes, qint64 modifiedMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        files_.insert(path, {std::move(bytes), modifiedMs});
    }

    Result<LocalFingerprint> fingerprint(const QString& path, bool strict = false) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = files_.constFind(path);
        if (it == files_.cend())
            return Result<LocalFingerprint>::failure(ImageError::IoError);
        return Result<LocalFingerprint>::success(
                {path, it->bytes.size(), it->ms, strict ? QCryptographicHash::hash(it->bytes, QCryptographicHash::Sha256) : QByteArray{}});
    }

private:
    struct File {
        QByteArray bytes;
        qint64 ms;
    };

    mutable std::mutex mutex_;
    QHash<QString, File> files_;
};
} // namespace aster::cache::testing
