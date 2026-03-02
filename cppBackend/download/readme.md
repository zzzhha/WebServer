# 分块下载（Chunked Download）

本模块提供一个“下载客户端侧”的分块下载管理器，并配套后端的 Range(206) 能力，使大文件可以被拆成多个区间并发拉取、断点续传、最终合并与校验。

## 服务端支持（必要前置）

本仓库已为以下路由增加：
- `GET/HEAD /download/*`（下载）
- `GET/HEAD /images/*`、`GET/HEAD /video/*`（静态文件）

Range 请求示例：

```http
GET /download/video/xxx.mp4 HTTP/1.1
Host: 127.0.0.1:18080
Range: bytes=0-1048575

```

若 Range 不合法，会返回 `416 Range Not Satisfiable` 并带 `Content-Range: bytes */<total>`。

MD5 获取（可选）：
- `HEAD ...?md5=1` 或请求头 `X-Request-MD5: 1`
- 响应头 `X-File-MD5: <hex>`

## 客户端 API

核心头文件：
- [ChunkedDownloadManager.h](file:///home/zsy/WebServer/cppBackend/download/include/ChunkedDownloadManager.h)

关键能力：
- 分块：`chunk_size_bytes`（默认 1MB）
- 并发：`max_concurrency`（建议 3-5）
- 重试：`max_retries`（默认 3）
- 超时：`timeout_ms`
- 断点续传：`<dest>.meta` 持久化每个块状态与重试次数
- 落盘：每个块写入 `<dest>.partN`
- 合并：全部完成后合并为 `<dest>`，并清理 `.meta`
- 完整性：若服务器返回 `X-File-MD5`，会在合并后计算本地 MD5 并比对

### 使用示例

```cpp
#include "ChunkedDownloadManager.h"
#include <atomic>
#include <iostream>

int main() {
  ChunkedDownloadConfig cfg;
  cfg.chunk_size_bytes = 1024 * 1024;
  cfg.max_concurrency = 4;
  cfg.max_retries = 3;
  cfg.timeout_ms = 3000;

  std::atomic<DownloadState> st{DownloadState::Idle};

  ChunkedDownloadCallbacks cb;
  cb.on_state = [&](DownloadState s) {
    st.store(s);
    std::cout << "state=" << static_cast<int>(s) << "\n";
  };
  cb.on_progress = [&](double percent, uint64_t done, uint64_t total, double bps) {
    std::cout << "progress=" << percent << "% "
              << done << "/" << total
              << " speed=" << ChunkedDownloadManager::FormatSpeed(bps) << "\n";
  };
  cb.on_error = [&](DownloadError e, const std::string& msg) {
    std::cout << "error=" << static_cast<int>(e) << " msg=" << msg << "\n";
  };

  std::string url = "http://127.0.0.1:18080/download/video/xxx.mp4";
  std::string out = "/tmp/xxx.mp4";
  ChunkedDownloadManager mgr(url, out, cfg, cb);

  if (!mgr.Start()) return 1;
  mgr.Wait();

  return st.load() == DownloadState::Completed ? 0 : 2;
}
```

## 状态与回调约定

- `on_state`：开始/暂停/恢复/完成/失败/取消
- `on_progress`：实时进度（百分比精确到 0.1%）、已下载字节、总字节、下载速度（bytes/s）
- `on_chunk`：每个块的状态变化（NotStarted/Downloading/Completed/Failed）与 retries
- `on_error`：错误码与错误描述

## 测试

- [chunked_download_tests.cpp](file:///home/zsy/WebServer/test/chunked_download_tests.cpp)
- 构建产物：`build/test/chunked_download_tests`

