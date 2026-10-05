# Changelog

Visnip was called Vislate before 0.4.0. Earlier versions were developed privately; the public history starts with the first open-source release.

## Unreleased

### Question panel: copy only the answer

- "复制答案" copies just the final answer: the part after the response's last "答案：" line (also "最终答案", "Answer:"), without the reasoning, Markdown marks or LaTeX. For several questions it copies the numbered list of answers.
- The default prompt asks for that answer block explicitly: one "答案：" line for a single question, or "答案：" followed by one line per question, and nothing after it.
- When a response has no answer line (for example with a custom prompt), the button reads "复制全部" and copies everything, so its label always matches what it copies.
- The analysis can be copied by selecting it: Ctrl+C, or a Chinese right-click menu with "复制所选内容", "复制答案", "复制全部" and "全选" (Qt's own menu was English). The button briefly shows "已复制答案" / "已复制全部".

## 0.4.4

- The translate button in the toolbar has a new icon ("文 A"), drawn in the same line style as the other toolbar icons.
- A shorter README, plus contributor documentation: `CONTRIBUTING.md`, issue forms, a pull request checklist and private security reporting.
- The documentation describes the current translation design; working notes of earlier versions moved to `docs/history/`.

## 0.4.3

### In-app updates

- Opening the preferences asks GitHub for the latest published release. When it is newer, "关于" in the sidebar reads "关于 · 有新版本", and 关于 → 更新 shows the version, its release notes and "立即更新到 x.y.z".
- The update downloads `Visnip-<version>-windows-x64.zip` from this repository's GitHub release and checks it against GitHub's asset digest and the published `.sha256` file. The download is resumable.
- Before anything is replaced, the new copy is unpacked into `%LOCALAPPDATA%\Visnip\updates` and must pass `--self-test`. It then waits for the running Visnip to exit, replaces the installation, and starts it again.
- Only the files of the package list (`package-files.txt`, new in every release package) are replaced or removed. Settings, offline resources, screenshots, `logs/` and any other files stay.
- Replaced files are moved to a backup folder first. If any step fails, they are restored and the old version starts. An update interrupted half-way is rolled back the next time.
- Installations in folders without write access (for example Program Files), and builds not run from a release package, show a link to the release instead of updating themselves.
- The automatic check can be turned off (关于 → 自动检查更新). Development builds only check when "检查更新" is pressed. Forks point `VISNIP_UPDATE_REPOSITORY` at their own repository, or leave it empty to build without update checks.
- Versions up to 0.4.2 have no updater and need one manual download of 0.4.3.

## 0.4.2

### Offline resource management

- Preferences now list the installed resources: each file's name, size and whether it is present，plus "打开文件目录"（open the folder）and "删除已下载资源"（delete the download）. Deleting stops a resident translation engine first - the reason deletion used to fail with "文件可能正在使用" - removes the files, the enable receipt and the cached archives, and keeps nothing enabled until a fresh download.
- "导入已有离线资源"（import an existing folder）is gone. The resources are app-managed and always come from the publishers, so a hand-picked directory had nothing to add.
- The download folder is selectable on the offline page（下载位置 → 选择文件夹… / 恢复默认）. It no longer has to live on the system drive; the free-space check follows the chosen volume, and installed resources keep working when the location changes.
- A download that stalls is recovered without the user pausing it: the transfer is sampled every 20 seconds, fewer than 256 KiB in a window hands the file to the retry or fallback publisher, and the status line reports the rate of the last window. Bytes that arrive before the status line is parsed are buffered instead of blocking the read loop.
- The download progress bar keeps its byte counter inside a 22 px bar instead of clipping it against a 6 px indicator, and the compact OCR indicator no longer draws a percentage it cannot show.

## 0.4.1

### Offline resources come from their publishers

- The lite tier downloads the official files directly: the llama.cpp b10964 Windows CPU build from GitHub, and Tencent's Hy-MT2-1.8B model from ModelScope, with Hugging Face as the fallback. Visnip no longer hosts or relays any resource file, and the client no longer contacts `vislate.ipxair.com`.
- Download addresses and SHA-256 are pinned in `src/core/OfflineResourceCatalog.cpp`. Every file is checked before use. Redirects are followed only to the publishers' own CDN hosts.
- Interrupted downloads resume from the downloaded size. A source that cannot be reached is skipped for the next one without losing progress.
- The download is about 1.1 GiB (previously 1.2 GiB). The model is written straight to where it runs instead of being stored twice.
- The official llama.cpp build needs the Microsoft Visual C++ 2015-2022 runtime. When it is missing, the preferences say so before anything is downloaded and link to Microsoft's redistributable.
- The precise tier has no official upstream package and can no longer be downloaded. Precise resources installed by an earlier version keep working and stay selectable.

## 0.4.0

First open-source release under the Apache License 2.0.

### Renamed from Vislate to Visnip

- The app, its executable (`visnip.exe`), window titles, logo wordmark and download package are now called Visnip. The V logo is unchanged.
- On the first start, Visnip copies the settings of an earlier Vislate installation, including hotkeys, saved keys and the offline resource folder. Installed offline resources keep working from where they are and are not downloaded again. The Vislate settings themselves are left untouched.
- The Vislate auto-start entry is replaced by the Visnip one, and downloaded OCR language packs are moved over.
- Quit Vislate before starting Visnip for the first time; both use the same global hotkeys.
- The Word and PDF copies of the v0.3 design spec were removed; the Markdown spec in `docs/docs/` is the source.

### Offline-only build

- The published build offers local offline translation only. Cloud, intranet and hybrid translation are not compiled in (CMake option `VISNIP_ONLINE_TRANSLATION`, default `OFF`), and the source code contains no translation service address.
- Settings stored by earlier versions with an online mode load as offline. Target languages other than Chinese and English fall back to Simplified Chinese.
- The translation preferences show no mode selector and no service settings. The translation services refuse to send anything and open no connections.

### Offline translation: new lite tier (default)

- Local C++ OCR → local `llama-server` running Hy-MT2-1.8B (Q4_K_M) → in-place refill. No Python, PyTorch or GPU needed; only the base resource package (about 1.2 GiB) is downloaded.
- The server listens on `127.0.0.1` only, uses a fresh random API key per start and is stopped with the client. It is prewarmed at start-up and on F1, and released after 10 idle minutes or when memory runs low.
- Prompts, glossary, number and placeholder validation, retry and kept-name rules match the precise tier.
- Wrapped prose is translated as one paragraph and split back over the original lines.
- Prefill and decode speed are measured once per start, and each job picks 1, 2 or 4 parallel requests from them. Slow CPUs run sequentially to reuse the cached context; GPUs run in parallel.
- An optional verified `engine-runtime` with Vulkan or CUDA is used when present; on any failure the client falls back to the CPU build.
- Preferences → Translation → Offline now offers **Lite** (default) and **Precise** (Python whole-image engine with Hi-SAM and LaMa).

### Privacy

- Translation never uploads screenshots or text in the published build.
- The Baidu secret key is now encrypted with DPAPI like the other secrets.
- Logs record the length of recognized text, never the text itself.

### Online modes (source builds with `VISNIP_ONLINE_TRANSLATION=ON` only)

- No service address is built in; set one with `VISNIP_DEFAULT_SERVICE_URL` or enter it in the preferences.
- Before switching to a mode that uploads anything, the preferences show which content goes to which receiver. The mode is enabled only after the user accepts. Without acceptance, nothing is sent and no connection is pre-opened.
- A failed or unconfigured Baidu request in hybrid mode is reported and is no longer re-sent to another service.
- Separate API tokens for the cloud and the intranet receiver, sent as `Authorization: Bearer` to `/v1/image-translate` and to the service's own `/v1/translate`. Neither token is ever sent to the other receiver or to Baidu. Cloud tokens are sent over HTTPS only (loopback excepted). HTTP 401 and 403 responses point to the token setting.

### Project

- Apache-2.0 `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, `SECURITY.md` and redistributed runtime license texts in `third_party/licenses`.
- GitHub Actions: Windows build and tests for every push and pull request, with online translation both off and on. Pushing a `v*` tag builds the offline-only Windows package and creates a draft release.

### Known limitations

- The Windows binaries are not code-signed yet, so Windows SmartScreen may warn on first start.
- Offline translation supports Chinese ↔ English only.
- Two compositor tests fail when real system fonts are available (`fragmentedSingleLineBoxesRecoverFullSentence`, `shortLabelDoesNotExpandAcrossNearbyArtwork`). The cause, either the test samples or the algorithm, has not been determined yet.
