# 参与贡献

*English: issues and pull requests in English are welcome too. The essentials are below — build on Windows with Qt 6.4.2 MinGW, keep every network access behind an explicit user action, and run the tests.*

感谢你愿意改进 Visnip！提交问题或代码之前，请先看看下面的约定。

## 先开 Issue

- **Bug 和功能建议**：请用仓库的 Issue 模板提交。
- **较大的改动**：比如新功能、改动截图流程或翻译链路，请先开 Issue 讨论方向，避免白做。
- **安全问题**：不要公开提 Issue，按 [SECURITY.md](SECURITY.md) 私下报告。

## 开发环境

- Windows 10 / 11 x64
- Qt 6.4.2（MinGW 64-bit）和对应的 MinGW 11.2
- CMake 3.25 以上、Ninja

`CMakePresets.json` 默认 Qt 在 `C:/Qt/6.4.2/mingw_64`，MinGW 在 `C:/ProgramData/mingw64/mingw64/bin`。装在别处时，在仓库根目录新建 `CMakeUserPresets.json`（已被 Git 忽略）覆盖这两个路径。

第一次构建前，先下载本地 OCR 需要的 ONNX Runtime 和模型（都核对 SHA-256）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_ocr_assets.ps1
```

## 构建与测试

```bat
scripts\configure-debug.cmd
scripts\build-debug.cmd
scripts\test-debug.cmd
scripts\smoke-debug.cmd
```

- 测试在无界面环境下运行（`QT_QPA_PLATFORM=offscreen`）。
- 需要真实模型或真实网络的验收测试默认跳过，要用对应的环境变量显式开启（见各测试文件和 `docs/`）。
- 修复问题时请尽量补一个能复现它的测试。

常用构建选项：

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `VISNIP_ONLINE_TRANSLATION` | `OFF` | `ON` 时加入云端、内网和混合翻译模式；发布版保持 `OFF` |
| `VISNIP_DEFAULT_SERVICE_URL` | 空 | 联网模式下云端服务的默认地址 |
| `VISNIP_UPDATE_REPOSITORY` | `babadaren/Visnip.desktop` | 检查更新的 GitHub 仓库；fork 改成自己的，留空则不检查 |

每次推送和 Pull Request 都会在 GitHub Actions 上构建并运行全部测试，`VISNIP_ONLINE_TRANSLATION` 为 `OFF` 和 `ON` 各一套。合并前 CI 必须全部通过。

### 本地打包与排查

- **打包 Release 版**：
  - 运行 `scripts\package-release.cmd`，会生成可以直接运行的 `dist\Visnip\visnip.exe`，并用 `windeployqt` 带上 Qt 和 MinGW 运行库。不要只复制单个 `visnip.exe` 给别人。
  - 也可以用 `scripts\package-release.ps1 -BuildDir <构建目录> -Version <版本>`。它和 GitHub Actions 发布用的是同一个脚本，会生成带许可证和 `package-files.txt` 的 zip，并对打包结果运行 `--self-test`。
- **排查翻译回填问题**：设置环境变量 `VISNIP_SAVE_TRANSLATION_IMAGES=1` 后，会在本机保存翻译诊断图片。默认不保存，诊断图片也不会上传。

## 必须遵守的原则

- **不悄悄联网**：截图、标注和贴图从不联网。新增的任何网络访问都必须由用户明确的操作触发，并在 README 的「隐私」一节写清楚发给谁、发了什么。离线翻译失败时不能自动改用在线服务。
- **源码里没有私密内容**：不提交翻译服务地址、密钥、令牌、个人信息或本机路径。
- **第三方文件来自官方渠道**：资源从上游官方渠道获取，固定版本、地址和 SHA-256。新增依赖、模型或数据时，同步更新 `THIRD_PARTY_NOTICES.md`，并确认许可证允许按本项目的方式使用。
- **不要手改 `resources/offline/*.py`**：它们是精细档引擎的同步副本，见该目录的 README。
- **保持更新协议兼容**：
  - 旧版本会用 `visnip.exe --apply-update --target <目录> --wait-pid <进程号>` 启动新版本来完成更新，这组参数不能改。
  - 发布包里的 `package-files.txt` 由 `scripts/package-release.ps1` 生成，不要手写。

## 代码风格

- C++20，只用 Qt 6.4 已有的 API。
- 跟随所在文件的命名、注释密度和写法。
- 界面文案用简体中文。
- 一个提交只做一件事。提交信息用英文写清楚改了什么、为什么改；风格参考 `git log` 里已有的提交。

## 发布（维护者）

1. 同时修改 `CMakeLists.txt` 和 `resources/visnip.rc` 里的版本号，并在 `CHANGELOG.md` 里加上该版本的段落。
2. 推送 `v<版本>` 标签。GitHub Actions 会检查标签与版本是否一致，然后构建、测试、打包，直接发布为最新版本。
3. 客户端的更新检查和官网的下载按钮都会读取这个最新版本。

## 许可

提交到本仓库的贡献，都按 [Apache License 2.0](LICENSE) 授权，和项目本身相同。请不要提交无法按 Apache-2.0 发布的代码、图片或数据。

"Visnip"名称和图标不在 Apache License 的授权范围内，详见 [NOTICE](NOTICE)。
