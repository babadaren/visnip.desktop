# Visnip Desktop

Visnip（0.4.0 之前名为 Vislate）是一个 Windows-first 的轻量截图与贴图工具。当前实现按 `docs/` 中的 v0.3 Compact 设计推进：30px 小巧工具栏、16 个高频按钮、截图/标注/复制/保存/贴图闭环。

## 技术栈

- C++20
- Qt 6 Widgets + QPainter + Qt SVG
- Windows 全局快捷键：Win32 `RegisterHotKey`
- 截图：Win32 GDI 快速路径 + Qt `QScreen::grabWindow(0)` 多屏/高 DPI 回退
- 构建：CMake + Ninja + MinGW

## 主要功能

- 托盘常驻，右键菜单包含截图、贴图、重复上次区域、设置、退出。
- 全局快捷键：`F1` 截图、`F3` 从剪贴板贴图、`Ctrl+F1` 重复上次区域、`Shift+F3` 隐藏/显示贴图、`Ctrl+T` 当前贴图穿透。
- 截图覆盖层：多屏、遮罩、选区、尺寸标签、取色标签、放大镜、长截图。
- Compact 工具栏：矩形、箭头、画笔、文字、马赛克、橡皮、序号、长截图、撤销、重做、贴图、保存、复制、翻译、解题、关闭。
- 解题面板：工具栏点「题」后遮罩关闭，屏幕侧边出现常驻面板并记住选区；点击「获取答案」或按 `F4`（仅面板打开时注册）截取选区当前画面，发送给首选项中配置的多模态大模型（OpenAI 兼容或 Anthropic 格式），流式显示答案。设计见 `docs/question-panel-design.md`。
- 贴图窗口：置顶、拖拽、滚轮缩放、`Ctrl+滚轮` 透明度、右键复制/保存/穿透/关闭。
- 设置窗口：常规、界面、截图、贴图、输出、翻译、解题、快捷键、关于。

## 下载

在 [Releases](../../releases) 下载 `Visnip-<版本>-windows-x64.zip`，解压后运行 `visnip.exe`，并用同名 `.sha256` 文件核对下载完整性。安装包目前还没有代码签名，首次运行时 Windows SmartScreen 可能会提示。本机离线翻译资源不在压缩包中，需要时在「首选项 → 翻译 → 本机离线」中下载。版本变化见 [CHANGELOG.md](CHANGELOG.md)。

## 构建

安装或确认以下工具：

- Qt 6.4.2 MinGW：`C:/Qt/6.4.2/mingw_64`
- MinGW：`C:/ProgramData/mingw64/mingw64/bin`
- CMake + Ninja

以上是 `CMakePresets.json` 与 `scripts\*.cmd` 使用的默认路径。工具装在别处时，在仓库根目录新建 `CMakeUserPresets.json`（已被 Git 忽略）覆盖 `CMAKE_PREFIX_PATH` 和编译器路径即可。

本地 OCR 需要的 ONNX Runtime 与 PP-OCR 模型不在仓库中，首次构建前运行一次：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_ocr_assets.ps1
```

```bat
scripts\configure-debug.cmd
scripts\build-debug.cmd
scripts\test-debug.cmd
scripts\smoke-debug.cmd
```

### 构建选项

| CMake 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `VISNIP_ONLINE_TRANSLATION` | `OFF` | `ON` 时加入云端、内网和旧版混合翻译模式。发布版保持 `OFF`。 |
| `VISNIP_DEFAULT_SERVICE_URL` | 空 | 联网模式下云端服务的默认根地址；为空时由用户在首选项中填写。 |

例如：`cmake --preset windows-mingw-debug -DVISNIP_ONLINE_TRANSLATION=ON -DVISNIP_DEFAULT_SERVICE_URL=https://translate.example.com`

Release 验证：

```bat
scripts\configure-release.cmd
scripts\build-release.cmd
scripts\test-release.cmd
scripts\smoke-release.cmd
```

生成可直接双击运行的发布目录：

```bat
scripts\package-release.cmd
dist\Visnip\visnip.exe
```

也可以用 `scripts\package-release.ps1 -BuildDir <构建目录> -Version <版本>` 打包。它会生成带许可证文件的发布目录和 zip，并对打包结果运行 `--self-test`；GitHub Actions 发布时用的就是这个脚本。

不要直接复制 `build\windows-mingw-release\visnip.exe` 单文件给别人使用；Qt 程序需要同时携带 Qt6Core、Qt6Gui、Qt6Widgets、Qt6Svg、platforms/qwindows.dll 和 MinGW 运行时等依赖。`package-release.cmd` 会自动关闭正在运行的 Visnip，再调用 `windeployqt` 部署这些文件。

也可以直接运行：

```bat
set PATH=C:\ProgramData\chocolatey\bin;C:\ProgramData\mingw64\mingw64\bin;C:\Qt\6.4.2\mingw_64\bin;%PATH%
cmake --preset windows-mingw-debug
cmake --build --preset debug
ctest --preset debug
```

## 运行

### 翻译处理模式

开源发布版**只提供本机离线翻译**：不包含云端翻译、内网服务器和旧版混合（联网）模式，源码中也没有任何翻译服务地址。旧版本保存的联网模式设置在启动时改为本机离线。

联网模式的代码仍保留，只在自行构建时开启（见下文「构建选项」）。开启后，切换到会上传内容的模式前必须先确认上传说明；云端与内网地址、API 令牌分别保存，令牌以 `Authorization: Bearer` 发送，云端令牌只走 HTTPS（本机回环地址除外），在 Windows 上用 DPAPI 加密保存。

离线资源在首选项中准备：**翻译 → 本机离线 → 下载并启用**。资源直接从官方渠道下载，Visnip 不托管任何资源文件：llama.cpp b10964 来自 GitHub，腾讯 Hy-MT2-1.8B 模型来自魔搭社区（ModelScope），不可用时改用 Hugging Face。下载地址和 SHA-256 固定写在源码里。用户确认后客户端会依次下载、逐个核对哈希、解压，并运行真实模型自检；自检通过后才保存资源路径并启用。下载支持暂停和续传。llama.cpp 的官方构建需要 Microsoft Visual C++ 2015-2022 运行库；缺少时客户端会在下载前提示安装。

本机离线分两档：

- **轻量（默认）**：C++ 本机 OCR → 本地 `llama-server`（Hy-MT2-1.8B Q4_K_M，仅监听 127.0.0.1，每次启动随机令牌）→ 客户端内原位回填。不需要 Python、PyTorch 或显卡，下载约 1.1 GiB。换行的正文按段落翻译再按原行宽拆回各行；按本机实测速度自动选择 1、2 或 4 路并发。截图（F1）时预热模型，空闲 10 分钟释放。设计与实测见 `docs/offline-lite-tier.md`。
- **精细**：Python 整图引擎，额外用 Hi-SAM 分割笔画、LaMa 修复复杂背景，需要约 6 GiB 资源，建议有独立显卡。它的资源没有官方整包，0.4.1 起不再提供下载；以前装好的精细资源仍可使用。

离线推理不上传截图或文字，也不会自动回退在线服务。普通截图、标注和贴图不需要加载模型。两档目前都只开放中英互译。Windows 发布者代码签名仍待配置。详情见 `docs/offline-resource-manager.md`。

实施阶段、模型包设计、待确认事项与验收要求见 `docs/translation-engine-architecture.md`。

```bat
set PATH=C:\ProgramData\mingw64\mingw64\bin;C:\Qt\6.4.2\mingw_64\bin;%PATH%
build\windows-mingw-debug\visnip.exe
```

非交互 smoke test：

```bat
scripts\smoke-debug.cmd
```

## 隐私

- **翻译不上传**：发布版只做本机离线翻译，截图和文字只在本机处理；截图、标注、贴图从不联网。
- **自行开启联网模式时**：切换到云端、内网或混合模式前，客户端先说明会把什么发给谁，同意后才启用；未确认时不发送，也不预先建立连接。百度翻译失败时直接报错，不会改发给其他服务；内网地址为空时也不回退到云端。
- **资源下载**：离线资源只在你确认后，从 GitHub、魔搭社区或 Hugging Face 下载；这些网站会像普通下载一样收到你的 IP 等访问信息。Visnip 的服务器不参与，也不上传截图或文字。
- **密钥**：API 令牌、百度密钥、解题模型 Key 在 Windows 上用 DPAPI 按当前用户加密保存。
- **日志**：程序目录下的 `logs/` 记录耗时、尺寸、状态和服务地址等运行信息，不写入截图中的文字。只有设置环境变量 `VISNIP_SAVE_TRANSLATION_IMAGES=1` 时才会在本机保存翻译诊断图片，用于排查问题。
- **解题面板**：只在你点击「获取答案」时，把选区画面发送给你在首选项中配置的模型接口。

## 许可证

本项目以 [Apache License 2.0](LICENSE) 授权，版权声明见 [NOTICE](NOTICE)。第三方组件、模型和数据的来源与许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，它们不因本仓库的许可而改变。

“Visnip”名称和图标不在 Apache License 的授权范围内。修改后再发布的版本请使用不同的名称和图标。

安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。

## 持续集成与发布

- 每次推送分支或提交 Pull Request，GitHub Actions 都会在 Windows 上用 Qt 6.4.2 和 MinGW 11.2 构建，分别在关闭和开启联网翻译时运行全部测试和 `--self-test`（见 `.github/workflows/ci.yml`）。
- 推送 `v<版本>` 标签时，会先检查标签与 `CMakeLists.txt` 中的版本一致，再构建只含本机离线翻译的版本、测试、打包，并以 `CHANGELOG.md` 中对应的段落作为说明，创建草稿发布。人工验证后再正式发布；官网的下载按钮指向 GitHub 的 `releases/latest`，它不包含草稿和预发布（见 `.github/workflows/release.yml`）。
- 发布新版本时要同时修改 `CMakeLists.txt` 与 `resources/visnip.rc` 中的版本号，并在 `CHANGELOG.md` 里添加该版本的段落。

## 设计资料

完整设计包位于 `docs/`，其中 `docs/docs/Visnip_轻量截图贴图工具_设计说明书_v0.3_compact.md` 是当前开发口径。正式实现优先遵循 `docs/dev/design_tokens.json` 与 `docs/dev/toolbar_spec.json`。
