#pragma once

#include "imagedecoder.h"

#include <QSize>

#include <atomic>

namespace aster::cache {
class BoundedImageDecoder {
public:
    explicit BoundedImageDecoder(DecodeLimits = {});
    ImageResult decode(const QByteArray&, const std::atomic<bool>& cancelled, QSize expectedSize = {}) const;

private:
    DecodeLimits limits_;
};
} // namespace aster::cache
