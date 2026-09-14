#pragma once

#include "imagecachepolicy.h"
#include "imagekey.h"

namespace aster::cache {
struct ImageRequest {
    SourceKey source;
    RenderOptions render;
    ImageCachePolicy cache;
};
} // namespace aster::cache
