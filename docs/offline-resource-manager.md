# 首选项内的离线资源管理（客户端 0.4.1 起）

## 用户流程

首选项 → 翻译 → 本机离线 → 下载并启用。

客户端先检查运行库和磁盘空间，再显示下载量、来源和隐私说明。用户确认后：

1. 直接从官方发布渠道下载资源。
2. 逐个核对 SHA-256。
3. 解压 llama.cpp。
4. 运行真实的离线翻译自检。
5. 自检通过后，才保存并启用资源目录。

正常截图、贴图和选择处理方式不会触发下载。离线推理不会调用资源管理器，也没有在线翻译回退。

## 资源与来源

Visnip 不托管、不转发任何资源文件，服务器也不参与下载。下载地址和 SHA-256 固定写在 `src/core/OfflineResourceCatalog.cpp` 里，谁都可以核对。

| 文件 | 来源（按顺序尝试） | 大小 | SHA-256 |
| --- | --- | --- | --- |
| `llama-b10964-bin-win-cpu-x64.zip`（llama.cpp 官方 CPU 构建） | GitHub `ggml-org/llama.cpp` release b10964 | 17.6 MiB | `917f39c0…df4c5d7` |
| `Hy-MT2-1.8B-Q4_K_M.gguf`（腾讯 Hy-MT2 翻译模型） | 魔搭社区 `Tencent-Hunyuan/Hy-MT2-1.8B-GGUF`（固定提交 `ef1d40b8`）→ Hugging Face `tencent/Hy-MT2-1.8B-GGUF`（固定提交 `a0c709d9`） | 1.06 GiB | `dc5f44fc…0c06699` |

两个模型来源发布的是同一个文件，哈希相同。魔搭社区在国内可直接访问，所以排在前面。

安装后的目录结构是 `llama/`（官方 zip 原样解压）加上 `models/Hy-MT2-1.8B-Q4_K_M.gguf`。

精细档需要 Python、PyTorch、Hi-SAM 和 LaMa，上游没有现成的整包，所以不再提供下载。以前版本装好的精细资源仍然可以使用；在首选项里，只有已安装时才会显示精细档。

## 运行库

llama.cpp 的官方 Windows 构建依赖 Microsoft Visual C++ 2015-2022 运行库（`msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll`）。Windows 自带 UCRT，但不一定装了这个运行库。

客户端会在下载前检查 System32 和 `llama/` 目录。缺少时直接提示安装微软官方的可再发行程序包（https://aka.ms/vs/17/release/vc_redist.x64.exe），不会先下载 1 GiB 模型再失败。

## 边界

- 只走 HTTPS。重定向逐次核对：只允许跳到 `github.com`、`githubusercontent.com`、`modelscope.cn`、`huggingface.co`、`hf.co` 及其子域名，其他地址一律拒绝并换下一个来源。不附带 Cookie 或凭据。
- 每次请求是一个开放的 Range（`bytes=<已下载>-`），中断后从已下载位置续传。
  - 遇到 429/5xx 时丢弃错误页面，在同一来源重试。
  - 连接不上或返回 4xx 时，改用下一个来源。不同来源内容相同，已下载的部分保留。
- 文件写满后才核对 SHA-256，不一致就删除，不会使用。损坏的完整缓存不会在没有再次确认的情况下触发全量下载。
- llama.cpp 的 zip 缓存在 `downloads/`。模型直接下载到运行位置，不重复占用 1 GiB 空间。
- 解压使用 Windows 10 1803 起自带的 `System32\tar.exe`（bsdtar），先解到临时目录。拒绝符号链接，确认有 `llama-server.exe` 后再放到位。tar 在临时目录内运行，并使用相对路径，所以用户名里的中文等字符不会经过它的命令行。
- 新资源安装到 `managed/` 下的独立目录，不覆盖正在使用的资源。下载、解压或自检失败都不会切换配置。

## 目录

`%LOCALAPPDATA%/Visnip/offline/downloads/` 保存 zip 缓存，成功的资源在相邻的 `managed/` 里。磁盘空间不足时会先拒绝，不会清理用户已有的模型或截图。

## 验证

`visnip_offline_resource_tests` 覆盖以下内容：

- 固定目录的完整性；
- 重定向主机白名单；
- HTTP Range；
- 同一来源重试和换来源续传；
- 哈希失败的处理；
- 首选项的显式授权边界；
- 精细档不可下载。

真实的下载、解压和自检是单独的验收。显式设置 `VISNIP_RESOURCE_REAL_TEST=lite` 和 `QTEST_FUNCTION_TIMEOUT=1800000`，再运行 `realProvisioningWhenExplicitlyRequested`。它会真实下载约 1.1 GiB，不纳入默认测试。
