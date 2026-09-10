#pragma once

#include "aster/cache/core/imagerendergeometry.h"

#include <atomic>

namespace aster::cache
{
enum class ResampleExecution
{
    Automatic,
    Scalar
};

enum class KernelEvaluation
{
    Default,
    Exact,
    Lut
};

struct ResampleLimits
{
    int maxSide = 16384;
    qint64 maxPixels = 16 * 1024 * 1024;
    qint64 maxWorkingBytes = 256 * 1024 * 1024;
};

struct ResampleMetrics
{
    qint64 workingBytes = 0;
    qint64 weightGenerationNs = 0;
};

class ImageResampler
{
public:
    explicit ImageResampler(ResampleLimits limits = {},
                            ResampleExecution execution = ResampleExecution::Automatic,
                            KernelEvaluation kernel = KernelEvaluation::Default);
    static bool simdAvailable();
    ImageResult resize(const QImage& source, const RenderGeometry& geometry,
                       ImageScaleAlgorithm algorithm, const std::atomic<bool>& cancelled,
                       ResampleMetrics* metrics = nullptr) const;

private:
    ResampleLimits limits_;
    ResampleExecution execution_;
    KernelEvaluation kernel_;
};
}
