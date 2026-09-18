# aster

[English](README.md)

aster 是一个基于 C++17、Qt 5/6 和 CMake 的异步图片加载与缓存框架。核心模块不依赖 Qt Widgets。

> **实验性项目：** aster 仍在持续开发中，不保证正确性、API 稳定性、性能或兼容性。请谨慎使用，在用于生产环境前务必充分验证。

## 特性

- 支持本地文件、Qt 资源、内存数据和 HTTP 图片来源。
- 提供线程安全的内存 LRU、可选磁盘缓存、TTL、ETag/304 校验和统计。
- 合并并发请求，订阅可独立取消。
- 支持 QtFast、QtSmooth、Bilinear、Bicubic、Lanczos3 和 Lanczos4 缩放算法。
- ImageBox 提供状态、占位图、圆角、过渡动画和离屏资源策略。

## 整体架构

```text
     ImageBox / 自定义 QWidget
                 |
            aster::gui
                 |
           aster::cache
        +--------+--------+
 SourceLoader Pipeline    Renderer
      |          |           |
 文件/网络    合并/取消    Decoder 链
      |          |        缩放/图片转换
 编码缓存      渲染缓存      磁盘缓存
```

`aster::cache` 负责来源识别、加载、缓存、解码、缩放和 `ImagePipeline`。`aster::gui` 依赖 cache，负责 QWidget 生命周期和绘制。CMake 目标为 `aster::cache`，以及可选的 `aster::network` 和 `aster::gui`。

## 构建

```sh
cmake -S . -B build -DASTER_QT_MAJOR=6 -DCMAKE_PREFIX_PATH=/path/to/Qt/6
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Qt5 将 `ASTER_QT_MAJOR` 设置为 `5`。使用 `ASTER_BUILD_GUI=OFF` 构建仅包含 cache 的版本，使用 `ASTER_BUILD_GALLERY=OFF` 关闭 Gallery 示例。

## 安装与使用

在已配置的构建目录中安装库和 CMake 包配置：

```sh
cmake --install build --config Release --prefix /path/to/aster-install
```

下游应用可以通过 `find_package` 导入导出的目标：

```cmake
find_package(aster CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE aster::cache)
# 使用 ImageBox 时再链接 aster::gui。
```

包配置会记录构建时使用的 Qt 主版本和可选组件。启用 `ASTER_BUILD_QT_NETWORK=ON` 时会导出 `aster::network`；启用 `ASTER_ENABLE_SQLITE_DISK_CACHE=ON` 时会声明 SQLite 依赖。

## cache 使用示例

不依赖 `aster::gui` 时，可以直接订阅 Pipeline：

```cpp
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/service/imageservice.h"

aster::cache::ImageServiceConfig config;
config.renderer = aster::cache::ImageRenderer{};
config.renderedMemoryBytes = 64 * 1024 * 1024;
config.encodedMemoryBytes = 32 * 1024 * 1024;
config.workerCount = 4; // 最大并行 source/render worker 数
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

`ImageSubscription` 由 Qt parent 管理。调用 `cancel()` 或销毁 parent 会取消订阅。`physicalTargetSize` 使用物理像素。

### 磁盘存储

`IDiskCache` 是原始 source 条目和 rendered 条目的存储边界。默认情况下，`ImageService::configure()` 和 `ImageService::createPipeline()` 会分别创建位于 `QStandardPaths::CacheLocation/source/aster-cache-v1` 与 `QStandardPaths::CacheLocation/rendered/aster-cache-v1` 的 `FileDiskCache`。Qt 会根据组织名和应用名解析缓存路径，`FileDiskCache` 会追加版本目录以隔离磁盘格式。默认预算均为 256 MiB，单条上限为 32 MiB。可以通过 `diskCacheDirectory`、`sourceDiskBytes`、`renderedDiskBytes` 和 `maxDiskEntryBytes` 调整；设置 `enableDefaultDiskCache = false` 可以关闭默认磁盘缓存。

Aster 还提供使用事务的 SQLite 实现，可显式替换默认文件缓存：

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

`SqliteDiskCache` 使用事务原子替换条目，把 metadata 与数据一起保存，读取时校验 SHA-256，并在超过字节预算时按最近最少使用策略驱逐。应用也可以针对其他数据库、加密存储或对象存储实现 `IDiskCache`，然后注入到上述两个位置。自定义实现必须支持并发调用，写入失败时保留旧条目，并在维护操作中响应取消。

调用 `pipeline->clearDiskCaches()` 可以同时清空该 Pipeline 的 source 与 rendered 磁盘缓存；`pipeline->cacheStats()` 分别返回两者的占用和命中统计。

SQLite 后端由 CMake 选项 `ASTER_ENABLE_SQLITE_DISK_CACHE` 控制，默认值为 `ON`。设置为 `OFF` 后，构建不会查找或链接 QtSql，也不会编译 SQLite 后端和相关测试。

### 自定义 Decoder

可以使用 `ImageDecoder::create(...)` 给 Pipeline 注册自定义静态图片格式。自定义 Decoder 按注册顺序匹配，全部不匹配时 Aster 才会使用受资源限制保护的 Qt Decoder：

```cpp
#include "aster/cache/decoder/imagedecoder.h"

config.addDecoder(aster::cache::ImageDecoder::create(
    {"sample/custom-format", 1, serializedParameters},
    [](const QByteArray& bytes, const QByteArray& contentType) {
        return bytes.startsWith("CSTM"); // 优先检查文件签名，不要只依赖 Content-Type。
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

某个 Decoder 声明支持后，其结果就是最终结果；损坏的自定义图片不会继续交给其他 Decoder。匹配器和解码函数可能被并发调用，必须保证线程安全。解码函数在分配内存时必须响应取消，并遵守 `DecodeContext::limits`。Aster 还会在外层再次检查空图、尺寸、像素数和实际解码字节数。

Decoder 身份、参数和顺序都会进入 rendered cache key。规范化后的响应 Content-Type 会作为提示传入，并用于隔离内容级 rendered cache 项，但服务端可能返回错误 Header，因此 Decoder 仍应检查字节签名。`addDecoder()` 通过 `RenderOptions` 提供 Decoder，标准 `ImageRenderer` 会执行它们；完全自定义的 renderer 需要自行遵守相同选项。

### 图片转换

图片转换会在缩放之后、写入 rendered cache 之前按顺序执行。Aster 内置灰度、反色、透明度、色调混合和盒式模糊转换：

```cpp
options.transformations = {
    aster::cache::ImageTransformation::grayscale(),
    aster::cache::ImageTransformation::tint(QColor("#4a90e2"), 0.2),
    aster::cache::ImageTransformation::blur(3),
};
```

转换顺序以及每个转换的标识、版本和参数都会进入 rendered cache key。因此，等价的转换实例可以复用同一缓存项，修改参数或顺序则会生成不同缓存项；源缓存仍然保留原始字节。

可以使用 `ImageTransformation::create(...)` 实现自定义效果。相同输出必须使用稳定的身份，算法或参数变化时必须更新版本或参数。回调可能被并发执行，必须检查取消标记，并且返回图片的尺寸必须保持不变：

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

`ImageBox` 可以通过链式配置 `.addTransformation(...)` 添加相同的转换对象。替换转换序列会发起新请求，生成的 rendered cache 项仍可由其他 ImageBox 复用。

### HTTP 拦截器

使用 `HttpInterceptor::create(...)` 可以修改或观察请求、短路返回响应，或者重试后续拦截器链：

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

拦截器按照注册顺序执行，并且只在确实需要发起 HTTP 请求时运行；命中内存或磁盘缓存时不会执行。继续链式调用 `.addInterceptor(...)` 可以添加多个拦截器；需要封装可复用状态时仍可直接实现 `IHttpInterceptor`。拦截器可能被并发调用，且必须响应取消标记。拦截器添加的请求头不会自动改变源缓存键；依赖登录身份的内容必须设置稳定的 `ImageSource::context.authScope`，避免不同账号共用缓存数据。

### Source 拦截器

`SourceInterceptor` 在底层源缓存查找之后、图片解码之前处理所有来源的字节，可用于解密、解压、校验或拆包本地文件、Qt 资源、内存数据和 HTTP 响应：

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

Source 拦截器按注册顺序运行，可以短路返回或重试后续链。它可能被并发调用，并且必须响应取消标记；配置的字节上限会在拦截完成后再次检查。编码内存缓存和源磁盘缓存保留原始字节，转换后字节的哈希会进入渲染缓存键，因此命中源缓存时仍会执行拦截器，不同转换内容的渲染缓存也会保持隔离。

### Pipeline 拦截器

`PipelineInterceptor` 包围包含请求合并、源加载、解码、渲染和缓存查找在内的完整请求。它可以在生成缓存键之前修改请求、观察最终 `ImageResult`，也可以短路整个 Pipeline：

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

每个 `SourceRequest` 和字符串来源请求都会执行 Pipeline 拦截器，包括最终结果来自内存或磁盘缓存的请求；旧的预计算键 `ImageRequest` 重载保持原有直接执行行为。拦截器链按值传递，可以保留后用于异步重试或备用源；`proceed()` 返回的 `Subscription` 必须直接返回或自行持有，销毁它会取消下游任务。`waitForIdle()` 只跟踪 `proceed()` 启动后的 Pipeline 工作，拦截器自己延迟调度的任务需要自行管理。

`ImageServiceConfig::addInterceptor(...)` 针对 HTTP、Source 和 Pipeline 三种拦截器指针类型提供重载，因此它们使用同一个链式配置函数。

不同页面需要不同拦截器链时，可以分别创建独立 Pipeline：

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

`createPipeline()` 不会读取或改变全局 `ImageService` 状态。每个返回的 Pipeline 拥有自己的内存缓存和拦截器链；只有在线程安全的前提下，多个配置才应共享注入的依赖实例。如果不同拦截器可能让同一 URL 产生不同内容，应使用不同的 `ImageSource::context.authScope`、缓存命名空间或磁盘缓存，避免跨页面复用错误数据。

## ImageBox 使用示例

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

ImageBox 自动处理请求 generation、取消、迟到结果、状态变化和绘制。字符串来源支持本地路径、`file:`、`qrc:` 和 HTTP(S) URL。

ImageBox 的显示参数可以集中使用 fluent 配置：

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

配置类方法的默认值写在 `imageboxconfig.h`：Cover、QtSmooth、75 ms 防抖、bucket 1、无占位/错误替换、CrossFade 动画、FirstLoadOrNonMemoryCache 动画触发策略、200 ms 动画时长、0 圆角和 Keep 离屏策略。NonMemoryCache 会跳过 ActiveResource、RenderedMemory 和 EncodedMemory 结果的动画。FirstLoadOrNonMemoryCache 会让每个新建 ImageBox 的首次成功加载仍执行动画，包括内存缓存命中；同一控件后续命中内存缓存时则跳过动画。多个 ImageBox 可以复用同一个配置值；公共显示参数变化时，通过 fluent 方法更新配置并统一下发即可。

使用 widget 替换加载和错误内容时通过工厂创建，因此同一配置用于多个 ImageBox 时，每个 ImageBox 都会得到自己独立持有的 widget：

```cpp
config.loadingErrorWidget([](QWidget* parent) {
    return new CustomStatusWidget(parent);
});
```

## 示例程序

- `aster_gallery`：演示文件夹浏览、HTTP 测试接口、缓存和 ImageBox。
- `aster_cache_widget`：仅使用 `aster::cache`，并排对比六种缩放算法。

## AI 辅助编写说明

AI 工具参与了架构梳理、接口草拟、实现迭代、测试编写、格式化以及 Qt5/Qt6 构建验证。

## 许可证

aster 使用 [MIT 许可证](LICENSE)发布。
