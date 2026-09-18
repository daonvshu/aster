#include "imagedecoder.h"

#include <stdexcept>
#include <utility>

namespace aster::cache {
ImageDecoder::ImageDecoder(ProcessorIdentity identity, Matcher matcher, Handler handler)
    : identity_(std::move(identity))
    , matcher_(std::move(matcher))
    , handler_(std::move(handler)) {
    if (identity_.identifier.isEmpty() || identity_.version == 0)
        throw std::invalid_argument("Invalid decoder identity");
    if (!matcher_ || !handler_)
        throw std::invalid_argument("Image decoder callables must not be empty");
}

QSharedPointer<IImageDecoder> ImageDecoder::create(ProcessorIdentity identity, Matcher matcher, Handler handler) {
    return QSharedPointer<ImageDecoder>(new ImageDecoder(std::move(identity), std::move(matcher), std::move(handler)));
}

const ProcessorIdentity& ImageDecoder::identity() const noexcept {
    return identity_;
}

bool ImageDecoder::supports(const QByteArray& bytes, const QByteArray& contentType) const {
    return matcher_(bytes, contentType);
}

ImageResult ImageDecoder::decode(const QByteArray& bytes, const DecodeContext& context, const std::atomic<bool>& cancelled) const {
    return handler_(bytes, context, cancelled);
}
} // namespace aster::cache
