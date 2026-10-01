# 本机离线质量与评测改进（0.2.15 / 0.2.16）

本轮继续按“先量化、再修可见失败”的顺序推进，未更换模型，也未改动协议。

## 改动

1. 统一合成路径接入源字体识别与跨语种字号换算（`vislate_engine/unified_composition.py`）。
   先按实际字形识别源字体（复用 `appearance.estimate_source`），再用参考字（CJK 取目标文本中的汉字，其余取 `H`）把源字号换算成目标字体字号，避免直接把拉丁像素尺寸套到中文字体上。识别到的字体家族通过 `appearance.target_family` 决定候选顺序，其余候选按配置顺序回退。
2. 失败降级链：译文放不下时先缩字号（下限为源字号的 70% 且不低于可读下限 9/10 px），再在框内避让图元（空余 ≤40 px 时穷举全部位置，否则取最近 12 个），再缩字号避让，最后才保留原文。撞击图元的区域不再一律 `artwork_collision`。
3. 逐区域报告新增可评测字段：`source_font_family`、`source_ink_height`、`target_ink_height`、`fit_mode`（`source_size`/`shrunk`/`tight_leading`/`collision_shrunk`）、`shrink_ratio`、`style_fit_score`。
4. 新增离线评测工具 `vislate-server/scripts/score_translation_quality.py`：输入引擎结果 JSON（可选原图与期望译文），输出区域 applied/preserved、原因码分布、字号偏差、缩小策略、框外改动像素、受保护区域改动、分阶段耗时；`--batch` 目录模式给出 p50/p95。

## 自动化验证

| 验证 | 结果 |
| --- | --- |
| 引擎 Python 测试（`vislate-server`） | 297 通过 / 7 跳过（含新增 `test_fit_degradation.py` 5 项、`test_quality_score.py` 3 项） |
| 桌面 CTest（Release，offscreen） | 5/5 目标通过 |
| 真实字体环境（`QT_QPA_FONTDIR=C:\Windows\Fonts`） | 87 通过 / 2 失败：`fragmentedSingleLineBoxesRecoverFullSentence`、`shortLabelDoesNotExpandAcrossNearbyArtwork`，与 `translation-engine-architecture.md` 记录的既有失败完全一致，无新增回归 |
| 真实 GPU 端到端（RTX 3050 Ti 4 GB，CUDA + Vulkan，precise，两张真实回归图译英） | 28 区域，10 应用（35.7%），13 区域本就等价，1 `layout_unfit`、2 `artwork_collision`、2 `no_confident_text_mask`；框外改动像素 0；受保护区域改动 0 |
| 单次耗时（不含模型加载，同批） | p50 5314 ms，p95 5636 ms；分解 p50：OCR 1044、翻译 1620、分割 1164、背景修复 1148（含合成 1469） |
| 降级链在真实截图上生效 | `source_size` 4、`shrunk` 4、`tight_leading` 1、`collision_shrunk` 1（即 6 个区域靠降级链才被回填） |
| 字号一致性 | 源/目标墨迹高度偏差 p95 = 41.7%（同一文本换语种后的重排版差异，逐区域原始值见评分报告） |
| 打包产物 | `dist/Visnip`，版本 0.2.15，`visnip.exe --self-test` 通过 |

真实 GPU 运行的逐区域数据、事件日志与评分报告保存在 `dist/eval/live/` 与 `dist/eval/final/`（`score-batch.json`、`engine` 覆盖层、`session/*.json`）。

注意：这两张是 0.2.14 记录里最重的回归图，**单次总耗时中位数仍高于 5 秒目标**。下一轮优化应优先压 translation（1.6 s）与 segmentation/repair（1.2 s + 1.1 s），而不是继续调整排版启发式。

## 复现

```powershell
# 引擎测试（在服务端仓库的 vislate-server 目录）
.\.venv\Scripts\python -m pytest -q
# 引擎同步到桌面内嵌资源（在服务端仓库根目录）
.\vislate-server\.venv\Scripts\python.exe scripts\sync_desktop_offline_adapter.py --desktop <visnip-desktop 仓库路径>
# 构建 + 测试 + 打包（在本仓库根目录）
scripts\package-release.cmd
# 评分（单个结果或目录批量，脚本在服务端仓库的 vislate-server\scripts）
.\vislate-server\.venv\Scripts\python.exe .\vislate-server\scripts\score_translation_quality.py --result out.json --source input.png
.\vislate-server\.venv\Scripts\python.exe .\vislate-server\scripts\score_translation_quality.py --batch results
```

会话工作进程会删除任务输入图片，批量评分时请自行保留原图，或接受“框外像素”指标为 `null`。

## 0.2.16：数字格式误判导致翻译区域被丢弃

### 现象

用户 0.2.15 日志（941×437、704×291 两张截图）出现区域内容失败：

- `u6`：`sentence_not_translated`（模型原样返回，重试一次仍失败）
- `u1`/`u6`：`protected_token_mismatch`，统计为 missing 2 / added 4、missing 1 / added 1，区域保留原文

### 根因

保护性数字校验按**字面**比较：源文本 `1,200` 被模型写成 `1200` 时，`1,200` 视为丢失、`1200` 视为新增，判定失败并丢弃该区域。合成用例已复现：源 “... exports 1,200 rows per batch ...” + 模型输出 “... 1200 行” → 首次与重试均 `protected_token_mismatch`。

### 修复

`vislate_engine/layout.py` 新增 `canonical_token` / `protected_token_counts`：数字按**值**比较（去千位分隔符、全角数字转半角），占位符仍按字面比较；数字值真的改变仍然 fail-closed。翻译器 `validate_translations`、合成器 `unified_composition.validate_values` 与失败统计 `native_translation._record_validation_failure` 统一使用这一份实现，移除第二处正则。

### 验证

| 验证 | 结果 |
| --- | --- |
| Python 测试 | 307 通过 / 7 跳过（新增 `test_numeric_formatting.py` 10 项：分隔符与全角等价、数值变化仍失败、合成器一致、失败统计不计格式差异） |
| 真实 GPU 复跑同一张失败图 | 由 `protected_token_mismatch` 变为 `applied`，译文保留 `1200`，翻译耗时 1007 ms |
| 同批另外两张长段落 | 翻译 1704 ms / 1127 ms，全部 applied，无校验失败 |
| 桌面 CTest（Release） | 5/5 通过 |
| 打包 | `dist/Visnip` 0.2.16，`visnip.exe --self-test` OK |

### 未解释的耗时波动（待观察）

同一批任务在更早一次会话中出现过 8.2 s / 13.2 s 的翻译耗时（约 190 ms/token），随后不可复现（本轮 1.0–1.7 s）。当次 GPU 显存余量约 1 GB，怀疑与 4 GiB 显卡上的显存/分页状态有关。后续若再出现，请对照 `translation_stats.generation_ms` 与 `vision_handoff.device_free_mib`，并关闭其它占用 GPU 的程序（含浏览器硬件加速、第二个 Visnip 实例）后复测。

## 尚未完成（下一轮）

- `layout.py::compose` 与 `unified_composition.py::compose` 仍是两份实现，前者只剩测试在用；需要合并为一份再删除，避免第二事实源。
- 术语表仍硬编码在 `translation_policy.py`，尚未外置为可更新资源包。
- 桌面内嵌引擎与 `vislate_server/vislate_engine` 仍靠同步脚本维护，未改为单一来源。
- 模型权重与训练/验证素材的再分发许可仍未逐项核对（发布计划 P0-11/P0-12）。
- 长段落只有一次重试；数字修复后仍可能出现真实丢数，下一轮考虑「按缺失 token 定向重试」或把超长段落拆句翻译。
