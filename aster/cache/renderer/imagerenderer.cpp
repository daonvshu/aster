#include "imagerenderer.h"

#include <cmath>
#include <new>
#include <utility>

namespace aster::cache {
ImageRenderer::ImageRenderer(DecodeLimits decodeLimits, ResampleLimits resampleLimits)
    : decodeLimits_(decodeLimits)
    , decoder_(std::move(decodeLimits))
    , resampler_(resampleLimits) {
}

ImageResult ImageRenderer::validateDecoded(ImageResult result, const std::atomic<bool>& cancelled) const {
    if (cancelled.load())
        return ImageResult::failure(ImageError::Cancelled);
    if (!result && result.error == ImageError::None)
        return ImageResult::failure(ImageError::ProcessingError, "Image decoder returned no image or error");
    if (!result)
        return result;
    if (result.value->isNull())
        return ImageResult::failure(ImageError::ProcessingError, "Image decoder returned an empty image");

    const auto size = result.value->size();
    const qint64 pixels = qint64(size.width()) * size.height();
    if (size.width() <= 0 || size.height() <= 0 || size.width() > decodeLimits_.maxSide || size.height() > decodeLimits_.maxSide ||
        (decodeLimits_.maxPixels > 0 && pixels > decodeLimits_.maxPixels) ||
        (decodeLimits_.maxDecodedBytes > 0 && qint64(result.value->sizeInBytes()) > decodeLimits_.maxDecodedBytes))
        return ImageResult::failure(ImageError::ResourceLimit, "Decoded image limit exceeded");
    return result;
}

ImageResult ImageRenderer::decodeImage(const QByteArray& bytes, const RenderOptions& options, const std::atomic<bool>& cancelled) const {
    if (cancelled.load())
        return ImageResult::failure(ImageError::Cancelled);
    if (bytes.isEmpty())
        return ImageResult::failure(ImageError::CorruptedEntry, "Empty image");
    if (bytes.size() > decodeLimits_.maxEncodedBytes)
        return ImageResult::failure(ImageError::ResourceLimit, "Encoded byte limit exceeded");

    const auto contentType = options.contentTypeHint.split(';').value(0).trimmed().toLower();
    const DecodeContext context{contentType, decodeLimits_};
    for (const auto& decoder : options.decoders) {
        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);
        if (!decoder)
            return ImageResult::failure(ImageError::InvalidRequest, "Null image decoder");

        bool supported = false;
        try {
            supported = decoder->supports(bytes, contentType);
        } catch (...) {
            return ImageResult::failure(ImageError::ProcessingError, "Image decoder matcher threw");
        }
        if (!supported)
            continue;
        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);

        ImageResult result;
        try {
            result = decoder->decode(bytes, context, cancelled);
        } catch (const std::bad_alloc&) {
            return ImageResult::failure(ImageError::ResourceLimit, "Image decoder allocation failed");
        } catch (...) {
            return ImageResult::failure(ImageError::ProcessingError, "Image decoder threw");
        }
        return validateDecoded(std::move(result), cancelled);
    }

    return validateDecoded(decoder_.decode(bytes, cancelled), cancelled);
}

ImageResult ImageRenderer::operator()(const QByteArray& bytes, const RenderOptions& options, const std::atomic<bool>& cancelled) const {
    const auto fit = parseFit(options.fitMode);
    if (!fit || options.resamplerVersion != 1 || !options.processors.isEmpty() || options.physicalTargetSize.isEmpty() || !std::isfinite(options.dpr) ||
        options.dpr <= 0)
        return ImageResult::failure(ImageError::InvalidRequest, "Unsupported render options");
    auto decoded = decodeImage(bytes, options, cancelled);
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
