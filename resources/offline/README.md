# 精细档引擎的客户端副本

这里的 Python 文件是精细档推理引擎（`vislate_engine`）的同步副本，由服务端仓库的 `scripts/sync_desktop_offline_adapter.py` 写入。它们以 Qt 资源的形式嵌进客户端，在启用已安装的精细资源时，写入资源目录的 `vislate_engine/` 中。

- 不要在本仓库里手动修改这些文件，改动会在下次同步时被覆盖。
- 它们依赖 `vislate_server` 等包，这些包只在精细资源包自带的 Python 环境里，本仓库不包含，所以单独无法运行。
- 0.4.1 起精细档不再提供下载，这些文件只为已经装过精细资源的用户保留。默认的轻量档不使用 Python，见 `docs/offline-lite-tier.md`。
