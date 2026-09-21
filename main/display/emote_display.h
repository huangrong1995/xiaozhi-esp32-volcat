#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <memory>
#include <string>
#include <vector>
#include "display.h"
#include "expression_emote.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                 int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    void RefreshAll();

    // Reminder presentation: pause idle animation and show the reminder emotion.
    // RestoreFromReminder() returns to the standby/mode presentation.
    void ShowReminder(const char* emotion);
    void RestoreFromReminder();

    // Page-based UI. Each method presents a named-gfx-label "page" overlaid on
    // the pet face, toggling visibility of the cached labels on and off.
    void ShowChatStandby();
    void ShowFunctionPage();
    void ShowSettingsPage(int selected_index, bool adjusting, int value);
    void ShowEmotionLearning(const char* emotion_name);

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    // Idle animation methods
    void StartIdleAnimation();
    void StopIdleAnimation();
    static void IdleAnimTimerCallback(void* arg);

    // Page UI: lazily create and cache the named gfx label objects.
    void EnsurePageUi();
    void SetPageUiVisible(bool visible);

    emote_handle_t emote_handle_ = nullptr;
    esp_timer_handle_t idle_anim_timer_ = nullptr;
    bool is_idle_ = false;
    size_t current_idle_index_ = 0;
    std::vector<std::string> idle_emotions_;

    // Cached page UI labels (created under emote_lock/emote_unlock).
    gfx_obj_t* page_title_ = nullptr;
    gfx_obj_t* page_hint_ = nullptr;
    gfx_obj_t* settings_brightness_ = nullptr;
    gfx_obj_t* settings_volume_ = nullptr;
    gfx_obj_t* settings_value_ = nullptr;
    gfx_obj_t* emotion_name_ = nullptr;
    bool page_ui_ready_ = false;
};

}  // namespace emote
