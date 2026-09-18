#include "imagerenderer.h"

#include <cmath>
#include <new>
#include <utility>

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
    if (!result)
        return result;

    const auto expectedSize = result.value->size();
    for (const auto& transformation : options.transformations) {
        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);
        if (!transformation)
            return ImageResult::failure(ImageError::InvalidRequest, "Null image transformation");
        try {
            result = transformation->transform(std::move(*result.value), cancelled);
        } catch (const std::bad_alloc&) {
            return ImageResult::failure(ImageError::ResourceLimit, "Image transformation allocation failed");
        } catch (...) {
            return ImageResult::failure(ImageError::ProcessingError, "Image transformation threw");
        }
        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);
        if (!result && result.error == ImageError::None)
            return ImageResult::failure(ImageError::ProcessingError, "Image transformation returned no image or error");
        if (!result)
            return result;
        if (result.value->isNull() || result.value->size() != expectedSize)
            return ImageResult::failure(ImageError::ProcessingError, "Image transformation changed the output dimensions");
    }
    result.value->setDevicePixelRatio(options.dpr);
    return result;
}
} // namespace aster::cache
