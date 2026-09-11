#pragma once

#include "aster/cache/core/imageresult.h"

#include <QList>
#include <QSize>

#include <atomic>

namespace aster::cache
{
struct DecodeLimits
{
    qint64 maxEncodedBytes = 32 * 1024 * 1024;
    int maxSide = 16384;
    qint64 maxPixels = 0;       // Zero disables the pixel-count limit.
    qint64 maxDecodedBytes = 0; // Zero disables the decoded-memory limit.
    QList<QByteArray> allowedFormats = {"png", "jpeg", "bmp", "webp"};
};

class BoundedImageDecoder
{
public:
    explicit BoundedImageDecoder(DecodeLimits = {});
    ImageResult decode(const QByteArray&, const std::atomic<bool>& cancelled,
                       QSize expectedSize = {}) const;

private:
    DecodeLimits limits_;
};
}
