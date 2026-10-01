# Visnip UI 区域模型

这套工具用于从零训练快速翻译的 UI 区域安全模型。模型不替代 OCR 字符识别，也不把现有启发式结果当成标签；它以 OCR 检测框为锚点，判断区域的视觉类型、布局关系和可安全贴图范围。

## 能力边界

第一阶段模型输出：

- `text_only / mixed / non_text`
- `heading / paragraph / ui_label / caption_metadata / data_code / other`
- `single / split_required / merge_required / none`
- `rect_safe / mask_required / none`
- `eraseBox` 和 `layoutBox`

只有高置信度 `text_only + single + rect_safe` 才允许进入翻译贴图。模型缺失、输出维度错误、NaN、框越界或置信度不足时，运行时必须保留原图。

这个模型能处理 OCR 已提出候选中的图标误识别、图文混合、标题/正文粗分类和贴图边界；它不能恢复 OCR 完全漏掉的文字。人工补充的漏检框会保存在同一数据集，供后续检测器训练和 proposal recall 评估使用。

## 数据格式

默认数据目录是：

```text
%LOCALAPPDATA%\Visnip\Visnip\ui-region-dataset
```

每张原图按原始字节 SHA-256 内容寻址，只保存一次。`ocrProposals.observations` 是 OCR 原始观测的唯一真源，提议框通过 `anchorObservation` 固定到其中一条观测；`annotations` 是独立的人工真值，二者通过 `proposalIds` 关联。关联框必须覆盖两者较小面积的至少 50%，既允许人工收紧或扩展 OCR 框，也拒绝仅有边缘擦碰的脏标签。OCR 的 `score`、`acceptedForTranslation` 和旧分组结果只能作为标注参考，不能直接作为监督标签。

三个标签维度必须分开：

- `textness` 表示视觉上是否为文字。
- `role` 表示标题、正文、菜单项、图标、头像等语义角色。
- `translationPolicy` 表示应该翻译、保留还是复核。例如年份属于文字，但策略通常是保留。

范围定义：

- `textBox`：实际文字或候选内容范围。
- `maskBox`：矩形背景下需要清除的源文字范围，必须包含 `textBox`。
- `layoutBox`：译文允许占用的最大范围。
- `layoutContainer`：按钮、菜单项、面板等上层边界，存在时必须包含上述范围。

图文混合区域标记为 `mixed + split_required`，并添加文字和非文字子区域。纹理、照片或无法用矩形安全恢复的背景标记为 `mask_required`，第一阶段直接保留原图。

需要合并的 OCR 分段可直接关联到同一标注；标注界面中也可为多个 `merge_required` 候选填写相同“上下文组”。严格校验要求每个合并关系最终覆盖至少两个不同 OCR proposal。

## 导出与标注

导出现有快速翻译诊断：

```powershell
scripts\export-ui-region-dataset.cmd
```

启动本地标注工具：

```powershell
scripts\annotate-ui-regions.cmd
```

浏览器访问 `http://127.0.0.1:8765`。服务只监听本机，保存使用 revision 冲突检测和原子替换，避免两个标注页面互相覆盖。

标注完成后执行严格校验：

```powershell
scripts\validate-ui-region-dataset.cmd --require-reviewed
```

同一应用族、同一页面会话和近重复截图必须使用相同 `splitGroup`。这些切分 ID 只使用小写 ASCII 标识符，例如 `github/profile-page`，避免大小写、空格和 Unicode 同形字符绕过隔离。导出器会按 SHA-256 去重，并用感知哈希合并明显近重复截图；人工仍需补充应用族和页面会话。无法取得稳定键时，必须人工核对相关截图都在同一切分组，再勾选“切分隔离已复核”。严格校验会同时要求文字、OCR 锚点和保护区域三项覆盖完整。

## 从零训练

训练环境与程序运行环境分离：

```powershell
python -m venv .venv-ui-region
.\.venv-ui-region\Scripts\python -m pip install -r tools\ui_region_model\requirements-train.txt
.\.venv-ui-region\Scripts\python -m tools.ui_region_model.train --dataset "$env:LOCALAPPDATA\Visnip\Visnip\ui-region-dataset"
```

`UiRegionNet` 始终从随机权重初始化，不下载或加载预训练权重。输出目录包含：

- `best.pt`：训练检查点。
- `candidate_model.onnx`：仅供评估的 ONNX Runtime 候选模型。
- `model_contract.json`：输入、输出、标签顺序、模型哈希和训练环境。
- `metrics.json`：每轮指标及最终验证/测试结果。
- `dataset_manifest.json`：本轮实际读取的样本、标签图片和测试切分内容哈希。
- `preprocess_golden.json`：与契约哈希绑定的预处理一致性证据。

ONNX 契约固定为单输入 `float32 [N,7,96,256]`、单输出 `float32 [N,24]`，并要求输入输出共享动态批量维。分类分量是 logits，由运行时执行 softmax；两个几何框已在模型内归一化到 `[0,1]`，运行时不得再次执行 sigmoid。详细偏移见 `model_contract.template.json`。

训练在数据集锁内建立内容哈希清单并把图片读入不可变内存快照，清单同时绑定每个样本和每个 split 的实际训练单元数，后续文件变更不会混入本轮训练。随机种子和确定性算法是强制约束；运行环境若无法提供确定性算子会直接报错。导出后会用最终 ONNX Runtime 模型重新跑完整 test split；发布统计不沿用导出前的 PyTorch 指标。训练完成不会直接产生可部署模型，`model_contract.json` 和 `metrics.json` 始终将其标为 `research_candidate / deploymentEligible=false`。

## 发布门槛

当前脚本把以下条件作为最低自动门槛：

- `non_text`、`mixed`、`unsafe_text` 每类至少 598 个独立切分组且测试集均为 0 个误接受，才能分别以 95% 单侧置信度支持错误率低于约 0.5%。
- 可安全文字至少覆盖 598 个独立切分组且无漏接受，组级召回率的 95% 单侧下界不低于 99.5%。
- 四个分类头的每个类别至少有 25 个单元、10 个独立切分组，且 role macro-F1 不低于 0.90。
- 几何真值至少覆盖 200 个独立切分组；清除框和排版框都同时检查覆盖不足与向外溢出，并要求清除框不越出排版框。

这些是必要条件，不是充分条件。候选审计会从各项计数、每类支持量和置信区间重新计算门槛，同时复核数据清单、预处理 golden、模型、契约和指标之间的内容哈希，不接受单独自报的“通过”布尔值：

```powershell
scripts\qualify-ui-region-model.cmd --run <训练输出目录>
```

审计通过只生成 `candidate_audit.json`，其中仍明确写入 `deploymentEligible=false`。当前项目尚未把模型接入 C++ 快速翻译运行时，也没有由仓库控制、实际执行最终贴图代码的像素测试程序，因此工具不会生成 `deployment_manifest.json`，候选权重也不会进入 `dist\Visnip`。

正式接入时必须新增仓库自有的端到端 harness，由资格工具亲自执行 test split 的全部真实 proposal，并比较原图、保护区和允许贴图范围内外的像素。外部 JSON 报告或自报 harness 哈希不能取得部署权限。

现有约 44 张诊断截图和一千余个 OCR 候选只适合建立流程与首轮主动学习，不足以发布通用模型。建议先覆盖至少 20 个应用或站点族，并持续加入模型高置信误判的 hard negatives；是否足够最终由独立测试集指标决定，而不是仅按样本数量决定。
