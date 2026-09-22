#pragma once

#include <atomic>

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
    // Hand the panel to LVGL and show the given page screen.
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

    // Skeleton: records whether a conversation overlay is active and logs.
    // Real overlay presentation is deferred to a later task.
    void SetConversationActive(bool on);

    // The screen currently being presented (Home or a page).
    ScreenId CurrentScreen() const { return current_; }

private:
    RenderSwitch render_switch_;
    ScreenId current_ = ScreenId::Home;
    bool conversation_active_ = false;
};