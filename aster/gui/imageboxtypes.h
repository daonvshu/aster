#pragma once

#include <QObject>

namespace aster::gui {
Q_NAMESPACE

enum class ImageTransition { None, Fade, CrossFade, Slide, Zoom, FadeZoom };
Q_ENUM_NS(ImageTransition)

enum class TransitionPolicy { Always, NonMemoryCache, FirstLoadOrNonMemoryCache };
Q_ENUM_NS(TransitionPolicy)

enum class OffscreenPolicy { Keep, ReleaseHandle, ReleaseImage };
Q_ENUM_NS(OffscreenPolicy)

enum class ImageBoxState { Empty, Loading, Ready, Error };
Q_ENUM_NS(ImageBoxState)
} // namespace aster::gui

Q_DECLARE_METATYPE(aster::gui::ImageBoxState)
