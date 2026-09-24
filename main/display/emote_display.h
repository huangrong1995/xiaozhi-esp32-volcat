#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "display.h"
#include "expression_emote.h"

namespace emote {

// Conversation "voice ripple" overlay, composited below the live pet face.
// Emote draws solid fills with no opacity blend (no soft glow) and no
// arcs/gradients, so a circular ripple is faked as a fan of small squares
// tiling a lower arc around a point just below the pet. The squares are spaced
// to overlap by ~1px (count per ring sized for its max surge radius), so the
// row joins into smooth continuous ripple LINES rather than dotted dots. Only
// the lower arc is drawn (squares sit at y >= the ring center) so the ripples
// never climb over the pet's face above. The rings ripple outward with the
// voice (speaking = strong outward surge, listening = gentle breathing).
// Shared as namespace constants so both the class member array and the .cc
// file-scope tables can size from them.
inline constexpr int kRippleRings = 3;
// Squares per ring, sized so adjacent squares overlap (no gaps) even at each
// ring's maximum surge radius. Smaller squares = thinner lines, but more of
// them are needed to stay continuous. Total is the flat object array size.
inline constexpr int kRippleSegCounts[kRippleRings] = {46, 70, 80};
inline constexpr int kRippleSegTotal =
    kRippleSegCounts[0] + kRippleSegCounts[1] + kRippleSegCounts[2];

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

    // Conversation "voice ribbon" overlay composited over the live pet face.
    // The panel stays owned by emote (the pet keeps animating its listen/speak
    // look via SetStatus); these gfx objects add a bottom ribbon of colored
    // bars whose heights shimmer like a voice waveform (blue->cyan->purple->
    // magenta->pink gradient) + a slim status label. The center stays clear so
    // the pet stays the focus. LVGL is never involved in a conversation.
    void ShowConversationOverlay(bool speaking);
    void HideConversationOverlay();
    // Flip the overlay's status text / ribbon energy for listening vs speaking.
    void SetConversationSpeaking(bool speaking);

    // Emotion-learning flashcard lesson, composited over the live pet face (like
    // the conversation overlay). ShowEmotionLesson pauses idle, plays the
    // demonstrated emotion's expression, and shows a "name · index/total"
    // caption so the user can follow the sequence; the board drives the advance
    // timing. HideEmotionLesson hides the caption and restores the standby pet.
    void ShowEmotionLesson(const char* name, int index, int total, const char* emotion);
    void HideEmotionLesson();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

    // Panel-write gate used by RenderSwitch. When disabled, the emote flush
    // callback drops panel flushes (acknowledging them itself) so another
    // renderer (LVGL) can own the panel. When re-enabled, emote resumes
    // writing to the panel.
    void SetPanelWritesEnabled(bool enabled) { panel_writes_enabled_.store(enabled); }
    bool PanelWritesEnabled() const { return panel_writes_enabled_.load(); }
    esp_lcd_panel_handle_t PanelHandle() const { return panel_; }

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
    esp_lcd_panel_handle_t panel_ = nullptr;
    std::atomic<bool> panel_writes_enabled_{true};
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
    std::atomic<bool> page_ui_ready_ = false;

    // Conversation overlay: a "voice ripple" of concentric rings fanning outward
    // from a point just below the live pet (driven by a smooth outward-traveling
    // envelope, not random). The pet stays centered and calm; the rings stay
    // below it. Emote fills don't blend opacity and there are no arcs, so each
    // ring is tiled from overlapping squares (motion + shape via size/pos).
    void EnsureConversationUi();
    void AnimateConversationRipple();
    static void ConversationTimerCallback(void* arg);

    // The ripple squares (tiling each ring's lower arc into continuous lines)
    // + the slim status label.
    gfx_obj_t* conversation_status_ = nullptr;
    gfx_obj_t* conversation_segs_[kRippleSegTotal] = {};
    esp_timer_handle_t conversation_timer_ = nullptr;
    bool conversation_active_ = false;
    bool conversation_speaking_ = false;
    uint32_t conversation_phase_ = 0;
    std::atomic<bool> conversation_ui_ready_ = false;

    // Emotion-learning lesson: a single caption label over the pet face showing
    // the demonstrated emotion's name + progress ("开心 · 3/8"). Created lazily,
    // shown by ShowEmotionLesson, hidden by HideEmotionLesson.
    void EnsureLessonUi();
    gfx_obj_t* lesson_status_ = nullptr;
    std::atomic<bool> lesson_ui_ready_ = false;
};

}  // namespace emote
