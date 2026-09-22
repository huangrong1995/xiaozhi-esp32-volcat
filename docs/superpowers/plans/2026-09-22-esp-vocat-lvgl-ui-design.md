# ESP-VoCAt LVGL 页面化 UI 与 iPhone 式交互 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `2026-09-22-esp-vocat-lvgl-ui-design` spec，把 ESP-VoCAt 重构为「真 LVGL 页面层（架构 F 渲染器切换）＋ iPhone 式扁平导航 + Siri 式对话覆盖层」，并修复两个交互 bug。

**Architecture:** 同一块 ST77916 面板由 `RenderSwitch` 交替交付给两个渲染器——Home/对话时 emote 引擎独占（保留可爱宠物动画），情绪学习/设置/说话覆盖层时 LVGL 接管渲染真 iPhone 风页面。`esp_vocat.cc` 的 `mode_`/`OnGesture` 重接为「当前屏」。

**Tech Stack:** C++ / ESP-IDF 5.5 / LVGL **9.5.0**（v9 指针式 API）/ esp_emote_gfx / ST77916 360×360 圆屏。

**Spec:** `docs/superpowers/specs/2026-09-22-esp-vocat-lvgl-ui-design.md`（本计划从 spec 论证而立，spec 随行）。

## Global Constraints

- 只改：`main/boards/esp-vocat/esp_vocat.cc`、`main/boards/esp-vocat/config.h`(仅若需要)、`main/display/emote_display.cc/h`、以及**新增** `main/display/esp_vocat_ui.h/.cc`（Ui + RenderSwitch 层）。**不得改 managed_components**（含 emote gfx、lvgl、esp_lvgl_port）。
- 复用已有 LVGL 集成先例做模板：`main/display/lcd_display.cc:113-154`（`lv_init` + `lvgl_port_init` + `lvgl_port_add_disp`，`esp_lvgl_port_add_disp` 接收 `.panel_handle`/`.io_handle`，可复用板子已建 panel）。
- 模板可视化主题：`main/display/lvgl_display/lvgl_theme.*`（`LvglTheme`，提供 `text_color()/background_color()/border_color()`）。
- LVGL **v9** API：`lv_obj_t*` 指针、`lv_obj_create`、`lv_obj_set_style_*(obj, val, sel)`、`lv_label_set_text`、`lv_scr_load`、`lv_timer_create`。不要用 v8 句柄式 API。
- 不改通信协议、音频管线核心、两阶省电架构与阈值。
- 已合入且保留：音量基线 seed、`page_ui_ready_` 原子化。
- Google C++ style；`.clang-format`；跑 `clang-format` 收尾。

---

### Task 1（Spike）：渲染器切换可行性 + 固定 RenderSwitch 接口

**文件（只读参考 + 新增 spike 演示）：**
- 新增：`main/display/esp_vocat_ui.h`（仅接口声明：`EspVocatUi` 前半 + `RenderSwitch` 函数签名）
- 修改：`main/boards/esp-vocat/esp_vocat.cc`（临时接管 init 以便演示切换；spike 通过后 Task 2 落定为最终结构）

**目的（kill 条件）：** 证明 emote 与 LVGL 能在**同一块 ST77916 panel 句柄**上交替接管、干净无闪烁、无并发写面板冲突。spike 产出本计划的 RenderSwitch 接口定义；**spike 失败则停下上报**（需重估全盘 LVGL F2，不在本计划假定范围）。

**Interfaces —— 由 spike 产出并固定（后续任务消费，签名以 spike 实证为准）：**
```cpp
// esp_vocat_ui.h （spike 时产出；Task 2 起按此契约实现/消费）
enum class ScreenId { Home, EmotionLearning, Settings, ConversationOverlay };
class RenderSwitch {
 public:
  RenderSwitch(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
               int width, int height);
  // 把面板交给 emote（Home / 对话）。仅 Home 时 emote 渲染宠物。
  void ShowEmote();
  // 把面板交给 LVGL，显示指定页面屏。非 Home 屏不含宠物脸。
  void ShowLvgl(ScreenId id);
  bool IsLvglActive() const;
};
```

- [ ] **Step 1: Read** 现有集成模板 `main/display/lcd_display.cc` 全部、`emote_display.cc` 的 `InitializeEmote`、`esp_vocat.cc:1790-1830`（panel/io 创建）。
- [ ] **Step 2:** 实现 spike 版 `RenderSwitch`：
  - 用 `lv_init()` + `lvgl_port_init(&port_cfg)` + `esp_lvgl_port_add_disp({.io_handle, .panel_handle, ...})` 在**同一 panel/io** 上加一个 LVGL 显示（照抄 `lcd_display.cc:113-154` 的配置，含 `LV_COLOR_FORMAT_RGB565`、`buffer_size = width*20`、`rotation` 用 esp-vocat 现配置）。
  - `ShowEmote()`：停 LVGL 的渲染任务（`lv_timer_pause` / 设 gate 让 lvgl flush 不写面板），让 emote 恢复正在渲染；`ShowLvgl()`：停 emote 对面板的写入（gate emote 的 `flush_cb` 或用 emote 暂停 API），`lv_scr_load` 该屏并让 lvgl 任务跑。
  - 用一把互斥 + `std::atomic<bool> active_lvgl_` 保证两渲染器不同时 flush（QSPI 面板操作不可重入）。
- [ ] **Step 3:** 在 `esp_vocat.cc` 启动路径临时构造 RenderSwitch 并做**往返演示**：初始化时 `ShowEmote()` 跑 2 秒表情 → `ShowLvgl(Settings)` 显示一块纯色 LVGL 屏 2 秒 → 回 `ShowEmote()`。真机/闭环观察：无闪烁撕裂、无卡死、面板正常。
- [ ] **Step 4:** 记录证实的接口细节（哪个 gate 机制、是否需停任务、esp_lvgl_port 配置是否可用板子 panel），把 `RenderSwitch` 接口写进 `esp_vocat_ui.h`。若 emote/LVGL 无法在同一 panel 交替，回报 BLOCKED 并说明。
- [ ] **Step 5:** `clang-format -i` 新建/改到的文件。
- [ ] **Step 6:** Commit（spike 结果 + 接口声明）。

### Task 2：EspVocatUi 骨架 + LVGL 页面层

**文件：**
- 新增：`main/display/esp_vocat_ui.cc`（实现 Task 1 的 `RenderSwitch` + `EspVocatUi` 屏管理）
- 修改：`esp_vocat.cc`（用 `EspVocatUi` 构建屏切换，Home 默认 emote）

**Interfaces:**
- Consumes: Task 1 的 `RenderSwitch::ShowEmote()/ShowLvgl(ScreenId)/IsLvglActive()`。
- Produces: `EspVocatUi` 骨千：
```cpp
class EspVocatUi {
 public:
  void ShowHome();                       // RenderSwitch::ShowEmote() + 起 Home 呈现
  void ShowScreen(ScreenId id);          // RenderSwitch::ShowLvgl(id) + 加载对应 LVGL 屏
  void SetConversationActive(bool on);   // 开启/关闭说话覆盖层，并联动结束对话的下游
};
```
- Consumes（Home 呈现）: `emote::EmoteDisplay` 现有宠物动画 API（`SetEmotion`、idle 动画）继续由 esp-vocat 的现有情绪/提醒逻辑驱动。

- [ ] **Step 1: Read** `esp_vocat.cc` 的 `EnterMode/ExitToChat/ResetToStandby/OnGesture`（604-699）、`display_` 用法（474,673-699,1822）。
- [ ] **Step 2:** 实现 `EspVocatUi::ShowHome()/ShowScreen(id)`：内部调 RenderSwitch；并维护一个「当前屏」int，供 esp-vocat 查询。
- [ ] **Step 3:** 在 esp-vocat 启动时构造 `EspVocatUi`（记下现有 `display_` 为 emote 引用），把 `display_` 相关入口改为经 `EspVocatUi` 分发（Home 走 emote，页面走 LVGL）。保持 `Display*` 抽象在 Home 路径仍可用。
- [ ] **Step 4:** build（`source export.sh && idf.py build` 或 `scripts/release.py esp-vocat`）通过；Home（emote）启动动画正常。
- [ ] **Step 5:** `clang-format -i`。
- [ ] **Step 6:** Commit。

### Task 3：设置屏（LVGL 分组列表）

**文件：** 修改 `main/display/esp_vocat_ui.cc`（+ `.h` 若需）。
**Interfaces:** Consumes `RenderSwitch::ShowLvgl(ScreenId::Settings)`；Produces `EspVocatUi::SetSettingsValueBrightness(int)/SetSettingsValueVolume(int)` 与屏刷新回调。

**行为契约（spec）：** iPhone 分组列表——标题「设置」；两行「亮度 / 音量」，各带 `− 值 +` 步进；左上 `‹` 返回 Home。不含宠物脸。值修改实时回调到 esp-vocat（调 `backlight_->SetBrightness` / `AudioService::SetOutputVolume`，沿用现有夹钳 0-100）。

- [ ] **Step 1:** 用 LVGL v9 搭设置屏对象树：一个全屏 `lv_obj_create` 屏；`lv_style` 用可爱多彩色板（深底 `0x1A1A2E`、卡片强调 `0xFF8FB1`/`0x9775FA` 等，见 Task 7 色板）；两行卡片（圆角 `lv_obj_set_style_radius`、柔和阴影 `lv_obj_set_style_shadow_*`）；步进按钮 `−/+/‹` 为 `lv_button_create`，值 `lv_label_create`。
- [ ] **Step 2:** 绑定两行步进逻辑：`−/＋` 改值 → 回调上抛 + 刷新 `值` label；持屏内状态 `brightness_/volume_`（int 0-100）。
- [ ] **Step 3:** 左上 `‹` 触发返回（回调上抛 `OnBackToHome()`）。
- [ ] **Step 4:** build + 用 `ShowLvgl(ScreenId::Settings)` 真机查看排版无裁切（圆屏内切）。
- [ ] **Step 5:** `clang-format -i`，Commit。

### Task 4：情绪学习屏（LVGL 卡片）

**文件：** 修改 `esp_vocat_ui.cc/.h`。
**Interfaces:** Consumes `ShowLvgl(ScreenId::EmotionLearning)`；Produces `SetEmotionLearningName(const char*)`（更新当前情绪名 label）、`RegisterStartButton(on_start)`（点「开始」进入学习的回调上抛）。

**行为契约：** 完整卡片；标题「情绪学习」；中央当前情绪名 label；大「开始」按钮；左上 `‹` 返回 Home；不含宠物脸。

- [ ] **Step 1:** 搭对象树：全屏屏；标题 label；情绪名 label（大号，可随情绪变色）；`开始` 大按钮（`lv_button_create` 放大尺寸/圆角/强调色）。
- [ ] **Step 2:** `开始` 点击 → 上抛 `on_start`；`‹` → 上抛 `OnBackToHome()`。
- [ ] **Step 3:** 提供 `SetEmotionLearningName(name)` 供 esp-vocat 更新。
- [ ] **Step 4:** build + 真机查看；`clang-format -i`；Commit。

### Task 5：说话覆盖层（Siri 式，LVGL）

**文件：** 修改 `esp_vocat_ui.cc/.h`。
**Interfaces:** Consumes `ShowLvgl(ScreenId::ConversationOverlay)` / `SetConversationActive(bool)`；Produces 覆盖层的监听/说话态切换 `SetSpeaking(bool)`。

**行为契约（spec）：** 半透明底部波形卡 + 麦克风动画；监听/说话态随设备状态变化；**任何导航离开覆盖层（返回/翻页）都会下抛「结束对话」，esp-vocat 据此强制停语言**（修 bug#2）。不含宠物脸。

- [ ] **Step 1:** 搭覆盖层对象树：半透明卡片（`lv_obj_set_style_opa` / 半透明深色底）；动画波形（`lv_timer_create` 周期性改若干短条高度，模拟音频）；麦克风图标/状态 label。
- [ ] **Step 2:** `SetSpeaking(bool)`：切换"正在听/正在说"文案与波形状态。
- [ ] **Step 3:** 离层回调：返回/翻页/覆盖层上滑 → 上抛 `OnDialogGone()`。
- [ ] **Step 4:** build + 真机看波形；`clang-format -i`；Commit。

### Task 6：交互重接（esp_vocat.cc iPhone 式导航）

**文件：** 修改 `esp_vocat.cc`（主），可能微调 `config.h`（手势阈值）。
**Interfaces:** Consumes `EspVocatUi::ShowHome/ShowScreen/SetConversationActive`、情绪学习/设置的屏内回调。

**改动：**
1. **`mode_` 语义统一为「当前屏」**：`Chat`→`Home`、`FunctionPage`→`EmotionLearning`（检测屏可作为 Home 的附属）、`SettingsPage`→`Settings`、新增 `Conversation` 态。命名与 `ScreenId` 对齐。
2. **手势模型（iPhone 式）**：Home 长按→开对话（`SetConversationActive(true)` + `ShowLvgl(ConversationOverlay)`）；Home 左滑→情绪学习、右滑→设置；**非 Home 屏（含 Dialog）左/右滑或上滑→回 Home**；左上 `‹`/屏内回调→回 Home。**开对话后任意翻页/返回→`SetConversationActive(false)` 并强制结束对话**（调现有停止语言/收尾逻辑）。
3. **手势判定改起手感知**（`esp_vocat.cc:1456-1472`）：从"绝对位移"改为"右边缘起手的左右滑=返回、非边缘=左右翻页"，避免圆屏误判（阈值取 `config.h`，若需微调）。
4. **强制结束对话**（修 bug#2）：在进入非 Home 屏与返回 Home 的分支，都保证对话覆盖层退出且语言管线停止。
5. **退役旧 emote-gfx 页面 UI**：`ShowFunctionPage/ShowSettingsPage/ShowEmotionLearning/ShowChatStandby`（emote 拼页、gfx 标签层）在 UI 切换改用 LVGL 后移除或标记废弃，避免两套并存。`ResetToStandby`/`EnterMode`/`ExitToChat` 改走 `EspVocatUi`。

- [ ] **Step 1: Read** `esp_vocat.cc` 全函（重点 604-810 的 mode_/OnGesture/handlers、1456-1472 手势判定、对话停止相关函数）。
- [ ] **Step 2:** `mode_`/`ScreenId` 对齐重命名；`OnGesture` 改为按"当前屏"分发。
- [ ] **Step 3:** 屏间导航接线（改到各 handler + 屏内回调）。
- [ ] **Step 4:** 对话覆盖层生命周期接线（开/关/强制结束）。
- [ ] **Step 5:** 手势判定改起手感知并验证四方向。
- [ ] **Step 6:** 退役 emote-gfx 页面 UI，清无用函数。
- [ ] **Step 7:** `clang-format -i`；build；Commit。

### Task 7：视觉打磨（可爱多彩 · iPhone 精致）

**文件：** 修改 `esp_vocat_ui.cc`（样式集中在某处为佳）+ `esp_vocat_ui.h`。

**目的：** 统一三屏与覆盖层的视觉语言。**色板（复用 spec）**：深底 `0x1A1A2E`，强调粉色 `0xFF8FB1`、橙 `0xFFA94D`、薄荷 `0x63E6BE`、薰衣草 `0x9775FA`、软黄 `0xFFD43B`、白 `0xFFFFFF`。

- [ ] **Step 1:** 抽一组共享 `lv_style`（屏背景/卡片/标题/正文/主按钮色），Task 3/4/5 屏统一套用。
- [ ] **Step 2:** 圆角/阴影/留白对齐：大圆角贴合圆屏曲率；卡片柔和阴影；层级=标题>内容>操作。
- [ ] **Step 3:** 屏切换动效：`lv_scr_load` 配轻量淡入/位移（`lv_anim` 或组件运行配合）。
- [ ] **Step 4:** 中文排版：沿用现有中文字体，标题加大字号。
- [ ] **Step 5:** `clang-format -i`；build；真机目视；Commit。

### Task 8：真机验收 + 收尾

**文件：** 不改代码（除非验收暴露问题）。
**验收（spec）：**
- [ ] 真机：Home 长按开对话（Siri 覆盖层、波形动）；左滑进情绪学习、右滑进设置；各屏左上 `‹` 返回/滑屏返回 Home。
- [ ] **bug#2**：对话中左/右滑走→覆盖层立即消失、语言停止。
- [ ] **bug#1**：设置/情绪学习/覆盖层均无宠物脸；仅 Home 显示宠物。
- [ ] 圆屏无裁剪、视觉可爱多彩+iPhone 精致。
- [ ] 编译产物 `releases/v2.2.6_esp-vocat.zip` 生成（`scripts/release.py esp-vocat`），烧录 `/dev/ttyACM0` 验证。
- [ ] 记录结论到 SDD ledger；必要时按 reviewer 意见小修。

---

## 执行说明

- Task 1 是唯一有 kill 条件的任务：spike 失败即停并上报，不做 Task 2+ 的猜测实现。
- 每个 LVGL 屏（Task 3/4/5）独立可测：用 `ShowLvgl(id)` 即可在只完成该屏时真机核验排版与行为。
- Task 6 是最大改动且依赖 Task 3/4/5 的屏存在，评审在 Task 6 后重点查 mode_ 到 Screen 的映射与对话强制结束的正确性。