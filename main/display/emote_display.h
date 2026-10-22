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

// Conversation "Siri-style voice spectrum" overlay, composited below the live
// pet face. Emote draws solid fills with no opacity blend and no arcs, so the
// spectrum is a row of vertical bars whose heights ripple like an audio
// spectrum with the voice: tallest + brightest in the centre, falling away and
// cooling toward the edges (cyan centre -> magenta -> pink), sitting on the
// round panel's bottom arc. Speaking = tall, lively travelling waves;
// listening = gentle breathing. Bars are named labels with a solid background,
// created once and cached.
inline constexpr int kSiriBarCount = 17;     // odd, symmetric around the centre
inline constexpr int kSiriBarWidth = 8;      // px per bar
inline constexpr int kSiriBarSpacing = 10;   // px between bar centres

class EmoteDisplay : public Display {
public:
    using Display::SetPreviewImage;

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
    virtual bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) override;
    virtual bool MountAssets(const char* partition_label) override;
    virtual void UnmountAssets() override;
    virtual bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) override;
    virtual void LoadAssets() override;

    void RefreshAll();

    // Reminder presentation: pause idle animation and show the reminder emotion.
    // The hint caption tells the child how to acknowledge it (tap = confirm /
    // ignore-reminder, long-press = snooze); RestoreFromReminder() returns to
    // the standby/mode presentation.
    void ShowReminder(const char* emotion);
    void RestoreFromReminder();

    // Lazily create + show the reminder hint caption (see ShowReminder).
    void EnsureReminderHint();

    // Conversation "Siri-style voice spectrum" overlay composited over the
    // live pet face. The panel stays owned by emote (the pet keeps animating
    // its listen/speak look via SetStatus); these gfx objects add a bottom arc
    // of vertical bars whose heights ripple like an audio spectrum with the
    // voice (speaking = tall lively waves, listening = gentle breathing) + a
    // slim status label at the top. The center stays clear so the pet stays
    // the focus. LVGL is never involved in a conversation.
    void ShowConversationOverlay(bool speaking);
    void HideConversationOverlay();
    // Flip the overlay's status text / wave energy for listening vs speaking.
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

    // Conversation overlay: a "voice spectrum" of vertical bars fanning up from
    // the round panel's bottom arc (driven by smooth travelling waves, not
    // random). The pet stays centered and calm; the bars stay below it. Emote
    // fills don't blend opacity and there are no arcs, so each bar is a solid
    // label whose height is re-sized every animation tick (motion via size/pos).
    // Show fades the wave in (opa 0->255) and Hide fades it out, so a
    // conversation never pops on/off the screen.
    void EnsureConversationUi();
    void AnimateConversationRipple();
    static void ConversationTimerCallback(void* arg);
    void StartConversationFade(bool fade_in);
    static void ConversationFadeTimerCallback(void* arg);
    void ApplyConversationOpa(uint8_t opa);

    // Emotion-learning lesson caption helpers.
    void EnsureLessonUi();

    emote_handle_t emote_handle_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    std::atomic<bool> panel_writes_enabled_{true};
    esp_timer_handle_t idle_anim_timer_ = nullptr;
    bool is_idle_ = false;
    size_t current_idle_index_ = 0;
    std::vector<std::string> idle_emotions_;

    // The spectrum bars (vertical bars along the bottom arc, heights = voice
    // energy) + the slim status label.
    gfx_obj_t* conversation_status_ = nullptr;
    gfx_obj_t* conversation_bars_[kSiriBarCount] = {};
    esp_timer_handle_t conversation_timer_ = nullptr;
    esp_timer_handle_t conversation_fade_timer_ = nullptr;
    bool conversation_active_ = false;
    bool conversation_speaking_ = false;
    uint32_t conversation_phase_ = 0;
    std::atomic<bool> conversation_ui_ready_ = false;
    int conversation_opa_ = 0;  // current overlay opacity 0..255 (fade progress)
    float conversation_energy_ = 0.0f;  // smoothed amplitude 0..1 (mode transitions)

    // Emotion-learning lesson: a single caption label over the pet face showing
    // the demonstrated emotion's name + progress ("开心 · 3/8"). Created lazily,
    // shown by ShowEmotionLesson, hidden by HideEmotionLesson.
    gfx_obj_t* lesson_status_ = nullptr;
    std::atomic<bool> lesson_ui_ready_ = false;

    // Reminder hint: a small caption telling the child how to acknowledge the
    // current reminder. Created lazily (EnsureReminderHint), shown with each
    // reminder and hidden on restore.
    gfx_obj_t* reminder_hint_ = nullptr;
    std::atomic<bool> reminder_hint_ready_ = false;

    // Home hint: a faint caption under the pet face on the standby Home screen
    // telling the child the core gestures ("左右滑动 · 单击对话"). Shown while
    // the idle animation runs, hidden whenever a higher-priority presentation
    // takes the face (reminder / conversation / lesson), pairing with
    // StartIdleAnimation/StopIdleAnimation.
    void EnsureHomeHint();
    void ShowHomeHint();
    void HideHomeHint();
    gfx_obj_t* home_hint_ = nullptr;
    std::atomic<bool> home_hint_ready_ = false;
};

}  // namespace emote