#pragma once

#include "imageresult.h"

#include <QRect>
#include <QSizeF>

namespace aster::cache {
enum class ImageFit { Fill, Contain, Cover, None, ScaleDown };

enum class ImageScaleAlgorithm { QtFast, QtSmooth, Bilinear, Bicubic, Lanczos3, Lanczos4 };

struct RenderGeometry {
    QSize scaledSize;
    QRect region;
};

QByteArray fitName(ImageFit fit);
Result<ImageFit> parseFit(const QByteArray& name);
Result<QSize> physicalTargetSize(QSizeF logicalSize, qreal dpr, int bucket = 1);
Result<RenderGeometry> renderGeometry(QSize source, QSize target, ImageFit fit);
} // namespace aster::cache
