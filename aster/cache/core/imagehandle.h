#pragma once

#include <QImage>

#include <memory>

namespace aster::cache
{
class ImageHandle
{
public:
    ImageHandle() = default;

    explicit operator bool() const
    {
        return bool(image_);
    }

    QImage image() const
    {
        return image_ ? *image_ : QImage();
    }

    void reset()
    {
        image_.reset();
    }

private:
    explicit ImageHandle(std::shared_ptr<const QImage> image) : image_(std::move(image))
    {
    }

    std::shared_ptr<const QImage> image_;
    friend class ActiveResourceStore;
};
}
