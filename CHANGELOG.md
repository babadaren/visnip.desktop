# Changelog

Visnip was called Vislate before 0.4.0. Earlier versions were developed privately; the public history starts with the first open-source release.

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
