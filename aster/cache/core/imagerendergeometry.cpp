#include "imagerendergeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aster::cache {
QByteArray fitName(ImageFit fit) {
    switch (fit) {
        case ImageFit::Fill:
            return "fill";
        case ImageFit::Contain:
            return "contain";
        case ImageFit::Cover:
            return "cover";
        case ImageFit::None:
            return "none";
        case ImageFit::ScaleDown:
            return "scale-down";
    }
    return {};
}

Result<ImageFit> parseFit(const QByteArray& name) {
    for (const auto fit : {ImageFit::Fill, ImageFit::Contain, ImageFit::Cover, ImageFit::None, ImageFit::ScaleDown})
        if (fitName(fit) == name)
            return Result<ImageFit>::success(fit);
    return Result<ImageFit>::failure(ImageError::InvalidRequest, "Unknown fit mode");
}

Result<QSize> physicalTargetSize(QSizeF logicalSize, qreal dpr, int bucket) {
    if (!std::isfinite(dpr) || dpr <= 0 || bucket < 1 || bucket > 4096)
        return Result<QSize>::failure(ImageError::InvalidRequest);
    auto dimension = [dpr, bucket](qreal logical) {
        const double value = std::ceil(std::ceil(logical * dpr) / bucket) * bucket;
        if (!std::isfinite(value) || logical <= 0 || value < 1 || value > std::numeric_limits<int>::max())
            return 0;
        return int(value);
    };
    const QSize size(dimension(logicalSize.width()), dimension(logicalSize.height()));
    if (size.isEmpty())
        return Result<QSize>::failure(ImageError::InvalidRequest, "Empty or oversized target");
    return Result<QSize>::success(size);
}

Result<RenderGeometry> renderGeometry(QSize source, QSize target, ImageFit fit) {
    using R = Result<RenderGeometry>;
    if (source.isEmpty() || target.isEmpty() || fitName(fit).isEmpty())
        return R::failure(ImageError::InvalidRequest);
    QSize scaled = source;
    if (fit == ImageFit::Fill)
        scaled = target;
    else if (fit != ImageFit::None) {
        const double sx = double(target.width()) / source.width();
        const double sy = double(target.height()) / source.height();
        double factor = fit == ImageFit::Cover ? std::max(sx, sy) : std::min(sx, sy);
        if (fit == ImageFit::ScaleDown)
            factor = std::min(1.0, factor);
        auto dimension = [fit, factor](int value) {
            const double rounded = fit == ImageFit::Cover ? std::ceil(value * factor) : std::floor(value * factor + 1e-9);
            return rounded > std::numeric_limits<int>::max() ? 0 : std::max(1, int(rounded));
        };
        scaled = QSize(dimension(source.width()), dimension(source.height()));
    }
    if (scaled.isEmpty())
        return R::failure(ImageError::ResourceLimit);
    const QSize output = fit == ImageFit::Cover ? target : scaled;
    const QPoint offset((scaled.width() - output.width()) / 2, (scaled.height() - output.height()) / 2);
    return R::success({scaled, QRect(offset, output)});
}
} // namespace aster::cache
