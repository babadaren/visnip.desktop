# Third-party notices

Visnip Desktop is licensed under the Apache License 2.0 (`LICENSE`). The components below keep their own licenses. Nothing in this list is relicensed by this repository.

## Shipped with the desktop client

| Component | Version / source | License | How it is used |
| --- | --- | --- | --- |
| Qt 6 (Core, Gui, Widgets, Svg, Network, Concurrent, Test) | 6.4.x, https://www.qt.io | LGPL-3.0 (open-source edition; `third_party/licenses/lgpl-3.0.txt`, `gpl-3.0.txt`) | Dynamically linked DLLs deployed beside `visnip.exe`. You may replace them with your own build of the same Qt version. Qt source code: https://download.qt.io/archive/qt/6.4/ |
| MinGW-w64 GCC runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`) | GCC toolchain used for the build | GPL-3.0 with the GCC Runtime Library Exception (`third_party/licenses/gcc-runtime-library-exception-3.1.txt`) | Runtime libraries of the compiler |
| MinGW-w64 winpthreads (`libwinpthread-1.dll`) | mingw-w64 | MIT-style license (`third_party/licenses/winpthreads-COPYING.txt`) | Threading runtime of the compiler |
| ONNX Runtime | 1.22.0, https://github.com/microsoft/onnxruntime | MIT (`third_party/onnxruntime/LICENSE`) | Inference runtime for local OCR; headers in `third_party/onnxruntime`, DLL fetched by `scripts/fetch_ocr_assets.ps1` |
| PP-OCRv4 text detection model (ONNX) | `SWHL/RapidOCR` on Hugging Face (conversion of PaddleOCR) | Apache-2.0 | Local OCR text detection |
| PP-OCRv5 mobile recognition models (general, Korean, Latin, Cyrillic, Arabic) | `PaddlePaddle/*_PP-OCRv5_mobile_rec_onnx` on Hugging Face | Apache-2.0 | Local OCR text recognition |
| PaddleOCR character dictionaries (`ppocr_keys_v1.txt`, `ppocrv5_*_dict.txt`) | https://github.com/PaddlePaddle/PaddleOCR | Apache-2.0 | OCR output alphabets in `third_party/ocr` |

The client renders text with fonts that are already installed on the user's system. It does not ship font files.

The Windows release package contains these license texts in its `licenses/` folder.

## Downloaded separately: offline translation resources

The offline packages are downloaded only when the user explicitly starts the download in the preferences. They are not part of this repository.

### Lite tier (base package)

| Component | Version / source | License |
| --- | --- | --- |
| llama.cpp (`llama-server`) | release b10964, https://github.com/ggml-org/llama.cpp | MIT |
| Hy-MT2-1.8B-GGUF (`Hy-MT2-1.8B-Q4_K_M.gguf`) | revision `a0c709d9fac510f2c807aa3af52872340dc37a4a`, https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF | Apache-2.0, Copyright (C) 2026 Tencent |

### Precise tier (adds the following)

| Component | Source | License |
| --- | --- | --- |
| Hi-SAM (code and `hi_sam_b.pth`) | https://github.com/ymy-k/Hi-SAM. The official weight link was unavailable, so the weights come from the `GoGiants1/Hi-SAM` mirror on Hugging Face and are checked against its published SHA-256. | Apache-2.0 |
| Segment Anything ViT-B (`sam_vit_b_01ec64.pth`) | https://github.com/facebookresearch/segment-anything | Apache-2.0 |
| LaMa inpainting (`big-lama.pt`, TorchScript export) | https://github.com/advimman/lama, export from https://github.com/Sanster/models | Apache-2.0 |
| Python runtime | https://www.python.org | PSF License 2.0 |
| PyTorch / torchvision (CPU builds) | https://pytorch.org | BSD-3-Clause |
| Other Python packages (for example NumPy, OpenCV, Pillow, fontTools, httpx) | PyPI | Their respective licenses. A complete per-package license list for the precise resource package still has to be generated before it is redistributed. |

## Documentation and design assets

The icons, mockups and design documents under `docs/` were made for Visnip and are covered by the Apache License 2.0, except where a file states otherwise.
