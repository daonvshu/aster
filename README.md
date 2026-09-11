# aster

[中文说明](README-zh.md)

aster is an asynchronous image loading and caching framework built with C++17, Qt 5/6, and CMake. Its core module does not depend on Qt Widgets.

> **Experimental project:** aster is under active development. Correctness, API stability, performance, and compatibility are not guaranteed. Use it with caution and validate it thoroughly before relying on it in production.

## Features

- Local files, Qt resources, memory data, and HTTP image sources.
- Thread-safe memory LRU, optional disk caches, TTL, ETag/304 validation, and statistics.
- Concurrent request coalescing with independently cancellable subscriptions.
- QtFast, QtSmooth, Bilinear, Bicubic, Lanczos3, and Lanczos4 scaling.
- ImageBox with states, placeholders, rounded corners, transitions, and offscreen policies.

## Architecture

```text
      ImageBox / custom QWidget
                  |
             aster::gui
                  |
            aster::cache
         +--------+--------+
 SourceLoader  Pipeline   Renderer
      |           |              |
 local/network  merge/cancel    decode/scale
      |           |                  |
 Encoded Cache  Rendered Cache  Disk Cache
```

`aster::cache` provides source identification, loading, caching, decoding, resampling, and `ImagePipeline`. `aster::gui` depends on cache and provides QWidget lifecycle management and painting. The CMake targets are `aster::cache` and `aster::gui`.

## Build

```sh
cmake -S . -B build -DASTER_QT_MAJOR=6 -DCMAKE_PREFIX_PATH=/path/to/Qt/6
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Set `ASTER_QT_MAJOR=5` for Qt 5. Use `ASTER_BUILD_GUI=OFF` for a cache-only build and `ASTER_BUILD_GALLERY=OFF` to disable the Gallery example.

## Cache Example

The cache module can be used without `aster::gui`:

```cpp
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/service/imageservice.h"

aster::cache::ImageServiceConfig config;
config.renderer = aster::cache::ImageRenderer{};
config.renderedMemoryBytes = 64 * 1024 * 1024;
config.encodedMemoryBytes = 32 * 1024 * 1024;
config.workerCount = 4; // Maximum concurrent source/render workers
aster::cache::ImageService::configure(config);

auto pipeline = aster::cache::ImageService::pipeline();
aster::cache::RenderOptions options;
options.physicalTargetSize = QSize(640, 480);
options.dpr = 1.0;
options.fitMode = "contain";
options.scaleAlgorithm = aster::cache::ImageScaleAlgorithm::Lanczos3;

auto* subscription = pipeline->request("C:/images/photo.jpg", options, owner);
QObject::connect(subscription, &aster::cache::ImageSubscription::finished,
                 owner, [](aster::cache::ImageResult result) {
    if (result)
        useImage(*result.value);
    else
        qWarning() << result.message;
});
```

`ImageSubscription` is owned by its Qt parent. Calling `cancel()` or destroying the parent cancels the subscription. `physicalTargetSize` is expressed in physical pixels.

## ImageBox Example

```cpp
#include "aster/cache/service/imageservice.h"
#include "aster/gui/imagebox.h"

auto* box = new aster::gui::ImageBox(parent);
box->setPipeline(aster::cache::ImageService::pipeline());
box->setFit(aster::gui::ImageFit::Contain);
box->setScaleAlgorithm(aster::gui::ImageScaleAlgorithm::Lanczos3);
box->setCornerRadius(12);
box->setPlaceholder(placeholder);
box->setSource("C:/images/photo.jpg");

QObject::connect(box, &aster::gui::ImageBox::loadFailed, parent,
                 [](aster::cache::ImageError error, const QString& message) {
    qWarning() << int(error) << message;
});
```

ImageBox handles request generations, cancellation, late results, state changes, and painting. String sources support local paths, `file:`, `qrc:`, and HTTP(S) URLs.

## Examples

- `aster_gallery` demonstrates folder browsing, HTTP endpoints, caching, and ImageBox.
- `aster_cache_widget` compares all six scaling algorithms using only `aster::cache`.

## Performance Reference

`aster_benchmark` measures cache get, put, eviction, and concurrent access operations and reports p50/p95 latency. `tests/resampler_tests.cpp` and `aster_cache_widget` help compare scaling quality and cost. Results depend on the CPU, compiler, Qt version, image dimensions, DPR, cache hit rate, and storage. Run benchmarks with a Release build on the target machine.

## AI Assistance

AI tools assisted with architecture exploration, API drafting, implementation iterations, test authoring, formatting, and Qt 5/6 build validation.

## License

aster is distributed under the [MIT License](LICENSE).
