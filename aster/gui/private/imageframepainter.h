#pragma once

#include "aster/cache/core/imagerendergeometry.h"

#include <QImage>
#include <QPainter>

namespace aster::gui::detail {
class ImageFramePainter {
public:
    ImageFramePainter(QRect contents, qreal dpr, cache::ImageFit fit);
    void draw(QPainter& painter, const QImage& image, QSize target, cache::ImageFit fit) const;

private:
    QRect contents_;
    qreal dpr_;
    cache::ImageFit fit_;
};
} // namespace aster::gui::detail
