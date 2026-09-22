#pragma once

#include <atomic>
#include <functional>
#include <string>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "lvgl.h"

// Forward declaration: the emote pet renderer is owned by the board's
// EmoteDisplay instance. RenderSwitch only needs a hook to gate its flushes.
namespace emote {
class EmoteDisplay;
}

// Screens that real LVGL renders when it owns the panel. Home is the emote pet
// face (handled by the emote renderer, not LVGL); the page screens are pure
// LVGL (no pet face).
enum class ScreenId {
    Home,
    EmotionLearning,
    Settings,
    ConversationOverlay,
};

// Owns the single ST77916 round panel and hands it alternately to the emote
// pet renderer (Home / conversation) and to a real LVGL scene (pages). Exactly
// one renderer may flush the panel at a time; RenderSwitch serializes them.
//
// Gate model (confirmed by the Task 1 spike):
//   * LVGL side:  lvgl_port_stop() / lvgl_port_resume() start/stop the LVGL
//     timer task, so while emote owns the panel LVGL produces no flushes at
//     all. This is the documented esp_lvgl_port hook - there is no public way
//     to gate just esp_lvgl_port's internal flush callback.
//   * Emote side: EmoteDisplay::SetPanelWritesEnabled(false) makes the emote
//     flush callback drop panel writes (it feeds emote_notify_flush_finished
//     itself), so the emote task keeps rendering into its own buffer but does
//     not touch the panel while LVGL owns it. The emote API exposes no
//     public pause/stop for its render task, so gating its flush callback is
//     the mechanism that works.
//   * Shared io slot: both emote and esp_lvgl_port want the panel_io's single
//     on_color_trans_done slot (the SPI io driver stores only one callback and
//     silently overwrites). RenderSwitch claims that slot with a fan-out that
//     routes the transfer-done signal to whichever renderer is the current
//     owner.
class RenderSwitch {
public:
    RenderSwitch(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                 int height, emote::EmoteDisplay* emote);
    ~RenderSwitch();

    // Hand the panel to the emote renderer (Home / conversation pet face).
    void ShowEmote();
    // Hand the panel to LVGL and show the given page screen. When build is
    // non-empty it runs under the LVGL lock (in place of the solid-color
    // stand-in) so EspVocatUi can assemble a real screen on the active scr.
    void ShowLvgl(ScreenId id, const std::function<void()>& build);
    void ShowLvgl(ScreenId id);

    // True while LVGL owns the panel (ShowLvgl active); false while emote does.
    bool IsLvglActive() const { return active_lvgl_.load(); }

private:
    // Routes the shared panel_io transfer-done signal to the active renderer.
    static bool IoReadyCallback(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t* edata, void* user_ctx);

    esp_lcd_panel_handle_t panel_;
    esp_lcd_panel_io_handle_t panel_io_;
    int width_;
    int height_;
    emote::EmoteDisplay* emote_;
    // Authoritative owner flag: true = LVGL owns the panel, false = emote.
    std::atomic<bool> active_lvgl_{false};
    lv_display_t* lvgl_display_ = nullptr;
};

// Top-level screen manager for the ESP-VOCAT round panel. Owns a RenderSwitch
// that hands the shared panel alternately to the emote pet renderer (Home /
// conversation) and to real LVGL page screens, and tracks which presentation
// is currently shown. The real page screens arrive in later tasks; for now a
// non-Home screen renders as a solid-color stand-in via the RenderSwitch.
class EspVocatUi {
public:
    EspVocatUi(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
               int height, emote::EmoteDisplay* emote);

    // Return the panel to the emote pet face (Home). Emotion / idle-animation
    // driving stays with the board via display_->SetEmotion(...).
    void ShowHome();

    // Switch to the given LVGL page screen (solid-color stand-in for now).
    void ShowScreen(ScreenId id);

    // Show/hide the conversation overlay. on = ShowScreen(ConversationOverlay) +
    // active=true (Siri-style full-screen scene that replaces the Home pet face
    // while talking). off = ShowHome() + active=false + fire the dialog-gone
    // callback (this is the leave/dialog-gone mechanism; when esp-vocat calls
    // off, the force-stop of the running dialogue is handled downstream).
    void SetConversationActive(bool on);

    // The screen currently being presented (Home or a page).
    ScreenId CurrentScreen() const { return current_; }

    // --- Settings screen (pure LVGL, no pet face) --------------------------
    // Value-change callback signature. EspVocatUi stays decoupled from the
    // backlight / audio service; the board registers these to apply changes.
    using ChangeCb = std::function<void(int)>;
    using BackToHomeCb = std::function<void()>;

    // Inject the current hardware value so the stepper displays reality
    // (esp-vocat calls these at startup). Values clamp to 0-100.
    void SetSettingsValueBrightness(int value);
    void SetSettingsValueVolume(int value);

    // Register callbacks invoked when the user changes a stepper value (only
    // when the value actually changed). Called with the new clamped 0-100 value.
    void SetBrightnessChangeCallback(ChangeCb cb) { brightness_change_cb_ = std::move(cb); }
    void SetVolumeChangeCallback(ChangeCb cb) { volume_change_cb_ = std::move(cb); }

    // Invoked when the settings screen's upper-left back button is pressed.
    void SetBackToHomeCallback(BackToHomeCb cb) { back_to_home_cb_ = std::move(cb); }

    // --- Emotion learning screen (pure LVGL, no pet face) --------------------
    using StartCb = std::function<void()>;

    // Set the central "current emotion" name shown on the learning screen.
    // If the screen is not yet built, the string is cached and applied on build.
    void SetEmotionLearningName(const char* name);

    // Register the callback invoked when the user presses the 开始 button.
    // The board wires this to its existing emotion-learning flow.
    void SetStartLearningCallback(StartCb cb) { start_learning_cb_ = std::move(cb); }

    // --- Conversation overlay (Siri-style, pure LVGL, no pet face) ----------
    // Fired when the conversation overlay is left/gone (SetConversationActive
    // off); the board force-stops the running dialogue (bug #2 fix basis).
    using DialogGoneCb = std::function<void()>;
    void SetDialogGoneCallback(DialogGoneCb cb) { dialog_gone_cb_ = std::move(cb); }

    // Toggle the live status label (正在听/正在说) and the waveform intensity:
    // listening = gentle waveform, speaking = taller/faster. Safe to call
    // before the overlay is built (the state is cached and applied on build).
    void SetSpeaking(bool speaking);

private:
    RenderSwitch render_switch_;
    ScreenId current_ = ScreenId::Home;
    bool conversation_active_ = false;

    // ---- Settings screen implementation ------------------------------------
    // Identifies which stepper was pressed (brightness/volume + direction).
    struct SettingsStepTarget {
        EspVocatUi* ui;
        bool is_brightness;
        int delta;
    };

    static void SettingsStepperEventCb(lv_event_t* e);
    static void SettingsBackEventCb(lv_event_t* e);

    // Build (once) and show the settings screen; reused across ShowScreen calls.
    void BuildSettingsScreen();
    // Build a single grouped row: name label + value label + −/＋ steppers.
    void BuildSettingsRow(lv_obj_t* scr, const char* name, lv_obj_t** value_label_out,
                          bool is_brightness, int y_offset);
    // Clamp, refresh the value label, and fire the change callback if changed.
    void ApplySettingsValue(bool is_brightness, int delta);
    void RefreshSettingsValueLabel(bool is_brightness);

    lv_obj_t* settings_screen_ = nullptr;
    lv_obj_t* brightness_value_label_ = nullptr;
    lv_obj_t* volume_value_label_ = nullptr;
    int brightness_ = 50;  // cached stepper values (0-100), shown on the screen
    int volume_ = 50;
    ChangeCb brightness_change_cb_;
    ChangeCb volume_change_cb_;
    BackToHomeCb back_to_home_cb_;

    // ---- Emotion learning screen implementation ------------------------------
    // Identifies the 开始 button press; heap-allocated once when the screen is
    // built (mirrors Task 3's per-button SettingsStepTarget pattern).
    struct StartLearningTarget {
        EspVocatUi* ui;
    };

    static void EmotionLearningStartEventCb(lv_event_t* e);

    // Build (once) and show the emotion learning screen; reused across
    // ShowScreen calls. Its back button reuses the shared back_to_home_cb_.
    void BuildEmotionLearningScreen();
    // Apply the cached emotion name to the central label if the screen is built.
    void RefreshEmotionLearningName();

    lv_obj_t* emotion_screen_ = nullptr;
    lv_obj_t* emotion_name_label_ = nullptr;
    std::string emotion_name_;  // cached; applied to the label once built
    StartCb start_learning_cb_;

    // ---- Conversation overlay implementation --------------------------------
    static constexpr int kWaveBarCount = 9;  // short bars in the waveform

    // lv_timer_cb_t: animates the waveform bar heights. Runs inside the LVGL
    // task's handler (already under the port lock), so it must NOT lock again.
    static void ConversationWaveformTimerCb(lv_timer_t* timer);

    // Build (once) and show the conversation overlay; reused across shows.
    void BuildConversationOverlayScreen();
    // One waveform tick: pseudo-randomize each bar height by speaking_ state.
    void AnimateConversationWaveform();
    // Apply the cached speaking_ state (status text + timer period). The caller
    // is responsible for holding the LVGL lock when called from outside it.
    void ApplyConversationState();

    lv_obj_t* conversation_screen_ = nullptr;
    lv_obj_t* conversation_status_label_ = nullptr;
    lv_obj_t* waveform_bars_[kWaveBarCount] = {};
    lv_timer_t* conversation_timer_ = nullptr;
    bool speaking_ = false;   // cached; 0 listening / 1 speaking
    uint32_t waveform_phase_ = 0;  // monotonically increasing timer tick counter
    DialogGoneCb dialog_gone_cb_;
};