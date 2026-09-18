#pragma once

#include "imageresult.h"

#include <QByteArray>
#include <QColor>
#include <QSharedPointer>

#include <atomic>
#include <functional>

namespace aster::cache {
struct ProcessorIdentity {
    QByteArray identifier;
    quint32 version = 1;
    QByteArray parameters;

    bool operator==(const ProcessorIdentity& other) const {
        return identifier == other.identifier && version == other.version && parameters == other.parameters;
    }
};

class ImageTransformation {
public:
    using Handler = std::function<ImageResult(QImage, const std::atomic<bool>&)>;

    virtual ~ImageTransformation() = default;

    /**
     * @brief Returns the stable identity used in rendered cache keys.
     * @return Transformation identifier, version, and serialized parameters.
     */
    const ProcessorIdentity& identity() const noexcept;

    /**
     * @brief Applies this transformation to a resized image.
     * @param image Input image owned by this call.
     * @param cancelled Cooperative cancellation flag.
     * @return Transformed image or an error. A successful image must retain the input size.
     */
    virtual ImageResult transform(QImage image, const std::atomic<bool>& cancelled) const = 0;

    /**
     * @brief Creates a transformation backed by a callable.
     * @param identity Stable cache identity. The identifier must be non-empty and version must be non-zero.
     * @param handler Thread-safe callable used to transform images.
     * @return Transformation instance.
     */
    static QSharedPointer<ImageTransformation> create(ProcessorIdentity identity, Handler handler);

    /**
     * @brief Creates a grayscale transformation.
     * @return Reusable grayscale transformation.
     */
    static QSharedPointer<ImageTransformation> grayscale();

    /**
     * @brief Creates an RGB inversion transformation that preserves alpha.
     * @return Reusable inversion transformation.
     */
    static QSharedPointer<ImageTransformation> invert();

    /**
     * @brief Creates an opacity transformation.
     * @param opacity Opacity multiplier from 0 to 1.
     * @return Reusable opacity transformation.
     */
    static QSharedPointer<ImageTransformation> opacity(qreal opacity);

    /**
     * @brief Creates a tint transformation that preserves image alpha.
     * @param color Tint color. Its alpha contributes to the blend strength.
     * @param amount Blend amount from 0 to 1.
     * @return Reusable tint transformation.
     */
    static QSharedPointer<ImageTransformation> tint(const QColor& color, qreal amount);

    /**
     * @brief Creates a two-pass box blur transformation.
     * @param radius Blur radius from 1 to 256 pixels.
     * @return Reusable blur transformation.
     */
    static QSharedPointer<ImageTransformation> blur(int radius);

protected:
    /**
     * @brief Constructs a transformation with a validated cache identity.
     * @param identity Stable cache identity.
     */
    explicit ImageTransformation(ProcessorIdentity identity);

private:
    ProcessorIdentity identity_;
};
} // namespace aster::cache
