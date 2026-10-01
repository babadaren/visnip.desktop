# Visnip Design Package v0.3 Compact

> 设计包完成于产品改名之前，设计图中显示的旧名称 Vislate 即现在的 Visnip。

本包是 Visnip 轻量截图贴图工具的完整设计交付物，针对反馈“工具栏高度太高，再小一些，小巧清新一些”重新整理。

## 本版核心变更

- 工具栏默认高度：30 px。
- 单按钮尺寸：22 × 22 px。
- 图标视觉尺寸：15 px。
- 工具栏圆角：9 px，按钮圆角：6 px。
- 分隔线：1 × 14 px。
- 主色更新为 #4F7CFF，辅色为 #45D6B4。
- 主工具栏仍保留 12 个高频按钮，但通过更小按钮、缩短阴影、轻边框降低存在感。

## 目录

- `docs/`：完整设计说明（Markdown）。
- `assets/app/`：Visnip 应用图标 SVG、PNG、ICO。
- `assets/icons/`：SVG 源文件和多尺寸 PNG 图标。
- `assets/buttons/`：22px 工具栏按钮状态切图。
- `assets/mockups/`：设计板、截图覆盖层、设置窗口、菜单、工具栏、放大镜等设计图。
- `dev/`：设计令牌、QSS、qrc、工具栏 JSON 规格。

## 使用建议

优先以 `dev/design_tokens.json` 和 `dev/toolbar_spec.json` 为开发口径。PNG 切图可用于早期原型；正式 Qt 工程建议加载 SVG 源文件并通过当前主题色渲染。
