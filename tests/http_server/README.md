# Image Test Server

Python 3.9+，仅标准库，ThreadingHTTPServer，每个请求独立线程。属于测试基础设施，不由 aster 产品库依赖。

## 手动启动

```powershell
python tests/http_server/server.py --images "D:\pictures" --port 8765
```

省略 `--port` 使用系统分配端口，启动第一行输出 JSON：`{"port": 12345, "url": "http://127.0.0.1:12345"}`。默认仅绑定 127.0.0.1，可通过 --host 指定。Ctrl+C 关闭。图片从指定目录读取，支持子目录与 URL 编码文件名，拒绝目录越界和越界符号链接。不需要上传或复制用户图片到仓库。

| 路径 | 行为 |
|---|---|
| /health | 200，返回 ok |
| /image/avatar.png | 原始图片字节，正确 MIME，no-store |
| /delay/3/avatar.png | 等待 3 秒再响应，范围 0～30 秒 |
| /status/404、/status/500 | 指定错误码 |
| /redirect/avatar.png | 302，Location 指向 /image/avatar.png |
| /etag/avatar.png | SHA-256 ETag，no-cache；If-None-Match 相同时返回 304 |
| /cache/avatar.png | public, max-age=3600 |
| /no-store/avatar.png | no-store |
| /wrong-content-type/avatar.png | text/plain，但图片字节保持正确 |
| /corrupt/avatar.png | 正确图片 MIME，内容替换为损坏数据 |
| /truncate/avatar.png | 声明完整长度，发送一半后关闭连接 |
| /oversize/avatar.png | 40MiB 响应，以 64KiB 块发送，不分配整份超大内容 |
| /stream/avatar.png | 大约 20 个块，每块间隔 50ms |

stream 仅模拟分段网络传输，Aster 当前仍等完整下载后解码，不代表已实现 progressive 图片显示。服务器会读取源文件进入内存，仅用于受控测试图片目录。

## 自动化 / CI

启用 GUI、QtNetwork 和 BUILD_TESTING 时默认构建 aster_http_tests，需要 Python 3.9+。可通过 `-DASTER_BUILD_HTTP_TESTS=OFF` 关闭。

```text
cmake --build <build-dir> --target aster_http_tests
ctest --test-dir <build-dir> -R aster_http --output-on-failure
```

CTest 调用 run_tests.py：生成两张真实有效的确定性 PNG 文件（avatar.png 红色、landscape.png 蓝色），在临时目录启动服务，使用随机独立端口，将地址写入测试进程环境变量 ASTER_IMAGE_TEST_URL，运行 Qt Test，再通过 finally 关闭服务器。子进程超时默认 100 秒，会杀死并等待测试进程；CTest 外层超时 120 秒。无需额外下载图片、pip 或后台常驻进程，可同时运行 Qt5/Qt6 测试。

通用启动器也支持指定图片目录并执行其他测试命令：

```text
python tests/http_server/run_tests.py --images <directory> -- <test-command> <args>
```

内置 Qt Test 期望 avatar.png 和 landscape.png 为上述颜色，因此运行内置测试建议使用默认生成的图片。手动测试或其他测试程序可以使用任意真实图片目录。

Qt Test 覆盖真实 QtNetworkService、CachedSourceLoader/FileDiskCache、ImagePipeline 与 ImageBox。现有重定向策略是 ManualRedirectPolicy：测试验证 302/Location，pipeline 返回错误；不会自动跨站跟随。错误 Content-Type 当前由内容探测解码，测试验证有效 PNG 仍可显示。no-store 的失败/取消场景不写缓存。

CI 工作流已增加 setup-python，并构建 aster_http_tests；正常 ctest 自动执行“启动 → 测试 → 关闭”。.github 仍按仓库约定被忽略，本地工作流修改尚未发布到远端。
