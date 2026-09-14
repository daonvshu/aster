#pragma once

#include <QImage>
#include <QSharedPointer>

namespace aster::cache {
class ImageHandle {
public:
    ImageHandle() = default;

    explicit operator bool() const {
        return bool(image_);
    }

    QImage image() const {
        return image_ ? *image_ : QImage();
    }

    void reset() {
        image_.reset();
    }

private:
    explicit ImageHandle(QSharedPointer<const QImage> image)
        : image_(std::move(image)) {
    }

    QSharedPointer<const QImage> image_;
    friend class ActiveResourceStore;
};
} // namespace aster::cache
