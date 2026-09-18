#include "aster/cache/core/imagetransformation.h"

#include <QDataStream>
#include <QIODevice>

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include <QColorSpace>
#endif

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace aster::cache {
namespace {
class CallableTransformation final : public ImageTransformation {
public:
    CallableTransformation(ProcessorIdentity identity, Handler handler)
        : ImageTransformation(std::move(identity))
        , handler_(std::move(handler)) {
    }

    ImageResult transform(QImage image, const std::atomic<bool>& cancelled) const override {
        return handler_(std::move(image), cancelled);
    }

private:
    Handler handler_;
};

QByteArray parameters(const std::function<void(QDataStream&)>& write) {
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    write(stream);
    return result;
}

ImageResult converted(QImage image, const std::atomic<bool>& cancelled) {
    if (cancelled.load())
        return ImageResult::failure(ImageError::Cancelled);
    if (image.isNull())
        return ImageResult::failure(ImageError::ProcessingError, "Transformation received an empty image");

    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull())
        return ImageResult::failure(ImageError::ResourceLimit, "Transformation image conversion failed");
    return ImageResult::success(std::move(image));
}

void copyColorSpace(const QImage& source, QImage& destination) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    destination.setColorSpace(source.colorSpace());
#else
    (void)source;
    (void)destination;
#endif
}
} // namespace

ImageTransformation::ImageTransformation(ProcessorIdentity identity)
    : identity_(std::move(identity)) {
    if (identity_.identifier.isEmpty() || identity_.version == 0)
        throw std::invalid_argument("Invalid transformation identity");
}

const ProcessorIdentity& ImageTransformation::identity() const noexcept {
    return identity_;
}

QSharedPointer<ImageTransformation> ImageTransformation::create(ProcessorIdentity identity, Handler handler) {
    if (!handler)
        throw std::invalid_argument("Image transformation handler must not be empty");
    return QSharedPointer<CallableTransformation>::create(std::move(identity), std::move(handler));
}

QSharedPointer<ImageTransformation> ImageTransformation::grayscale() {
    return create({"aster/grayscale", 1, {}}, [](QImage image, const std::atomic<bool>& cancelled) {
        auto prepared = converted(std::move(image), cancelled);
        if (!prepared)
            return prepared;
        auto& output = *prepared.value;
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            auto* row = output.scanLine(y);
            for (int x = 0; x < output.width(); ++x) {
                auto* pixel = row + x * 4;
                const int gray = (54 * pixel[0] + 183 * pixel[1] + 19 * pixel[2] + 128) >> 8;
                pixel[0] = pixel[1] = pixel[2] = uchar(gray);
            }
        }
        return prepared;
    });
}

QSharedPointer<ImageTransformation> ImageTransformation::invert() {
    return create({"aster/invert", 1, {}}, [](QImage image, const std::atomic<bool>& cancelled) {
        auto prepared = converted(std::move(image), cancelled);
        if (!prepared)
            return prepared;
        auto& output = *prepared.value;
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            auto* row = output.scanLine(y);
            for (int x = 0; x < output.width(); ++x) {
                auto* pixel = row + x * 4;
                pixel[0] = uchar(pixel[3] - pixel[0]);
                pixel[1] = uchar(pixel[3] - pixel[1]);
                pixel[2] = uchar(pixel[3] - pixel[2]);
            }
        }
        return prepared;
    });
}

QSharedPointer<ImageTransformation> ImageTransformation::opacity(qreal opacity) {
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
        throw std::invalid_argument("Invalid image opacity");
    const auto identityParameters = parameters([opacity](QDataStream& stream) { stream << double(opacity); });
    return create({"aster/opacity", 1, identityParameters}, [opacity](QImage image, const std::atomic<bool>& cancelled) {
        auto prepared = converted(std::move(image), cancelled);
        if (!prepared)
            return prepared;
        auto& output = *prepared.value;
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            auto* row = output.scanLine(y);
            for (int x = 0; x < output.width() * 4; ++x)
                row[x] = uchar(std::lround(row[x] * opacity));
        }
        return prepared;
    });
}

QSharedPointer<ImageTransformation> ImageTransformation::tint(const QColor& color, qreal amount) {
    if (!color.isValid() || !std::isfinite(amount) || amount < 0 || amount > 1)
        throw std::invalid_argument("Invalid image tint");
    const auto identityParameters = parameters([color, amount](QDataStream& stream) { stream << quint32(color.rgba()) << double(amount); });
    return create({"aster/tint", 1, identityParameters}, [color, amount](QImage image, const std::atomic<bool>& cancelled) {
        auto prepared = converted(std::move(image), cancelled);
        if (!prepared)
            return prepared;
        auto& output = *prepared.value;
        const double blend = amount * color.alphaF();
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            auto* row = output.scanLine(y);
            for (int x = 0; x < output.width(); ++x) {
                auto* pixel = row + x * 4;
                const int alpha = pixel[3];
                const int tint[3] = {color.red() * alpha / 255, color.green() * alpha / 255, color.blue() * alpha / 255};
                for (int channel = 0; channel < 3; ++channel)
                    pixel[channel] = uchar(std::clamp(std::lround(pixel[channel] * (1 - blend) + tint[channel] * blend), 0L, 255L));
            }
        }
        return prepared;
    });
}

QSharedPointer<ImageTransformation> ImageTransformation::blur(int radius) {
    if (radius < 1 || radius > 256)
        throw std::invalid_argument("Invalid blur radius");
    const auto identityParameters = parameters([radius](QDataStream& stream) { stream << qint32(radius); });
    return create({"aster/box-blur", 1, identityParameters}, [radius](QImage image, const std::atomic<bool>& cancelled) {
        auto prepared = converted(std::move(image), cancelled);
        if (!prepared)
            return prepared;
        const auto& input = *prepared.value;
        QImage horizontal(input.size(), QImage::Format_RGBA8888_Premultiplied);
        QImage output(input.size(), QImage::Format_RGBA8888_Premultiplied);
        if (horizontal.isNull() || output.isNull())
            return ImageResult::failure(ImageError::ResourceLimit, "Blur allocation failed");

        for (int y = 0; y < input.height(); ++y) {
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            qint64 sums[4] = {};
            const auto* source = input.constScanLine(y);
            auto* destination = horizontal.scanLine(y);
            for (int x = 0; x <= std::min(radius, input.width() - 1); ++x)
                for (int channel = 0; channel < 4; ++channel)
                    sums[channel] += source[x * 4 + channel];
            for (int x = 0; x < input.width(); ++x) {
                const int first = std::max(0, x - radius);
                const int last = std::min(input.width() - 1, x + radius);
                const int count = last - first + 1;
                for (int channel = 0; channel < 4; ++channel)
                    destination[x * 4 + channel] = uchar((sums[channel] + count / 2) / count);
                if (x - radius >= 0)
                    for (int channel = 0; channel < 4; ++channel)
                        sums[channel] -= source[(x - radius) * 4 + channel];
                if (x + radius + 1 < input.width())
                    for (int channel = 0; channel < 4; ++channel)
                        sums[channel] += source[(x + radius + 1) * 4 + channel];
            }
        }

        for (int x = 0; x < horizontal.width(); ++x) {
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            qint64 sums[4] = {};
            for (int y = 0; y <= std::min(radius, horizontal.height() - 1); ++y) {
                const auto* pixel = horizontal.constScanLine(y) + x * 4;
                for (int channel = 0; channel < 4; ++channel)
                    sums[channel] += pixel[channel];
            }
            for (int y = 0; y < horizontal.height(); ++y) {
                const int first = std::max(0, y - radius);
                const int last = std::min(horizontal.height() - 1, y + radius);
                const int count = last - first + 1;
                auto* pixel = output.scanLine(y) + x * 4;
                for (int channel = 0; channel < 4; ++channel)
                    pixel[channel] = uchar((sums[channel] + count / 2) / count);
                if (y - radius >= 0) {
                    const auto* removed = horizontal.constScanLine(y - radius) + x * 4;
                    for (int channel = 0; channel < 4; ++channel)
                        sums[channel] -= removed[channel];
                }
                if (y + radius + 1 < horizontal.height()) {
                    const auto* added = horizontal.constScanLine(y + radius + 1) + x * 4;
                    for (int channel = 0; channel < 4; ++channel)
                        sums[channel] += added[channel];
                }
            }
        }
        copyColorSpace(input, output);
        return ImageResult::success(std::move(output));
    });
}
} // namespace aster::cache
