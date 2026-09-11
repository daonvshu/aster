#include "boundedimagedecoder.h"

#include <QBuffer>
#include <QImageReader>

#include <stdexcept>

namespace aster::cache
{
BoundedImageDecoder::BoundedImageDecoder(DecodeLimits limits) : limits_(std::move(limits))
{
    if (limits_.maxEncodedBytes <= 0 || limits_.maxSide <= 0 || limits_.maxPixels < 0 ||
        limits_.maxDecodedBytes < 0 || limits_.allowedFormats.isEmpty())
        throw std::invalid_argument("Invalid decode limits");

    for (auto& format : limits_.allowedFormats)
        format = format.toLower();
}

ImageResult BoundedImageDecoder::decode(const QByteArray& bytes, const std::atomic<bool>& cancelled,
                                        QSize expectedSize) const
{
    if (cancelled.load())
        return ImageResult::failure(ImageError::Cancelled);

    if (bytes.size() > limits_.maxEncodedBytes)
        return ImageResult::failure(ImageError::ResourceLimit, "Encoded byte limit exceeded");

    if (bytes.isEmpty())
        return ImageResult::failure(ImageError::CorruptedEntry, "Empty image");

    try
    {
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setDecideFormatFromContent(true);
        reader.setAutoTransform(false);

        if (!reader.canRead())
            return ImageResult::failure(ImageError::CorruptedEntry, "Invalid image header");

        if (!limits_.allowedFormats.contains(reader.format().toLower()))
            return ImageResult::failure(ImageError::UnsupportedFormat, "Image format is disabled");

        const auto size = reader.size();
        if (size.width() <= 0 || size.height() <= 0)
            return ImageResult::failure(ImageError::CorruptedEntry, "Missing image dimensions");

        // Qt6 can decode up to 128 bits per pixel. Account for that before allocating.
        const qint64 pixels = qint64(size.width()) * size.height();
        if (size.width() > limits_.maxSide || size.height() > limits_.maxSide ||
            (limits_.maxPixels > 0 && pixels > limits_.maxPixels) ||
            (limits_.maxDecodedBytes > 0 && pixels > limits_.maxDecodedBytes / 16))
            return ImageResult::failure(ImageError::ResourceLimit, "Decoded image limit exceeded");

        if (expectedSize.isValid() && size != expectedSize)
            return ImageResult::failure(ImageError::CorruptedEntry, "Image dimensions mismatch");

        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);

        auto image = reader.read();
        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);

        if (image.isNull() || image.size() != size)
            return ImageResult::failure(ImageError::CorruptedEntry, "Image decode failed");

        if (limits_.maxDecodedBytes > 0 && qint64(image.sizeInBytes()) > limits_.maxDecodedBytes)
            return ImageResult::failure(ImageError::ResourceLimit, "Decoded byte limit exceeded");

        return ImageResult::success(std::move(image), CacheResultSource::Loaded);
    }
    catch (const std::bad_alloc&)
    {
        return ImageResult::failure(ImageError::ResourceLimit);
    }
    catch (...)
    {
        return ImageResult::failure(ImageError::CorruptedEntry);
    }
}
}
