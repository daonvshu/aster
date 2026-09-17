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
 SourceLoader Pipeline Renderer
      |          |         |
 文件/网络    合并/取消   解码/缩放
      |          |         |
 编码缓存      渲染缓存     磁盘缓存
```

`aster::cache` 负责来源识别、加载、缓存、解码、缩放和 `ImagePipeline`。`aster::gui` 依赖 cache，负责 QWidget 生命周期和绘制。CMake 目标为 `aster::cache` 和 `aster::gui`。

## 构建

```sh
cmake -S . -B build -DASTER_QT_MAJOR=6 -DCMAKE_PREFIX_PATH=/path/to/Qt/6
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Qt5 将 `ASTER_QT_MAJOR` 设置为 `5`。使用 `ASTER_BUILD_GUI=OFF` 构建仅包含 cache 的版本，使用 `ASTER_BUILD_GALLERY=OFF` 关闭 Gallery 示例。

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

`ImageServiceConfig::addInterceptor(...)` 针对 HTTP 和 Source 拦截器指针类型提供重载，因此两类拦截器使用同一个链式配置函数。

不同页面需要不同拦截器链时，可以分别创建独立 Pipeline：

```cpp
auto pageAConfig = baseConfig;
pageAConfig.addInterceptor(aster::cache::HttpInterceptor::create(pageAHandler));
pageAConfig.addInterceptor(aster::cache::SourceInterceptor::create(pageASourceHandler));

auto pageBConfig = baseConfig;
pageBConfig.addInterceptor(aster::cache::HttpInterceptor::create(pageBHandler));
pageBConfig.addInterceptor(aster::cache::SourceInterceptor::create(pageBSourceHandler));

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
