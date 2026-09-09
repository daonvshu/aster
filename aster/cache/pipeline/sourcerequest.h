#pragma once

#include "aster/cache/core/diskcachestrategy.h"
#include "aster/cache/source/iimagesourceloader.h"

namespace aster::cache
{
struct SourceRequest
{
    ImageSource source;
    RenderOptions render;
    SourceLoadOptions load;
    DiskCacheStrategy diskStrategy = DiskCacheStrategy::Automatic;
    bool expensiveProcessing = false;
};
}
