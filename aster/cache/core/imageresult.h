#pragma once

#include "imagehandle.h"

#include <QImage>
#include <QString>

#include <optional>
#include <utility>

namespace aster::cache
{
enum class ImageError
{
    None,
    CacheMiss,
    CorruptedEntry,
    Cancelled,
    SourceChanged,
    IoError,
    InvalidRequest,
    ProcessingError,
    ResourceLimit,
    UnsupportedFormat
};

enum class CacheResultSource
{
    Unknown,
    RenderedMemory,
    Loaded,
    EncodedMemory,
    RawDisk,
    Network,
    Validated,
    LocalFile,
    Data,
    RenderedDisk,
    ActiveResource,
    Resource
};

template <class T> struct Result
{
    std::optional<T> value;
    ImageError error = ImageError::None;
    QString message;
    CacheResultSource source = CacheResultSource::Unknown;
    ImageHandle handle;

    explicit operator bool() const
    {
        return value.has_value() && error == ImageError::None;
    }

    static Result success(T value, CacheResultSource source = CacheResultSource::Unknown)
    {
        return {std::move(value), ImageError::None, {}, source, {}};
    }

    static Result failure(ImageError error, QString message = {})
    {
        return {std::nullopt, error, std::move(message), CacheResultSource::Unknown, {}};
    }
};

using ImageResult = Result<QImage>;
}
