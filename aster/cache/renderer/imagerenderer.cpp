#include "imagerenderer.h"

#include <cmath>

namespace aster::cache {
ImageRenderer::ImageRenderer(DecodeLimits decodeLimits, ResampleLimits resampleLimits)
    : decoder_(std::move(decodeLimits))
    , resampler_(resampleLimits) {
}

ImageResult ImageRenderer::operator()(const QByteArray& bytes, const RenderOptions& options, const std::atomic<bool>& cancelled) const {
    const auto fit = parseFit(options.fitMode);
    if (!fit || options.resamplerVersion != 1 || !options.processors.isEmpty() || options.physicalTargetSize.isEmpty() || !std::isfinite(options.dpr) ||
        options.dpr <= 0)
        return ImageResult::failure(ImageError::InvalidRequest, "Unsupported render options");
    auto decoded = decoder_.decode(bytes, cancelled);
    if (!decoded)
        return decoded;
    const auto geometry = renderGeometry(decoded.value->size(), options.physicalTargetSize, *fit.value);
    if (!geometry)
        return ImageResult::failure(geometry.error);
    auto result = resampler_.resize(*decoded.value, *geometry.value, options.scaleAlgorithm, cancelled);
    if (result)
        result.value->setDevicePixelRatio(options.dpr);
    return result;
}
} // namespace aster::cache
