#include "imageframepainter.h"

#include <algorithm>

namespace aster::gui::detail
{
ImageFramePainter::ImageFramePainter(QRect contents, qreal dpr, cache::ImageFit fit)
    : contents_(contents), dpr_(dpr), fit_(fit)
{
}

void ImageFramePainter::draw(QPainter& painter, const QImage& image, QSize target,
                             cache::ImageFit fit) const
{
    if (image.isNull())
        return;
    QSizeF size = QSizeF(image.size()) / image.devicePixelRatio();
    const auto exactTarget = cache::physicalTargetSize(contents_.size(), dpr_);
    const bool preview = !exactTarget || target != *exactTarget.value || fit != fit_ ||
                         image.devicePixelRatio() != dpr_;
    const auto displayFit = target.isEmpty() ? fit : fit_;
    if (preview && displayFit != cache::ImageFit::None)
    {
        const auto area = QSizeF(contents_.size());
        if (displayFit == cache::ImageFit::Fill)
            size = area;
        else
        {
            const qreal sx = area.width() / size.width();
            const qreal sy = area.height() / size.height();
            qreal scale =
                displayFit == cache::ImageFit::Cover ? std::max(sx, sy) : std::min(sx, sy);
            if (displayFit == cache::ImageFit::ScaleDown)
                scale = std::min(qreal(1), scale);
            size *= scale;
        }
    }
    const QRectF destination(QPointF(contents_.x() + (contents_.width() - size.width()) / 2,
                                     contents_.y() + (contents_.height() - size.height()) / 2),
                             size);
    painter.drawImage(destination, image);
}
}
