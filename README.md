<p align="center">
  <img src="docs/assets/app/visnip-app-icon-white.svg" alt="Visnip" width="88" height="88">
</p>

<h1 align="center">Visnip</h1>

<p align="center">开源免费的 Windows 截图工具：截图、标注、贴图，一条工具栏搞定。</p>

<p align="center">
  <a href="https://visnip.com">官网</a> ·
  <a href="https://github.com/babadaren/Visnip.desktop/releases/latest">下载</a> ·
  <a href="CHANGELOG.md">更新日志</a>
</p>

## 功能

- **截图**：按 `F1` 框选，支持多屏和高 DPI。尺寸标签、放大镜和 RGB / HEX 取色帮你对准像素，`Ctrl+F1` 重复上次区域。
- **标注**：矩形、箭头、画笔、文字、序号、马赛克与模糊、橡皮擦，可以撤销和重做，画完直接复制或保存。
- **长截图**：在选区里滚动页面，内容实时拼成一张长图；滚过头往回滚，还能向上补拼。
- **贴图**：截图一键置顶到桌面，可拖动、滚轮缩放、`Ctrl+滚轮` 调透明度、鼠标穿透；`F3` 直接贴出剪贴板里的图片。
- **原位翻译**：在本机识别并翻译选区里的文字，译文回到原来的位置，再点一次切回原图。全程离线，不需要显卡，目前支持中英互译。
- **AI 解题**：截下题目或报错，发给你自己配置的多模态模型（OpenAI 兼容或 Anthropic 格式），答案在屏幕侧边逐字显示。
- **一键更新**：有新版本时，在「首选项 → 关于」中点「立即更新」即可。

## 下载与使用

支持 Windows 10 / 11 64 位。在 [Releases](https://github.com/babadaren/Visnip.desktop/releases/latest) 下载 `Visnip-<版本>-windows-x64.zip`，解压后运行 `visnip.exe`，不需要安装，程序会常驻在托盘。可以用同名的 `.sha256` 文件核对下载是否完整。

- **SmartScreen 提示**：安装包还没有代码签名，Windows 可能拦截。确认文件来自本仓库的 Releases 后，点「更多信息 → 仍要运行」。
- **离线翻译资源**：首次使用翻译时，在「首选项 → 翻译」中下载约 1.1 GB 官方资源。llama.cpp 来自 GitHub，腾讯 Hy-MT2 翻译模型来自魔搭社区（不可用时改用 Hugging Face），下载后逐个核对 SHA-256。翻译引擎需要 Microsoft Visual C++ 运行库，缺少时客户端会提示安装。
- **更新**：设置、离线资源和截图都会保留。如果程序放在没有写入权限的位置（例如 Program Files），需要手动下载新版本替换。

默认快捷键（都可以在首选项中修改）：

| 快捷键 | 作用 |
| --- | --- |
| `F1` | 截图 |
| `Ctrl+F1` | 重复上次区域 |
| `F3` | 把剪贴板图片贴到桌面 |
| `Shift+F3` | 隐藏 / 显示所有贴图 |
| `Ctrl+T` | 当前贴图鼠标穿透 |
| `F4` | 解题面板打开时获取答案 |

## 隐私

- **截图、标注、贴图从不联网。**
- **翻译只在本机完成**：截图和文字都不上传，翻译失败也不会改用在线服务。
- **只有下面三种情况会联网**，都由你触发，或者可以关闭：
  - **下载离线资源**：你确认后，从 GitHub、魔搭社区或 Hugging Face 下载。
  - **AI 解题**：点击「获取答案」时，把选区画面发给你在首选项中配置的模型接口，没有中转服务器。
  - **检查更新**：打开首选项时向 GitHub 查询最新版本，不附带个人数据，可以在「关于」中关闭；点击「立即更新」才会下载。
- **不收集使用数据**，也不需要注册登录。
- **密钥加密保存**：解题模型的 API Key 在 Windows 上按当前用户加密（DPAPI）保存。
- **日志不含截图文字**：程序目录下的 `logs/` 只记录耗时、尺寸和状态，不写入截图里的文字。

## 从源码构建

技术栈是 C++20、Qt 6.4 Widgets 和 CMake。需要 Windows、Qt 6.4.2（MinGW 64-bit）、MinGW 11.2、CMake 3.25 以上和 Ninja。

```bat
powershell -ExecutionPolicy Bypass -File scripts\fetch_ocr_assets.ps1
scripts\configure-debug.cmd
scripts\build-debug.cmd
scripts\test-debug.cmd
```

工具路径、构建选项、打包方法和必须遵守的原则见 [CONTRIBUTING.md](CONTRIBUTING.md)。发布版只编译本机离线翻译；联网翻译模式需要在自行构建时开启，说明见 [docs/translation-engine-architecture.md](docs/translation-engine-architecture.md)。

## 文档

- [离线翻译（轻量档）的设计与实测](docs/offline-lite-tier.md)
- [离线资源的下载与校验](docs/offline-resource-manager.md)
- [截图翻译的处理方式与数据边界](docs/translation-engine-architecture.md)
- [解题面板设计](docs/question-panel-design.md)
- [界面设计规范](docs/README.md)

## 参与贡献

欢迎提交问题和改进，开始前请先读 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。

## 许可证

本项目以 [Apache License 2.0](LICENSE) 授权，版权声明见 [NOTICE](NOTICE)。第三方组件、模型和数据的来源与许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

"Visnip"名称和图标不在 Apache License 的授权范围内，修改后再发布的版本请使用不同的名称和图标。
