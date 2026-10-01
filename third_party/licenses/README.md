# Licenses of redistributed runtime components

The Windows package redistributes these components unchanged. Their license texts are copied here so that the package can include them.

- `lgpl-3.0.txt` and `gpl-3.0.txt`: Qt 6 open-source edition (LGPL-3.0, which incorporates the GPL-3.0 text). The package links Qt dynamically, so the Qt DLLs can be replaced. Qt source code is available at https://download.qt.io/archive/qt/6.4/.
- `gcc-runtime-library-exception-3.1.txt`: the GCC Runtime Library Exception that covers `libgcc_s_seh-1.dll` and `libstdc++-6.dll` from the MinGW-w64 GCC toolchain.
- `winpthreads-COPYING.txt`: the mingw-w64 winpthreads license that covers `libwinpthread-1.dll`.

The ONNX Runtime MIT license is in `third_party/onnxruntime/LICENSE`. The full component list is in `THIRD_PARTY_NOTICES.md`.
