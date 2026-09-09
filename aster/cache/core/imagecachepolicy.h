#pragma once

namespace aster::cache
{
enum class CacheReadPolicy
{
    Default,
    BypassMemory,
    BypassDisk,
    CacheOnly,
    NoCache
};

enum class CacheWritePolicy
{
    Default,
    MemoryOnly,
    DiskOnly,
    NoStore
};

struct ImageCachePolicy
{
    CacheReadPolicy read = CacheReadPolicy::Default;
    CacheWritePolicy write = CacheWritePolicy::Default;
    bool allowStale = false;
};
}
