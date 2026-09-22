# ESP-VoCAt LVGL 页面化 UI 与 iPhone 式交互 Design Spec

> 状态：Approved（2026-09-22，经 brainstorm 分节确认）
> 配套：本 spec 由 `2026-09-22-esp-vocat-lvgl-ui-design` 实施计划落地。

## 目标

把 ESP-VoCAt 的交互与显示从「emote-gfx 拼装的 gfx 标签 + 手势迷宫」重构为「真 LVGL 页面层 + iPhone 式扁平导航 + Siri 式对话覆盖层」，视觉走可爱多彩 + iPhone 精致质感。修复两个已知交互 bug。

## 非目标（Scope）

- 不改通信协议（WebSocket/MQTT）、音频管线核心、两阶省电逻辑的架构。
- 不替换表情宠物脸的动画引擎本身（emote gfx 仍负责宠物表情/动画）。
- 不为其它板子引入变更；只动 esp-vocat 相关文件 + 显示层抽象。

## 背景与现状（已核实）

- 面板：ST77916，360×360 圆屏，QSPI（`esp_lcd_new_panel_st77916`，`esp_vocat.cc:1813`）。
- esp-vocat 当前 `display_ = new emote::EmoteDisplay(panel, panel_io, 360, 360)`（`esp_vocat.cc:1822`），运行在 esp_emote_gfx 引擎上。
- 项目已有全量 LVGL 先例：`lvgl__lvgl` + `espressif__esp_lvgl_port` 在 managed_components；`LvglDisplay`（`main/display/lvgl_display/`）用于多个板子；`lcd_display.cc:114/134`、`oled_display.cc:46` 已 `lv_init()` + `lvgl_port_init()`。
- 手势：全局触摸任务在 release 时据起终点归一成 Tap/SwipeLeft/Right/Up/Down/LongPress（`esp_vocat.cc:1453-1472`），再路由 `OnGesture` → 按 `mode_` 分发。`mode_`（页面)与对话状态（Listening/Speaking）相互独立。
- 表情引擎 esp_emote_gfx 是 LVGL 派生渲染器（gfx_label/gfx_obj 对应 LVGL 对象），与 LVGL 共享 LVGL 式对象模型。

## 架构

### 渲染所有权

同一个 ST77916 面板同一时刻只能由一个渲染器驱动。经源码核实（`InitializeEmote`，`emote_display.cc:81`），emote 引擎是**自含全屏渲染器**：`emote_init` 自建渲染任务 + 双缓冲，`flush_cb` 直接写面板，**无 canvas/容器 API**（只有 anim/label/img/qrcode），无法把宠物脸渲染进 LVGL 子控件。故**首选 P 不可行**，采用**回退 F**：

- **架构 F（渲染器切换器，已定）**：Home / 对话时 emote 引擎独占面板（保留可爱宠物动画）；情绪学习 / 设置 / 说话覆盖层时 LVGL 接管同一面板渲染真 iPhone 风页面。两者在屏幕边界通过一个 `RenderSwitch` 切换所有权（暂停一方渲染任务、激活另一方）。QSPI 面板操作不可重入，需保证同一时刻只有一个渲染器在写面板、且切换无闪烁。

> **Spike 门槛**：架构 F 能否顺畅落地，取决于 emote 与 LVGL 能否在**同一块 ST77916** 上交替接管且无闪烁、无冲突。实施计划第一条任务必须是这个 spike，产出可运行最小演示：初始化两套渲染做**有效交接**，证明 Home↔页面 往返干净。**涉及渲染器接管的结论不允许在计划里悬空**。若 F 也失败，则需重估全盘 LVGL（F2）——不在本 spec 假定范围。

### 组件归属（规划中的文件层级）

- 新 `EspVocatUi`（含 `RenderSwitch`）：接管屏切换决策。Home / 对话时把面板交给 emote、由现有 `EmoteDisplay` 驱动；情绪学习 / 设置 / 说话覆盖层时把面板交给 LVGL、渲染页面对象树。
- 现有 `emote::EmoteDisplay`：Home / 对话时独占面板渲染宠物表情/动画（保持不变）。
- LVGL 页面层（新，照搬 `LvglDisplay`/`lcd_display.cc` 的 `lv_init`+`lvgl_port_init` 集成）：情绪学习、设置、说话覆盖层的对象树。
- `esp_vocat.cc`：`OnGesture`/`mode_` 表单机继续存在，但 `mode_` 含义与屏切换统一为「当前屏」，且**每次进非对话屏都强制结束对话**。

## 屏与交互映射

统一 iPhone 手势语言：**右滑边缘=返回**、**左右滑=前后翻页/切换**、**左上 ‹ =返回**、每屏有标题。需要把现有"绝对滑动"判定改为"起手位置感知"（右边缘起手左右滑=返回），放进实施任务。

| 界面 | 内容 | 进入 | 退出 |
|---|---|---|---|
| **Home（主屏）** | 宠物脸居中为主角；顶部细状态栏（弱提示，避免牵入时钟子系统，**时间显示列为可选降级项**）；底部弱提示（左右滑翻页） | 开机/任意返回 | — |
| **说话覆盖层** | Siri 式底部半透明波形卡 + 麦克风动画，监听/说话态切换 | Home 长按 | 任意导航手势 / 下滑收起；**退出即强制结束对话** |
| **情绪学习** | 完整卡片：标题「情绪学习」+ 当前情绪名 + 大「开始」按钮 | Home 左滑 | 左上 ‹ / 右滑边缘/上滑返回 |
| **设置** | iPhone 分组列表：亮度、音量，每项 − 值 + 步进 | Home 右滑 | 左上 ‹ / 右滑边缘/上滑返回 |

### 关键行为契约

1. **进任何非对话屏即强制结束对话**（修 bug#2）：从 Home 长按开了对话后，左/右滑→情绪学习/设置，或任意返回，都必须停止 ASR/输出并收起覆盖层。
2. **设置/情绪学习/说话覆盖层不显示宠物脸**（修 bug#1）：这些是 LVGL 独立屏/覆盖层，不含 pet face widget（Home 屏才含）。
3. 返回统一收敛到 Home；`ExitToChat` 语义改为 `ShowHome`。
4. 省电：非 Home 屏保持「高风险呈现，禁止 ligh sleep」语义不变；页面屏同样在亮屏超时后进入一阶显示睡眠（现逻辑保留）。

## 视觉语言（可爱多彩 · iPhone 精致）

- 构图：圆屏内切排版，避免文字顶到边缘；留白充分；层级 = 标题 > 内容 > 操作。
- 卡片：大圆角（贴合圆屏曲率）、柔和阴影、统一间隙网格。
- 配色：深色底为主，配少量精致的彩色强调（沿用可爱多彩基调但收敛，勿花哨）；状态栏/标题用低饱和，操作按钮用高饱和强调色。
- 字体：复用现有中文字体，标题可加大字号。
- 动效：屏切换轻量淡入/滑动，说话波形实时更新，克制不抢戏。

## 渐变与迁移

- 现有 emote-gfx 拼的页面 UI（ShowFunctionPage/ShowSettingsPage/ShowEmotionLearning 等 gfx 标签层）在落地后被真 LVGL 屏取代并淘汰。
- 已合入的修复保留：音量基线（seed from codec）、page_ui_ready_ 原子化。

## 风险与去风险化

| 风险 | 缓解 |
|---|---|
| emote 无法渲染进 LVGL canvas → P 架构不行 | 计划首任务 spike；失败走 F 切换器 |
| LVGL 与 emote 双引擎内存/任务开销 | 复用现有 LvglDisplay 集成路径；评估缓冲与任务 |
| 圆屏上手势判定（边缘返回）适配 | 改判定为起手位置感知，单任务实现+真机验证 |
| 对话结束改为屏切换触发，防误杀 | 清晰的事件顺序（先停语言再切屏），真机回归 |

## 验收

- 真机可：Home 长按说话，左/右滑进情绪学习/设置，返回 Home；对话中滑走对话立即结束；设置/情绪学习屏无宠物脸。
- 视觉：可爱多彩 + iPhone 精致，圆屏排版无裁切。