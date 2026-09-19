# ESP-VoCat 交互与节能重设计实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 ESP-VoCat 重构为“屏幕主交互、外圈抚摸辅助”的统一手势分发和模式状态机，并加入统一提醒、分层反馈及两级节能唤醒。

**Architecture:** 在 `esp_vocat.cc` 内建立 `Gesture`/`Mode` 语义层和集中 `OnGesture()` 分发入口；底层触摸、IMU、BOOT 回调只负责转换输入。`Chat` 是根模式，情绪学习和瑜伽是可退出子模式；提醒和显示反馈使用统一优先级，节能复用公共 `PowerSaveTimer`，外圈电容垫作为浅睡眠首选唤醒源。

**Tech Stack:** ESP-IDF 5.5、C++、FreeRTOS、ESP timer、`PowerSaveTimer`、CST816S LCD 触摸、ESP32-S3 capacitive touch、BMI270、EmoteDisplay。

**Spec:** `docs/superpowers/specs/2026-09-19-esp-vocat-interaction-redesign-design.md`

## Global Constraints

- 屏幕是唯一语义点击/滑动输入源；外圈电容垫只产生 `Pet` 反馈或休眠唤醒，不产生点击语义。
- 保留单击、长按、左右滑、倾斜、外圈抚摸；删除双击、三击。
- `Chat` 是唯一根状态；情绪学习和瑜伽必须支持长按退出与 30 秒无操作自动退出。
- BOOT 键保留对话开始/停止兜底；设备启动阶段仍进入 Wi-Fi 配网。
- 长按不再切换静音，也不再隐式记录喝水；喝水提醒通过屏幕单击确认、长按忽略。
- 两级节能阈值默认：30 秒关屏、60 秒进入浅睡眠；阈值必须可配置。
- 浅睡眠关闭唤醒词和音频输入；优先通过外圈电容触摸唤醒，LCD 触摸和 BOOT 键作为备用唤醒源。
- 每个任务完成后运行 `python3 scripts/release.py esp-vocat`；提交前运行 clang-format 检查。
- 不引入跨板卡通用 `InputManager`/`ModeManager` 框架，不实现上下滑功能，不扩展文字 UI。

---

## 文件结构与职责

- Modify `main/boards/esp-vocat/esp_vocat.cc`: 本板卡的手势归一化、模式路由、提醒调度、节能生命周期和传感器适配。
- Modify `main/boards/esp-vocat/config.h`: 本板卡手势/模式/节能时间常量及触摸唤醒相关 GPIO 配置。
- Modify `main/display/emote_display.h`: 声明模式/提醒/待机反馈控制接口（仅在现有 Display 抽象允许时加入）。
- Modify `main/display/emote_display.cc`: 实现反馈层互斥、待机恢复和模式/提醒显示。
- Modify `main/boards/esp-vocat/README.md`: 同步最终手势、模式、提醒、节能和唤醒说明。
- Create `main/boards/esp-vocat/interaction_types.h` only if the implementation needs to keep `Gesture`/`Mode` out of the already-large board file; otherwise define small enums locally in `esp_vocat.cc` to avoid unnecessary public API。

---

### Task 1: 建立统一手势与模式骨架

**Files:**
- Modify: `main/boards/esp-vocat/esp_vocat.cc:437-510, 918-1115`
- Modify: `main/boards/esp-vocat/config.h`
- Test: `python3 scripts/release.py esp-vocat`

**Interfaces:**
- Produces `enum class Gesture { None, Tap, LongPress, SwipeLeft, SwipeRight, SwipeUp, SwipeDown, Pet, Shake, ChatToggle }`。
- Produces `enum class Mode { Chat, EmotionLearning, Yoga }`。
- Produces `void OnGesture(Gesture gesture)`、`void EnterMode(Mode mode)`、`void ExitToChat()`、`void ResetModeIdleTimer()`。

- [ ] **Step 1: Add the explicit enums and member state.**

  In `EspVocat`, replace `emotion_learning_mode_` and `yoga_mode_` as decision state with `Mode mode_ = Mode::Chat`; retain the existing emotion/pose indices. Add an `esp_timer_handle_t mode_idle_timer_` and a `static void mode_idle_timer_callback(void* arg)` declaration/definition. Add constants for `kModeIdleTimeoutMs = 30000` and document that mode state is accessed by the interaction task/callback context only.

- [ ] **Step 2: Add the central dispatch methods with safe default behavior.**

  Implement `OnGesture()` as a mode switch:

  ```cpp
  void OnGesture(Gesture gesture) {
      ResetModeIdleTimer();
      switch (mode_) {
      case Mode::Chat: HandleChatGesture(gesture); break;
      case Mode::EmotionLearning: HandleEmotionLearningGesture(gesture); break;
      case Mode::Yoga: HandleYogaGesture(gesture); break;
      }
  }
  ```

  Add the three private handlers with only the existing behavior routed through them initially. `Pet` and `Shake` must not change mode. `ChatToggle` must call `Application::GetInstance().ToggleChatState()` only outside reminder handling.

- [ ] **Step 3: Make mode entry/exit single-owner operations.**

  Update `EnterEmotionLearningMode()`, `ExitEmotionLearningMode()`, `EnterYogaChallengeMode()`, and `ExitYogaChallengeMode()` to assign `mode_`, stop/restart the idle timer as appropriate, and make repeated entry idempotent. Add `EnterMode()`/`ExitToChat()` wrappers so future handlers do not set mode flags directly.

- [ ] **Step 4: Add the mode idle timer lifecycle.**

  Create the one-shot timer in the constructor, restart it on entering either child mode and on each accepted gesture, and have its callback call `ExitToChat()` only when `mode_ != Mode::Chat`. Stop/delete it in the destructor. The callback must not block or call `vTaskDelay`.

- [ ] **Step 5: Compile before changing input behavior.**

  Run:

  ```bash
  source /home/hrong/workspace/code/esp-idf-5.5/export.sh
  python3 scripts/release.py esp-vocat
  ```

  Expected: the board builds with the new enums/timer and existing behavior still reachable.

- [ ] **Step 6: Commit the structural checkpoint.**

  ```bash
  git add main/boards/esp-vocat/esp_vocat.cc main/boards/esp-vocat/config.h
  git commit -m "refactor(esp-vocat): add unified interaction mode skeleton\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 2: Normalize LCD touch gestures and make screen the semantic input

**Files:**
- Modify: `main/boards/esp-vocat/esp_vocat.cc:315-435, 757-816`
- Modify: `main/boards/esp-vocat/config.h`
- Test: `python3 scripts/release.py esp-vocat`

**Interfaces:**
- Consumes `OnGesture(Gesture)` from Task 1.
- Produces `Tap`, `LongPress`, `SwipeLeft`, `SwipeRight` from CST816S touch input.

- [ ] **Step 1: Extend CST816S tracking to distinguish tap, long press, and four-direction swipe.**

  Store press timestamp and start coordinates on `TOUCH_PRESS`; update the latest point during `TOUCH_HOLD`; on `TOUCH_RELEASE`, compute duration and deltas. Use a minimum swipe distance of 80 pixels and require the dominant axis to exceed the other axis. Emit a single `LongPress` when duration is at least 1200 ms; otherwise emit the corresponding directional swipe or `Tap`. Do not emit both a swipe and a tap for one release.

- [ ] **Step 2: Route normalized events to `OnGesture()`.**

  Replace `app.ToggleChatState()` and direct yoga entry in `touch_event_task` with `board.OnGesture(...)`. Keep the startup special case: a touch release while `kDeviceStateStarting` calls `EnterWifiConfigMode()` and does not emit a normal gesture.

- [ ] **Step 3: Implement Chat handler semantics.**

  In `HandleChatGesture()` map `Tap` to `ToggleChatState`, `SwipeRight` to `EnterYogaChallengeMode`, `SwipeLeft` to `EnterEmotionLearningMode`, `LongPress` to a short cancel/help feedback without muting, and ignore mode-only gestures. Ensure a swipe does not toggle chat.

- [ ] **Step 4: Implement child-mode screen semantics.**

  In `HandleEmotionLearningGesture()`, map left/right to previous/next emotion, tap to current-emotion feedback, and long press to `ExitToChat()`. In `HandleYogaGesture()`, map left/right to previous/next pose, tap to replay the current pose, and long press to `ExitToChat()`.

- [ ] **Step 5: Build and inspect gesture routing.**

  Run the board build and inspect logs/compile output for references to the old direct `ToggleChatState()` in `touch_event_task`. Expected: only `OnGesture()` owns normal screen gesture dispatch.

- [ ] **Step 6: Commit the screen input checkpoint.**

  ```bash
  git add main/boards/esp-vocat/esp_vocat.cc main/boards/esp-vocat/config.h
  git commit -m "feat(esp-vocat): route screen gestures through interaction modes\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 3: Convert outer capacitive touch to pet feedback and sleep wake input

**Files:**
- Modify: `main/boards/esp-vocat/esp_vocat.cc:864-887, 1036-1129`
- Modify: `main/boards/esp-vocat/config.h`
- Test: `python3 scripts/release.py esp-vocat`

**Interfaces:**
- Consumes `OnGesture(Gesture::Pet)` and the sleep manager’s wake callback.
- Produces no `Tap`, `LongPress`, or `ChatToggle` from the outer capacitive sensor.

- [ ] **Step 1: Remove the capacitive tap-count state and triple-tap branches.**

  Delete `tap_count_`, `tap_first_time_`, `touch_last_release_time_`, `touch_press_time_`, and `touch_is_pressed_` once no remaining callback needs them. Remove `ShowDoubleTapFeedback()`, the triple-tap detection, the long-press mute branch, and the drink completion call from `touch_button_event_callback`.

- [ ] **Step 2: Normalize slider and button events to `Pet`.**

  Keep slider swipe/release recognition only as a tactile interaction signal. Call `OnGesture(Gesture::Pet)` once per debounced interaction, with the existing 1200 ms feedback cooldown moved to a member field so it is not hidden static state. For the single-pad board, active/inactive transitions should likewise call `OnGesture(Pet)` once on release.

- [ ] **Step 3: Ensure `Pet` never changes mode.**

  Route `Pet` through the mode handlers as feedback-only. It may wake the device from power save, but after wake it must not enter a mode, toggle chat, advance an emotion, or mark a drink complete.

- [ ] **Step 4: Build and audit old gesture symbols.**

  Run the board build and grep for `tap_count_`, `ShowDoubleTapFeedback`, `touch_last_release_time_`, `is_muted`, and `MarkDrinkCompleted` in the board file. Expected: no old outer-touch semantic gesture code remains; `MarkDrinkCompleted()` remains only as an explicit reminder action for Task 4.

- [ ] **Step 5: Commit the capacitive input checkpoint.**

  ```bash
  git add main/boards/esp-vocat/esp_vocat.cc main/boards/esp-vocat/config.h
  git commit -m "refactor(esp-vocat): make outer touch pet-only\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 4: Unify reminders and make drink confirmation explicit

**Files:**
- Modify: `main/boards/esp-vocat/esp_vocat.cc:475-634, 918-993`
- Modify: `main/display/emote_display.h`
- Modify: `main/display/emote_display.cc`
- Test: `python3 scripts/release.py esp-vocat`

**Interfaces:**
- Produces one active `Reminder` record with `type`, `message`, `emotion`, `sound`, `due_time`, and `handled`/`snooze_count` state.
- `OnGesture(Tap)` and `OnGesture(LongPress)` are consumed by the active reminder before normal mode handling.

- [ ] **Step 1: Define a single reminder record and queue state.**

  Replace the parallel fixed-time and drink display actions with a small in-class `Reminder` struct and one pending reminder slot/queue. Preserve the five daily schedule entries and the persisted habit fields. Add `reminder_active_`, `reminder_snooze_count_`, and a bounded retry constant (for example, three reminders) so an ignored drink reminder eventually becomes quiet.

- [ ] **Step 2: Make reminder callbacks enqueue instead of rendering directly.**

  `reminder_timer_callback` must detect due reminders and enqueue them; it must not directly call `SetEmotion`, `PlaySound`, or block with `vTaskDelay`. Add a non-blocking `ShowNextReminder()`/`DismissReminder()` path that serializes audio and display updates.

- [ ] **Step 3: Add reminder-first gesture routing.**

  In `OnGesture()`, if a reminder is active, route `Tap` to `MarkDrinkCompleted()` only for `Drink`, route `LongPress` to dismiss/snooze, and ignore mode navigation until the reminder is handled. Schedule reminders can use tap to acknowledge and long press to dismiss without changing habit state.

- [ ] **Step 4: Give reminders a dedicated display priority.**

  Add the minimum `EmoteDisplay` API needed to enter/leave reminder presentation without changing the existing `Display` base interface unnecessarily. Ensure reminder rendering pauses idle animation and that dismissal restores the current mode/standby presentation. Use a semantically appropriate reminder emotion instead of `confused` for water.

- [ ] **Step 5: Remove blocking delays from reminder paths.**

  Replace the two `vTaskDelay(500)` sound calls with queued/non-blocking sound scheduling or a single sound supported by the existing audio API. The timer callback and display callback must remain short and non-blocking.

- [ ] **Step 6: Build and inspect reminder references.**

  Run the board build and grep for direct `PlaySound`/`ShowTemporaryEmotion` calls from `reminder_timer_callback`. Expected: timer only schedules; active reminder handling owns acknowledgement and dismissal.

- [ ] **Step 7: Commit the reminder checkpoint.**

  ```bash
  git add main/boards/esp-vocat/esp_vocat.cc main/display/emote_display.h main/display/emote_display.cc
  git commit -m "feat(esp-vocat): unify reminders and explicit drink confirmation\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 5: Isolate mode/feedback display priority and yoga completion flow

**Files:**
- Modify: `main/boards/esp-vocat/esp_vocat.cc:624-685, 954-1034`
- Modify: `main/display/emote_display.h`
- Modify: `main/display/emote_display.cc`
- Test: `python3 scripts/release.py esp-vocat`

**Interfaces:**
- Produces `ResetToStandby()`/mode presentation methods used by interaction handlers and reminder dismissal.
- Consumes `Mode` and current emotion/pose state from Tasks 1–4.

- [ ] **Step 1: Add a display presentation owner.**

  Track the current presentation layer (`Standby`, `Mode`, `Reminder`, `TransientFeedback`) in the board/display integration. `SetEmotion` calls from transient feedback must not restart or permanently overwrite a mode presentation. Provide `ResetToStandby()` that stops transient animation, restores idle animation when appropriate, and leaves the device state emotion consistent.

- [ ] **Step 2: Make emotion-learning presentation explicit.**

  Keep the current emotion visible while in the mode, add the mode-entry/exit feedback, and add a visual direction cue through existing display APIs where possible. Do not add a new text rendering framework in this task.

- [ ] **Step 3: Make yoga pose navigation and completion deterministic.**

  Add previous/next pose helpers with wraparound. On a correct IMU direction, stop accepting duplicate matches for the same pose during the success animation, show success feedback, increment the pose, and then show the next arrow. After the last pose, play completion feedback and call `ExitToChat()`.

- [ ] **Step 4: Keep IMU behavior mode-aware.**

  Shake feedback is active only in `Chat`; yoga tilt matching is active only in `Yoga`. Both paths must call `ResetModeIdleTimer()` through `OnGesture(Shake)` or an equivalent mode-aware event rather than silently extending a mode from unrelated sensor noise.

- [ ] **Step 5: Build and audit mode exit paths.**

  Run the board build and verify there are call sites for `ExitYogaChallengeMode()`/`ExitToChat()` from long press, timeout, and completion. Confirm no code writes `emotion_learning_mode_` or `yoga_mode_` directly.

- [ ] **Step 6: Commit the display/mode checkpoint.**

  ```bash
  git add main/boards/esp-vocat/esp_vocat.cc main/display/emote_display.h main/display/emote_display.cc
  git commit -m "feat(esp-vocat): isolate mode presentation and yoga completion\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 6: Add two-level power saving and capacitive wake-up

**Files:**
- Modify: `main/boards/esp-vocat/esp_vocat.cc:437-455, 1307-1387`
- Modify: `main/boards/esp-vocat/config.h`
- Read/reuse: `main/boards/common/power_save_timer.h:8-34`, `main/boards/common/power_save_timer.cc:62-131`
- Test: `python3 scripts/release.py esp-vocat`

**Interfaces:**
- Produces `TouchPowerSaveActivity()`, `EnterDisplayPowerSave()`, `ExitDisplayPowerSave()`, and `ConfigureWakeSources()`.
- Uses `PowerSaveTimer` only after verifying the board target links the common component.

- [ ] **Step 1: Add configurable power-save constants and state.**

  Add `kDisplaySleepTimeoutSeconds = 30`, `kLightSleepTimeoutSeconds = 60`, and member state for last interaction/display sleep. Any `Tap`, swipe, `Pet`, `Shake`, BOOT event, reminder, mode entry, or audio activity must reset the activity timer.

- [ ] **Step 2: Implement level-one display sleep.**

  After 30 seconds of eligible idle, call `GetDisplay()->SetPowerSaveMode(true)` and set backlight brightness to the lowest safe value or off using the existing `PwmBacklight` API. Do not disable wake-word/audio at this level. Any normalized gesture or BOOT event calls `WakeDisplay()` before normal dispatch and restores the previous brightness.

- [ ] **Step 3: Wire `PowerSaveTimer` for level-two light sleep.**

  Instantiate the common `PowerSaveTimer` with the board’s configured timeout and callbacks. The enter callback must stop display animation, disable wake-word detection/audio through the existing helper behavior, and configure the wake sources. The exit callback must restore CPU power management, audio input, wake-word detection, backlight, and display presentation.

- [ ] **Step 4: Configure outer capacitive touch as the preferred wake source.**

  Use the ESP-IDF touch wake-up API for the configured `TOUCH_PAD1`/`TOUCH_PAD2` channels. Configure LCD touch interrupt GPIO10 and BOOT GPIO0 as fallback wake sources only if their polarity and ESP32-S3 wake capability are confirmed for this board. Do not assume the custom polling task runs during light sleep.

- [ ] **Step 5: Reinitialize or resynchronize capacitive sensing after wake.**

  On wake, clear the first stale active/inactive edge so the wake gesture produces no accidental tap, mode entry, drink confirmation, or chat toggle. Resume the custom slider/button polling task only after wake state is stable.

- [ ] **Step 6: Gate power saving by application state.**

  Ensure power save never starts while in EmotionLearning/Yoga, while a reminder is active, while audio/protocol activity is open, or while the device is not `kDeviceStateIdle`. Use `Application::CanEnterSleepMode()` plus the interaction manager’s mode/reminder checks.

- [ ] **Step 7: Build and inspect power-save linkage.**

  Run the board build. If the board does not link the common `PowerSaveTimer`, add only the existing common source/component dependency required by project CMake conventions; do not duplicate its implementation in the board file.

- [ ] **Step 8: Commit the power-save checkpoint.**

  ```bash
  git add main/boards/esp-vocat/esp_vocat.cc main/boards/esp-vocat/config.h main/CMakeLists.txt
  git commit -m "feat(esp-vocat): add touch-wake power saving\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 7: Update board documentation and perform final verification

**Files:**
- Modify: `main/boards/esp-vocat/README.md`
- Modify: all implementation files changed by Tasks 1–6
- Test: formatting, build, source audits, manual hardware matrix

- [ ] **Step 1: Rewrite the README interaction table.**

  Document screen-first semantics: tap, long press, left/right swipe, yoga tilt, outer pet feedback, BOOT fallback, and removal of double/triple tap. Document Chat, EmotionLearning, and Yoga entry/exit behavior and the 30-second timeout.

- [ ] **Step 2: Document reminders and power saving.**

  State that drink confirmation is a screen tap while the reminder is active; long press dismisses it. Document 30-second display sleep, 60-second light sleep, disabled wake word during light sleep, and outer capacitive touch as the preferred wake action with LCD/BOOT fallback.

- [ ] **Step 3: Run formatting checks on only changed C++ files.**

  ```bash
  clang-format --dry-run -Werror main/boards/esp-vocat/esp_vocat.cc main/display/emote_display.cc main/display/emote_display.h
  ```

  If formatting fails, run clang-format on those files and inspect the diff before accepting it.

- [ ] **Step 4: Run the release build.**

  ```bash
  source /home/hrong/workspace/code/esp-idf-5.5/export.sh
  python3 scripts/release.py esp-vocat
  ```

  Expected: build and packaging complete without new warnings/errors. Record any environment-only warning separately; do not claim a clean build if it fails.

- [ ] **Step 5: Run source-level regression audits.**

  Confirm these searches return no obsolete interaction paths:

  ```bash
  grep -n "ShowDoubleTapFeedback\|tap_count_\|touch_last_release_time_\|static bool is_muted" main/boards/esp-vocat/esp_vocat.cc
  grep -n "ExitYogaChallengeMode\|ExitToChat\|OnGesture" main/boards/esp-vocat/esp_vocat.cc
  ```

  Expected: obsolete gesture symbols absent; yoga exit and central dispatch have multiple expected call sites.

- [ ] **Step 6: Execute the hardware acceptance matrix.**

  Verify on a real ESP-VoCat:

  1. Screen tap starts/stops conversation; BOOT tap behaves identically outside startup.
  2. Screen right swipe enters Yoga; long press and 30s inactivity return to Chat.
  3. Screen left swipe enters EmotionLearning; left/right navigate; long press and timeout return to Chat.
  4. Yoga tilt advances only on the correct direction; completion exits cleanly.
  5. Outer touch produces only pet feedback and never toggles chat or changes mode.
  6. Shake feedback works only in Chat.
  7. Drink reminder tap confirms; long press dismisses; no hidden mute/drink coupling remains.
  8. After 30s idle the screen powers down while wake word remains available.
  9. After 60s eligible idle the device enters light sleep; touching the outer capacitive pad wakes it, restores audio/wake-word/display, and does not generate a phantom tap.
  10. Reminders and mode indicators are not overwritten by idle animation.

- [ ] **Step 7: Commit documentation and verification results.**

  ```bash
  git add main/boards/esp-vocat/README.md main/boards/esp-vocat/esp_vocat.cc main/display/emote_display.cc main/display/emote_display.h main/boards/esp-vocat/config.h
  git commit -m "docs(esp-vocat): document redesigned interaction and power save\n\nCo-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

## Plan self-review

- **Spec coverage:** input normalization (Tasks 1–3), mode state/exit (Tasks 1, 2, 5), reminder queue and explicit drink confirmation (Task 4), feedback priority (Task 5), two-level power saving and outer capacitive wake (Task 6), README/test matrix (Task 7).
- **Placeholder scan:** no `TBD`, `TODO`, or unspecified implementation step is required; all risky hardware assumptions are explicit verification steps.
- **Type consistency:** `Gesture`, `Mode`, `OnGesture`, `EnterMode`, `ExitToChat`, and `ResetModeIdleTimer` are introduced in Task 1 and consumed by later tasks; `PowerSaveTimer` is reused with its existing API.
- **Scope check:** all tasks belong to the approved single-board interaction redesign; no cross-board framework or unrelated refactor is introduced.
