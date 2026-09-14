#pragma once

#include "aster/cache/core/imagekey.h"
#include "aster/cache/decoder/boundedimagedecoder.h"
#include "imageresampler.h"

namespace aster::cache {
class ImageRenderer {
public:
    explicit ImageRenderer(DecodeLimits decodeLimits = {}, ResampleLimits resampleLimits = {});
    ImageResult operator()(const QByteArray& bytes, const RenderOptions& options, const std::atomic<bool>& cancelled) const;

private:
    BoundedImageDecoder decoder_;
    ImageResampler resampler_;
};
} // namespace aster::cache
