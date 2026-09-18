#pragma once

#include "aster/cache/core/imageresult.h"
#include "aster/cache/core/processoridentity.h"

#include <QByteArray>
#include <QImageReader>
#include <QList>
#include <QSharedPointer>

#include <atomic>
#include <functional>

namespace aster::cache {
struct DecodeLimits {
    qint64 maxEncodedBytes = 32 * 1024 * 1024;
    int maxSide = 16384;
    qint64 maxPixels = 0; // Zero disables the pixel-count limit.
    qint64 maxDecodedBytes = 0; // Zero disables the decoded-memory limit.
    QList<QByteArray> allowedFormats = QImageReader::supportedImageFormats();
};

struct DecodeContext {
    QByteArray contentType;
    DecodeLimits limits;
};

class IImageDecoder {
public:
    virtual ~IImageDecoder() = default;

    /**
     * @brief Returns the stable identity used in rendered cache keys.
     * @return Decoder identifier, version, and serialized parameters.
     */
    virtual const ProcessorIdentity& identity() const noexcept = 0;

    /**
     * @brief Checks whether this decoder owns the input format.
     * @param bytes Encoded image bytes. Implementations should inspect a short signature when possible.
     * @param contentType Normalized Content-Type hint, which may be empty or incorrect.
     * @return True when this decoder must handle the input.
     */
    virtual bool supports(const QByteArray& bytes, const QByteArray& contentType) const = 0;

    /**
     * @brief Decodes an input previously accepted by supports().
     * @param bytes Encoded image bytes.
     * @param context Content-Type hint and mandatory resource limits.
     * @param cancelled Cooperative cancellation flag.
     * @return Decoded image or a terminal error. Failures do not fall through to later decoders.
     */
    virtual ImageResult decode(const QByteArray& bytes, const DecodeContext& context, const std::atomic<bool>& cancelled) const = 0;
};

class ImageDecoder final : public IImageDecoder {
public:
    using Matcher = std::function<bool(const QByteArray&, const QByteArray&)>;
    using Handler = std::function<ImageResult(const QByteArray&, const DecodeContext&, const std::atomic<bool>&)>;

    /**
     * @brief Creates a decoder backed by callables.
     * @param identity Stable cache identity. The identifier must be non-empty and version must be non-zero.
     * @param matcher Thread-safe format matcher.
     * @param handler Thread-safe decoder that observes DecodeContext limits.
     * @return Decoder instance.
     */
    static QSharedPointer<IImageDecoder> create(ProcessorIdentity identity, Matcher matcher, Handler handler);

    /**
     * @brief Returns this decoder's stable cache identity.
     * @return Decoder identity.
     */
    const ProcessorIdentity& identity() const noexcept override;

    /**
     * @brief Delegates format matching to the configured callable.
     * @param bytes Encoded image bytes.
     * @param contentType Normalized Content-Type hint.
     * @return True when this decoder owns the input.
     */
    bool supports(const QByteArray& bytes, const QByteArray& contentType) const override;

    /**
     * @brief Delegates decoding to the configured callable.
     * @param bytes Encoded image bytes.
     * @param context Decode context and resource limits.
     * @param cancelled Cooperative cancellation flag.
     * @return Decoded image or an error.
     */
    ImageResult decode(const QByteArray& bytes, const DecodeContext& context, const std::atomic<bool>& cancelled) const override;

private:
    ImageDecoder(ProcessorIdentity identity, Matcher matcher, Handler handler);

    ProcessorIdentity identity_;
    Matcher matcher_;
    Handler handler_;
};
} // namespace aster::cache
