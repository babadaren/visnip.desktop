# 本机离线 · 轻量档

更新日期：2026-10-01

## 目标

开源版默认的离线翻译要轻、译文要尽量好、要尽量快。目标是 100 字左右的小截图，从 OCR 到译文回填显示，总耗时 3–5 秒。

精细档（Python + PyTorch + Hi-SAM + LaMa，约 2.4 GiB 下载、6.2 GiB 安装）不适合当默认。轻量档只保留翻译模型，其余步骤都在客户端进程内完成。

## 链路

```text
选区截图
  -> OcrService（C++，ONNX Runtime，PP-OCRv4 检测 + PP-OCRv5 识别，随客户端发布）
  -> filterTranslatableLines / makeLineTranslationUnits / mergeLinesIntoBlocks（与混合模式相同）
  -> LocalMt::planParagraphs：换行的正文合成一个段落，标签、菜单、按钮保持单条
  -> LocalTextTranslationService -> 本机 llama-server（Hy-MT2-1.8B Q4_K_M）
  -> LocalMt::distributeTranslations：段落译文按原行宽拆回各行
  -> composeTranslatedImage（C++，QPainter，与混合模式相同）
```

资源只需要基础包里的两样东西：`llama/llama-server.exe` 和 `models/Hy-MT2-1.8B-Q4_K_M.gguf`。首选项选「轻量」时只下载基础包（约 1.2 GiB），不安装也不调用 Python。已经装过精细资源的目录同样满足轻量档。

## 关键设计

| 项 | 做法 |
| --- | --- |
| 进程与网络 | llama-server 作为子进程，只监听 `127.0.0.1` 的随机端口，每次启动生成新的 API 令牌，带 `--offline`。环境变量只保留系统必需项，不继承代理。Windows 上用 Job Object（`KILL_ON_JOB_CLOSE`），客户端崩溃时服务进程一并结束。 |
| 提示词与校验 | 与精细档 `native_translation.py` / `translation_policy.py` 一致：Hy-MT 官方上下文模板，页面上下文作为共享前缀，术语表只加入本段出现的词条。数字按值比较（1,200 = 1200），占位符逐字比较。不合格时用不带上下文的提示词重试一次；两次都原样返回的短专有名词（如 Copilot）视为保留，其余失败的段保留原文并在状态栏计数。 |
| 段落翻译 | 合并要满足以下条件：同一视觉块、首行已是正文（至少 5 个词或 16 个汉字）、上一行没有以句末标点结束、下一行左边缘对齐且不比上一行宽。拆回时只在空格或中日韩字符之间断开，不拆开英文单词和数字，句末标点不放到行首。拆不开时整段保留原文。 |
| 并发 | 服务固定开 4 个槽，但每次翻译的实际并发数按实测速度选 1、2 或 4（`LocalMt::chooseConcurrency`）。每个用到的槽都要把共享上下文 prefill 一遍，并行 decode 则能分摊生成时间。prefill 慢的 CPU 选串行，这样上下文只算一次、之后都命中缓存；显卡和较新的 CPU 选并行。速度在模型就绪前的计时预热里测出。 |
| 生命周期 | 程序启动时、按 F1 截图时、开始 OCR 时都会预热，已在运行就什么都不做。可用内存低于 3 GB 时不预热。空闲 10 分钟释放，可用内存低于 1.5 GB 时也释放。只有当前选中的离线档持有模型内存。 |
| 显卡（可选） | 和精细档使用同一个 `engine-runtime` 约定：清单锁定 llama.cpp b10964，逐文件校验 SHA-256，限定在 `runtime/` 目录内。优先使用 NVIDIA 设备并确认层确实卸载到显卡；失败就自动退回 CPU 版。 |
| 缓存 | 同一进程内按提示词缓存最多 512 条译文。 |
| 隐私 | 离线模式下日志只记录 OCR 文字的长度，不记录提示词和译文。 |

## 实测（2026-10-01）

测试机：4 vCPU Xeon E5-2696 v4 虚拟机（AVX2），同时有一个常驻进程占满约一个核，没有显卡。这台机器明显慢于普通台式机和笔记本，数据只能作为下限参考。

llama.cpp b10964（从源码编译，CPU），Hy-MT2-1.8B Q4_K_M（SHA-256 与 `provenance.json` 一致）。

| 项目 | 结果 |
| --- | --- |
| `llama-bench`，3 线程 | prefill 32.2 token/s，decode 11.7 token/s |
| 服务内计时预热，2 线程 | prefill 23.6 token/s，decode 10.0 token/s，自适应选择串行 |
| 约 100 个英文词的设置对话框，8 段，英译中 | 热态 15.7 s：实际 prefill 173 token 用 8.0 s，生成 73 token 用 7.5 s，没有重试，全部通过校验 |
| 同样内容，中译英 | 热态 15.7 s |
| 冷启动（加载模型 + 两次预热） | 10.6 s；正常使用时由启动和 F1 预热提前完成 |
| 固定 4 并发（改成自适应之前） | 热态 23–25 s，每个槽都重复 prefill 上下文 |

译文样例：「Visnip keeps every capture on this computer. When you translate a region, only the selected pixels are read, and the text never leaves your device in offline mode.」→「Visnip会保存此电脑上的所有截图。当您翻译某个区域时，只有选中的像素会被读取，且在离线模式下文本不会离开您的设备。」

### 换算到其他机器（估算，需实测）

100 字左右的截图，翻译阶段大约需要 prefill 170 token、生成 75 token。总耗时约等于：

```text
OCR (0.2–0.5 s) + 170 / prefill速度 + 75 / decode速度（串行时）+ 回填 (<0.2 s)
```

| 机器 | 假设速度 | 估算总耗时 |
| --- | --- | --- |
| 本测试机 | 24 / 10 token/s | 约 16 s（实测） |
| 较新的 4 核以上笔记本或台式 CPU | 约 300 / 25 token/s | 约 3–4 s，自适应选 2 并发 |
| 任意 Vulkan 或 CUDA 显卡 | 约 2000+ / 100+ token/s | 约 1–1.5 s |

客户端日志 `OfflineEngine.lite_ready` 会记录每台机器实测的 `prefill_tps` 和 `decode_tps`，`LiteMt.job` 会记录每次翻译的拆分耗时。可以用它们核对上表。

## 验证

- `visnip_local_translation_tests`：纯逻辑规则 13 项，外加通过测试替身 `visnip_fake_llama_server` 的端到端测试 6 项，覆盖进程启动、令牌鉴权、并发、重试、保留名、校验失败、缓存、取消、空闲释放和自检。
- 真实模型验收（默认跳过）：`VISNIP_LITE_REAL_ROOT=<含 llama/ 和 models/ 的目录> visnip_local_translation_tests realModelWhenProvided`。
- `tst_offline_resources`、`tst_translation_modes` 已改为两档设置的断言。它们依赖 Windows 专用的 OCR 和截图代码，需要在 Windows 上构建运行。

## 尚未完成

- Windows 上的完整构建、界面和截图翻译还没有实机验证（本次只在 Linux 上编译检查，Windows 专用代码用 mingw 做了语法检查）。显卡路径还没有在真实显卡上验证。
- 逐段渐进显示（先译完的段先回填）还没有实现。
- 轻量专用资源包（只含 llama-server 和模型，不含 Python）需要服务端重新打包；现在复用基础包，其中仍带 Python 运行时。
- 空间不足而保留原文的段，还没有接入精细档那样的译文面板。
