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
 SourceLoader  Pipeline       Renderer
      |           |              |
 local/network  merge/cancel  Decoder chain
      |           |           scale/transform
 Encoded Cache  Rendered Cache  Disk Cache
```

`aster::cache` provides source identification, loading, caching, decoding, resampling, and `ImagePipeline`. `aster::gui` depends on cache and provides QWidget lifecycle management and painting. The CMake targets are `aster::cache`, optional `aster::network`, and optional `aster::gui`.

## Build

```sh
cmake -S . -B build -DASTER_QT_MAJOR=6 -DCMAKE_PREFIX_PATH=/path/to/Qt/6
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Set `ASTER_QT_MAJOR=5` for Qt 5. Use `ASTER_BUILD_GUI=OFF` for a cache-only build and `ASTER_BUILD_GALLERY=OFF` to disable the Gallery example.

### Install And Use

Install the library and its CMake package from a configured build tree:

```sh
cmake --install build --config Release --prefix /path/to/aster-install
```

Downstream applications can import the exported targets with `find_package`:

```cmake
find_package(aster CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE aster::cache)
# Link aster::gui when using ImageBox.
```

The package records the Qt major version and optional components used by the build. `aster::network` is exported when `ASTER_BUILD_QT_NETWORK=ON`; the SQLite dependency is exported when `ASTER_ENABLE_SQLITE_DISK_CACHE=ON`.

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

### Disk Storage

`IDiskCache` is the storage boundary for raw source entries and rendered entries. By default, `ImageService::configure()` and `ImageService::createPipeline()` create separate `FileDiskCache` instances at `QStandardPaths::CacheLocation/source/aster-cache-v1` and `QStandardPaths::CacheLocation/rendered/aster-cache-v1`. Qt resolves the cache location using the organization and application name, while `FileDiskCache` adds its version directory to isolate on-disk formats. Each cache has a 256 MiB budget and a 32 MiB per-entry limit. Use `diskCacheDirectory`, `sourceDiskBytes`, `renderedDiskBytes`, and `maxDiskEntryBytes` to change these defaults, or set `enableDefaultDiskCache = false` to disable the default disk caches.

Aster also includes a transactional SQLite implementation that can explicitly replace the default filesystem caches:

```cpp
#include "aster/cache/cache/sqlitediskcache.h"
#include "aster/cache/cache/rendereddiskcache.h"

#include <QStandardPaths>

auto disk = QSharedPointer<aster::cache::SqliteDiskCache>::create(
    QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/images.sqlite",
    512 * 1024 * 1024,
    32 * 1024 * 1024);

config.sourceDisk = disk;
config.renderedDisk = QSharedPointer<aster::cache::RenderedDiskCache>::create(disk);
```

`SqliteDiskCache` uses an atomic transaction for replacement, stores metadata beside the payload, verifies a SHA-256 checksum on reads, and evicts least-recently-used entries when the byte budget is exceeded. Applications can implement `IDiskCache` for another database, encrypted store, or object backend and inject it in the same two locations. Implementations must be thread-safe, preserve old entries when a write fails, and honor cancellation during maintenance operations.

Call `pipeline->clearDiskCaches()` to clear both source and rendered disk caches owned by that pipeline. `pipeline->cacheStats()` reports their usage and hit statistics separately.

The SQLite backend is controlled by the CMake option `ASTER_ENABLE_SQLITE_DISK_CACHE`, which defaults to `ON`. Set it to `OFF` to omit the QtSql dependency, backend source, and SQLite tests from the build.

### Custom Decoders

Register custom static-image formats on a pipeline with `ImageDecoder::create(...)`. Custom decoders are matched in registration order before Aster falls back to Qt's bounded decoder:

```cpp
#include "aster/cache/decoder/imagedecoder.h"

config.addDecoder(aster::cache::ImageDecoder::create(
    {"sample/custom-format", 1, serializedParameters},
    [](const QByteArray& bytes, const QByteArray& contentType) {
        return bytes.startsWith("CSTM"); // Prefer a file signature over Content-Type.
    },
    [](const QByteArray& bytes,
       const aster::cache::DecodeContext& context,
       const std::atomic<bool>& cancelled) {
        if (cancelled.load())
            return aster::cache::ImageResult::failure(
                aster::cache::ImageError::Cancelled);
        return decodeCustomImage(bytes, context.limits);
    }));
```

Once a decoder reports support, its result is terminal; a corrupt custom image does not fall through to another decoder. Matchers and handlers may run concurrently and must be thread-safe. Handlers must observe cancellation and `DecodeContext::limits` while allocating. Aster validates the returned image again for null images, dimensions, pixel count, and decoded byte size.

Decoder identity, parameters, and order are part of the rendered cache key. The normalized response Content-Type is passed as a hint and also isolates content-level rendered entries, but decoders should inspect the byte signature because servers may send incorrect headers. `addDecoder()` supplies decoders through `RenderOptions`; the standard `ImageRenderer` handles them. A fully custom renderer must honor the same options itself.

### Image Transformations

Transformations run in order after scaling and before the rendered image is cached. Aster includes grayscale, invert, opacity, tint, and box blur transformations:

```cpp
options.transformations = {
    aster::cache::ImageTransformation::grayscale(),
    aster::cache::ImageTransformation::tint(QColor("#4a90e2"), 0.2),
    aster::cache::ImageTransformation::blur(3),
};
```

The transformation order and each transformation's identifier, version, and parameters are part of the rendered cache key. Equivalent transformation instances therefore share rendered entries, while a parameter or order change creates a separate entry. Source caches continue to store the original bytes.

Use `ImageTransformation::create(...)` for a custom effect. The identity must remain stable for the same output and must change when the algorithm or its parameters change. The callable may run concurrently, must observe the cancellation flag, and must return an image with the same dimensions:

```cpp
auto custom = aster::cache::ImageTransformation::create(
    {"sample/custom-effect", 1, serializedParameters},
    [](QImage image, const std::atomic<bool>& cancelled) {
        if (cancelled.load())
            return aster::cache::ImageResult::failure(
                aster::cache::ImageError::Cancelled);
        applyCustomEffect(image);
        return aster::cache::ImageResult::success(std::move(image));
    });
options.transformations.push_back(custom);
```

For `ImageBox`, add the same objects to its fluent configuration with `.addTransformation(...)`. Replacing the transformation sequence starts a new request, and the resulting rendered cache entry remains reusable by other boxes.

### HTTP Interceptors

Use `HttpInterceptor::create(...)` to modify or observe requests, short-circuit responses, or retry the remaining chain:

```cpp
aster::cache::ImageServiceConfig config;
config.renderer = aster::cache::ImageRenderer{};
config.network = QSharedPointer<aster::cache::QtNetworkService>::create();
config.addInterceptor(aster::cache::HttpInterceptor::create(
    [](aster::cache::HttpInterceptorChain& chain,
       aster::cache::HttpRequest request,
       const std::atomic<bool>& cancelled) {
        request.options.headers.insert("authorization", "Bearer " + token());
        return chain.proceed(std::move(request), cancelled);
    }));
aster::cache::ImageService::configure(config);
```

Interceptors run in registration order only when a real HTTP request is needed; memory and disk cache hits bypass them. Add more interceptors by chaining further `.addInterceptor(...)` calls. Implementing `IHttpInterceptor` directly remains available for stateful reusable classes. Interceptors may be called concurrently and must honor the cancellation flag. Headers added by an interceptor do not automatically change the source cache key. Authentication-dependent content must use a stable `ImageSource::context.authScope` value so different accounts do not share cached data.

### Source Interceptors

`SourceInterceptor` processes bytes from every source type after the underlying source cache lookup and before image decoding. It can decrypt, decompress, validate, or unpack local files, Qt resources, memory data, and HTTP responses:

```cpp
config.addInterceptor(aster::cache::SourceInterceptor::create(
    [](aster::cache::SourceInterceptorChain& chain,
       aster::cache::SourceLoadRequest request,
       const std::atomic<bool>& cancelled) {
        auto result = chain.proceed(std::move(request), cancelled);
        if (!result || cancelled.load())
            return result;

        auto plain = decrypt(result.value->bytes);
        if (plain.isEmpty()) {
            return aster::cache::Result<aster::cache::SourcePayload>::failure(
                aster::cache::ImageError::ProcessingError,
                "Source decryption failed");
        }
        result.value->bytes = std::move(plain);
        result.value->contentType = "image/png";
        return result;
    }));
```

Source interceptors run in registration order and may short-circuit or retry the remaining chain. They may execute concurrently and must honor cancellation. The configured byte limit is checked again after interception. Encoded memory and source disk caches retain the original bytes; transformed bytes are hashed into the rendered cache key, so source-cache hits are intercepted again while rendered entries remain separated by transformed content.

### Pipeline Interceptors

`PipelineInterceptor` wraps the complete request, including request merging, source loading, decoding, rendering, and cache lookup. It can modify a request before cache keys are generated, observe the final `ImageResult`, or short-circuit the pipeline:

```cpp
config.addInterceptor(aster::cache::PipelineInterceptor::create(
    [](aster::cache::PipelineInterceptorChain chain,
       aster::cache::SourceRequest request,
       aster::cache::PipelineCompletion completion) {
        const auto started = std::chrono::steady_clock::now();
        return chain.proceed(
            std::move(request),
            [started, completion = std::move(completion)](
                aster::cache::ImageResult result) mutable {
                recordLoadTime(std::chrono::steady_clock::now() - started);
                completion(std::move(result));
            });
    }));
```

Pipeline interceptors run for every `SourceRequest` and string-source request, including requests whose final result comes from memory or disk cache. The legacy pre-keyed `ImageRequest` overload keeps its existing direct behavior. The chain is passed by value and can be retained for asynchronous retry or fallback. The `Subscription` returned by `proceed()` must be returned or retained; destroying it cancels downstream work. `waitForIdle()` tracks pipeline work after `proceed()` starts it, while interceptor-owned delayed scheduling remains the interceptor's responsibility.

`ImageServiceConfig::addInterceptor(...)` is overloaded for HTTP, source, and pipeline interceptor pointer types, so all three kinds use the same fluent configuration method.

Different pages can create independent pipelines when they require different interceptor chains:

```cpp
auto pageAConfig = baseConfig;
pageAConfig.addInterceptor(aster::cache::HttpInterceptor::create(pageAHandler));
pageAConfig.addInterceptor(aster::cache::SourceInterceptor::create(pageASourceHandler));
pageAConfig.addInterceptor(aster::cache::PipelineInterceptor::create(pageAPipelineHandler));

auto pageBConfig = baseConfig;
pageBConfig.addInterceptor(aster::cache::HttpInterceptor::create(pageBHandler));
pageBConfig.addInterceptor(aster::cache::SourceInterceptor::create(pageBSourceHandler));
pageBConfig.addInterceptor(aster::cache::PipelineInterceptor::create(pageBPipelineHandler));

auto pageAPipeline = aster::cache::ImageService::createPipeline(pageAConfig);
auto pageBPipeline = aster::cache::ImageService::createPipeline(pageBConfig);
pageAImageBox->setPipeline(pageAPipeline);
pageBImageBox->setPipeline(pageBPipeline);
```

`createPipeline()` does not read or change the global `ImageService` state. Each returned pipeline owns its own memory caches and interceptor chain. Configuration objects may share injected dependencies only when those dependencies support concurrent access. If interceptors can produce different content for the same URL, use distinct `ImageSource::context.authScope` values, cache namespaces, or disk caches to prevent cross-page cache reuse.

## ImageBox Example

```cpp
#include "aster/cache/service/imageservice.h"
#include "aster/gui/imagebox.h"

auto* box = new aster::gui::ImageBox(parent);
box->setPipeline(aster::cache::ImageService::pipeline());
const auto imageBoxConfig = aster::gui::ImageBoxConfig()
                                .fit(aster::gui::ImageFit::Contain)
                                .scaleAlgorithm(aster::gui::ImageScaleAlgorithm::Lanczos3)
                                .cornerRadius(12)
                                .placeholder(placeholder)
                                .build();
box->setConfig(imageBoxConfig);
box->setSource("C:/images/photo.jpg");

QObject::connect(box, &aster::gui::ImageBox::loadFailed, parent,
                 [](aster::cache::ImageError error, const QString& message) {
    qWarning() << int(error) << message;
});
```

ImageBox handles request generations, cancellation, late results, state changes, and painting. String sources support local paths, `file:`, `qrc:`, and HTTP(S) URLs.

Display settings can be applied through the fluent configuration object:

```cpp
auto config = aster::gui::ImageBoxConfig()
                  .fit(aster::gui::ImageFit::Contain)
                  .scaleAlgorithm(aster::gui::ImageScaleAlgorithm::Lanczos3)
                  .cornerRadius(12)
                  .transition(aster::gui::ImageTransition::Fade)
                  .transitionDuration(200)
                  .build();
box->setConfig(config);
```

Default values are documented in `imageboxconfig.h`: Cover, QtSmooth, 75 ms resize debounce, bucket 1, no placeholder/error replacement, CrossFade transition, FirstLoadOrNonMemoryCache transition policy, 200 ms transition duration, zero corner radius, and Keep offscreen policy. NonMemoryCache skips transitions for ActiveResource, RenderedMemory, and EncodedMemory results. FirstLoadOrNonMemoryCache still animates the first successful load of each newly created ImageBox, including memory-cache hits, while later memory hits on the same widget skip the transition. Reuse one configuration value across ImageBox instances and update it through the fluent methods when shared display settings change.

Widget-based loading and error content uses a factory so a shared configuration creates an independently owned widget for every ImageBox:

```cpp
config.loadingErrorWidget([](QWidget* parent) {
    return new CustomStatusWidget(parent);
});
```

## Examples

- `aster_gallery` demonstrates folder browsing, HTTP endpoints, caching, and ImageBox.
- `aster_cache_widget` compares all six scaling algorithms using only `aster::cache`.

## Performance Reference

`aster_benchmark` measures cache get, put, eviction, and concurrent access operations and reports p50/p95 latency. `tests/resampler_tests.cpp` and `aster_cache_widget` help compare scaling quality and cost. Results depend on the CPU, compiler, Qt version, image dimensions, DPR, cache hit rate, and storage. Run benchmarks with a Release build on the target machine.

## AI Assistance

AI tools assisted with architecture exploration, API drafting, implementation iterations, test authoring, formatting, and Qt 5/6 build validation.

## License

aster is distributed under the [MIT License](LICENSE).
