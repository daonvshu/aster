#pragma once

namespace aster::cache
{
enum class DiskCacheStrategy
{
    None,
    SourceOnly,
    RenderedOnly,
    All,
    Automatic
};
}
