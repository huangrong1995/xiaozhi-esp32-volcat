#include "emote_display.h"

// Standard C++ headers
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <tuple>
#include <unordered_map>

// Standard C headers
#include <math.h>
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "expression_emote.h"
#include "gfx.h"

namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// Cute-colorful palette shared by the conversation overlay and lesson caption.
// Values are RGB hex.
static const gfx_color_t kColorWhite = GFX_COLOR_HEX(0xFFFFFF);
// Opaque label backdrop. Set to the display's own background (gfx_disp
// defaults to 0x0000 black), so the caption rectangles are invisible against
// the screen — white text stays readable over any pet animation without the
// visible "矩形色差" a lighter panel would produce.
static const gfx_color_t kColorDarkBg = GFX_COLOR_HEX(0x000000);

// Conversation "Siri-style voice spectrum" palette. The hue sweeps cyan at the
// centre -> magenta -> pink at the edges; over time the whole sweep drifts
// (kSiriHueDrift) and each bar's brightness follows its height/energy, so the
// spectrum shimmer changes instead of being a static gradient.
static constexpr int kSiriBarHeightMin = 2;            // keep bars visible
static constexpr int kSiriBarHeightMaxSpeaking = 88;   // tall, lively waves
static constexpr int kSiriBarHeightMaxListen = 32;     // gentle breathing
static constexpr float kSiriPanelRadius = 180.0f;      // round panel inscribed circle
static constexpr float kSiriHueDrift = 14.0f;          // deg hue wander over time

// HSV (h in deg, s/v in [0,1]) -> GFX RGB565. Siri's spectrum is a hue sweep,
// so animating hue keeps the whole bar row alive instead of a static gradient.
static gfx_color_t HsvToGfxColor(float h, float s, float v) {
    // Normalize hue to [0,360).
    h = fmodf(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    const float c = v * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f) {
        r = c; g = x;
    } else if (hp < 2.0f) {
        r = x; g = c;
    } else if (hp < 3.0f) {
        g = c; b = x;
    } else if (hp < 4.0f) {
        g = x; b = c;
    } else if (hp < 5.0f) {
        r = x; b = c;
    } else {
        r = c; b = x;
    }
    const float m = v - c;
    const uint32_t ri = static_cast<uint32_t>((r + m) * 255.0f);
    const uint32_t gi = static_cast<uint32_t>((g + m) * 255.0f);
    const uint32_t bi = static_cast<uint32_t>((b + m) * 255.0f);
    return GFX_COLOR_HEX((ri << 16) | (gi << 8) | bi);
}

// Colour for bar i at animation phase t (sec) with relative energy [0,1]:
// hue sweeps cyan(centre) -> pink(edge), slowly drifts with t, and brightness
// follows the bar's energy so high bars glow brighter.
static gfx_color_t SiriBarColor(int i, float t, float energy) {
    const float half = (kSiriBarCount - 1) / 2.0f;
    const float pos = fabsf(static_cast<float>(i) - half) / half;  // 0 centre .. 1 edge
    // Map position to hue: 205 (cyan) centre -> 320 (pink) edge, plus the
    // time drift so the whole sweep slowly breathes through the spectrum.
    const float hue = 205.0f + pos * 115.0f + kSiriHueDrift * sinf(t * 0.09f);
    const float sat = 0.85f;
    // Brightness rides the bar height: quiet bars dim, tall bars glow.
    const float val = 0.72f + 0.28f * energy;
    return HsvToGfxColor(hue, sat, val);
}

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t* const edata, void* user_ctx) {
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote. When the render switch (LVGL) is active, emote
// flushes are dropped but acknowledged ourselves so the emote render loop stays
// alive without writing to the panel (nothing is drawn, so the io-complete
// callback that would normally acknowledge the flush never fires on its own).
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data,
                            emote_handle_t handle) {
    EmoteDisplay* self = static_cast<EmoteDisplay*>(emote_get_user_data(handle));
    if (self == nullptr) {
        return;
    }
    if (self->PanelWritesEnabled()) {
        // A failed draw_bitmap means no transfer was queued, so the io-complete
        // callback that would normally ack this flush will never fire. Fail
        // hard: ack the flush ourselves (dropping the frame) so the emote
        // render loop never blocks forever on WAIT_FLUSH_DONE.
        if (esp_lcd_panel_draw_bitmap(self->PanelHandle(), x_start, y_start, x_end, y_end, data) != ESP_OK) {
            ESP_LOGW(TAG, "panel flush %d,%d-%d,%d failed; dropping frame", x_start, y_start, x_end, y_end);
            emote_notify_flush_finished(handle);
        }
    } else {
        emote_notify_flush_finished(handle);
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(EmoteDisplay* self, const int width, const int height) {
    if (self == nullptr) {
        ESP_LOGE(TAG, "Invalid EmoteDisplay");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags =
            {
                .swap = true,
                .double_buffer = true,
                .buff_dma = false,
                // Render buffers in PSRAM: keeps 23KB of double-buffering out
                // of internal RAM, which is what starves the I2S wake path
                // (i2s_alloc_dma_desc OOM crash). SPI flushes copy through a
                // small temp internal DMA buffer per chunk - slower but stable;
                // direct PSRAM DMA (psram_dma_direct) underflows at 40MHz.
                .buff_spiram = true,
            },
        .gfx_emote =
            {
                .h_res = width,
                .v_res = height,
                .fps = 30,
            },
        .buffers =
            {
                .buf_pixels = static_cast<size_t>(width * 16),
            },
        .task =
            {
                .task_priority = 5,
                .task_stack = 6 * 1024,
                .task_affinity = 0,
                .task_stack_in_ext = false,
            },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)self,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel,
                           const esp_lcd_panel_io_handle_t panel_io, const int width,
                           const int height)
    : emote_handle_(nullptr),
      idle_anim_timer_(nullptr),
      is_idle_(false),
      current_idle_index_(0),
      idle_emotions_({"happy", "loving", "neutral", "confused"}) {
    width_ = width;
    height_ = height;
    panel_ = panel;
    emote_handle_ = InitializeEmote(this, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);

    // Initialize idle animation timer
    esp_timer_create_args_t timer_args = {
        .callback = &EmoteDisplay::IdleAnimTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "idle_anim",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &idle_anim_timer_);
}

EmoteDisplay::~EmoteDisplay() {
    if (idle_anim_timer_) {
        esp_timer_stop(idle_anim_timer_);
        esp_timer_delete(idle_anim_timer_);
        idle_anim_timer_ = nullptr;
    }
    if (conversation_timer_) {
        esp_timer_stop(conversation_timer_);
        esp_timer_delete(conversation_timer_);
        conversation_timer_ = nullptr;
    }
    if (conversation_fade_timer_) {
        esp_timer_stop(conversation_fade_timer_);
        esp_timer_delete(conversation_fade_timer_);
        conversation_fade_timer_ = nullptr;
    }
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (emote_handle_ && emotion && strlen(emotion) > 0) {
        emote_set_anim_emoji(emote_handle_, emotion);
    }
}

void EmoteDisplay::ShowReminder(const char* emotion) {
    // Pause idle animation so it does not overwrite the reminder, then present
    // the reminder emotion plus a hint telling the child how to acknowledge it.
    StopIdleAnimation();
    SetEmotion(emotion);
    EnsureReminderHint();
    if (reminder_hint_ready_) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(reminder_hint_, true);
        emote_unlock(emote_handle_);
    }
}

// Lazily build the reminder hint caption ("点击确认 · 长按忽略"). One label,
// bottom strip like the lesson caption; hidden by RestoreFromReminder.
void EmoteDisplay::EnsureReminderHint() {
    if (!emote_handle_ || reminder_hint_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    gfx_obj_t* hint = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL,
                                               "vocat_reminder_hint");
    if (hint) {
        gfx_label_set_text(hint, "点击确认 · 长按忽略");
        gfx_label_set_color(hint, kColorWhite);
        gfx_label_set_text_align(hint, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(hint, GFX_LABEL_LONG_CLIP);
        gfx_label_set_bg_enable(hint, true);
        gfx_label_set_bg_color(hint, kColorDarkBg);
        gfx_obj_set_size(hint, 220, 30);
        gfx_obj_align(hint, GFX_ALIGN_BOTTOM_MID, 0, -12);
        gfx_obj_set_visible(hint, false);
    }
    reminder_hint_ = hint;
    reminder_hint_ready_ = true;
    emote_unlock(emote_handle_);
}

void EmoteDisplay::RestoreFromReminder() {
    // Hide the acknowledgment hint once the reminder is handled.
    if (reminder_hint_ready_) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(reminder_hint_, false);
        emote_unlock(emote_handle_);
    }
    // Return to the standby presentation. StartIdleAnimation() is a no-op when
    // idle is already running, so this is safe to call at any time.
    StartIdleAnimation();
}

// Conversation "Siri-style voice spectrum": a row of vertical bars fanning up
// from the round panel's bottom arc, composited over the live pet face (panel
// stays owned by emote, so the pet keeps animating). Emote can't draw arcs or
// gradients, so each bar is a named label with a solid background, created
// once and cached; every animation tick re-sizes/re-positions them (see
// AnimateConversationRipple). Bars are tallest and brightest in the centre,
// cooling to the edges, so the whole row reads as a Siri voice spectrum.
void EmoteDisplay::EnsureConversationUi() {
    if (!emote_handle_ || conversation_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    const int cx = width_ / 2;
    for (int i = 0; i < kSiriBarCount; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "vocat_siri_b%d", i);
        gfx_obj_t* bar =
            emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name);
        if (bar) {
            gfx_label_set_text(bar, "");
            gfx_label_set_bg_enable(bar, true);
            gfx_label_set_bg_color(bar, SiriBarColor(i, 0.0f, 0.5f));
            // Park each bar at the bottom of the panel until the first tick.
            const int dx = (i - (kSiriBarCount - 1) / 2) * kSiriBarSpacing;
            gfx_obj_set_size(bar, kSiriBarWidth, kSiriBarHeightMin);
            gfx_obj_set_pos(bar, cx + dx - kSiriBarWidth / 2,
                            static_cast<gfx_coord_t>(height_ - kSiriBarHeightMin));
        }
        conversation_bars_[i] = bar;
    }
    gfx_obj_t* status =
        emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "vocat_conv_status");
    if (status) {
        gfx_label_set_text(status, "");
        gfx_label_set_color(status, kColorWhite);
        gfx_label_set_text_align(status, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(status, GFX_LABEL_LONG_CLIP);
        gfx_label_set_bg_enable(status, true);
        gfx_label_set_bg_color(status, kColorDarkBg);
        // Status sits at the very top so the spectrum bars own the bottom arc.
        gfx_obj_set_size(status, 96, 24);
        gfx_obj_align(status, GFX_ALIGN_TOP_MID, 0, 8);
        gfx_obj_set_visible(status, false);
    }
    conversation_status_ = status;
    conversation_ui_ready_ = true;
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ShowConversationOverlay(bool speaking) {
    ESP_LOGI(TAG, "ShowConversationOverlay: speaking=%d", speaking);
    EnsureConversationUi();
    conversation_active_ = true;
    conversation_speaking_ = speaking;
    if (!conversation_ui_ready_) {
        return;
    }
    // Reset the fade progress so the overlay re-enters fully transparent.
    conversation_opa_ = 0;
    emote_lock(emote_handle_);
    if (conversation_status_) {
        gfx_label_set_text(conversation_status_, speaking ? "正在说" : "正在听");
        gfx_obj_set_visible(conversation_status_, true);
        gfx_label_set_opa(conversation_status_, 0);
    }
    for (int i = 0; i < kSiriBarCount; ++i) {
        if (conversation_bars_[i]) {
            gfx_obj_set_visible(conversation_bars_[i], true);
            gfx_label_set_opa(conversation_bars_[i], 0);
        }
    }
    emote_unlock(emote_handle_);
    // Prime the ripple immediately so it isn't empty on the first frame.
    AnimateConversationRipple();
    // Drive the glow with a periodic esp_timer (created lazily on first show).
    if (conversation_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &EmoteDisplay::ConversationTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "conv_glow",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &conversation_timer_);
    }
    esp_timer_start_periodic(conversation_timer_, 100000);  // 10fps both modes
    // Fade the wave in from transparent (short, ~250ms).
    StartConversationFade(true);
}

void EmoteDisplay::HideConversationOverlay() {
    ESP_LOGI(TAG, "HideConversationOverlay");
    conversation_active_ = false;
    // Stop the wave animation; the fade timer keeps running to dissolve the
    // overlay out (short, ~250ms) instead of popping it off instantly.
    if (conversation_timer_ != nullptr) {
        esp_timer_stop(conversation_timer_);
    }
    StartConversationFade(false);
}

// Fade the conversation overlay in/out. fade_in=true ramps 0->255; fade_out
// ramps 255->0 then hides the objects. Driven by a lightweight one-shot chain
// on the esp_timer task (10ms steps -> ~250ms total per direction).
void EmoteDisplay::StartConversationFade(bool fade_in) {
    if (conversation_fade_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &EmoteDisplay::ConversationFadeTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "conv_fade",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &conversation_fade_timer_);
    }
    // Cancel any fade chain still in flight (e.g. a fade-out mid-run when a new
    // conversation starts) so the old chain cannot fight this one — each fade
    // direction is driven by conversation_fade_dir_, not inferred from opa.
    esp_timer_stop(conversation_fade_timer_);
    // Fade target: full for in, hidden for out.
    conversation_fade_dir_ = fade_in ? +1 : -1;
    conversation_opa_ = fade_in ? 0 : 255;
    ApplyConversationOpa(static_cast<uint8_t>(conversation_opa_));
    esp_timer_start_once(conversation_fade_timer_, 10000);  // 10ms per step
}

void EmoteDisplay::ConversationFadeTimerCallback(void* arg) {
    auto* self = static_cast<EmoteDisplay*>(arg);
    if (self == nullptr) {
        return;
    }
    const int dir = self->conversation_fade_dir_;
    if (dir == 0) {
        return;  // no fade in progress (shouldn't fire, but be safe)
    }
    const int step = 26;  // ~255/10 steps, 10ms each -> ~250ms full fade
    self->conversation_opa_ += dir * step;
    if (self->conversation_opa_ > 255) {
        self->conversation_opa_ = 255;
    }
    if (self->conversation_opa_ < 0) {
        self->conversation_opa_ = 0;
    }
    self->ApplyConversationOpa(static_cast<uint8_t>(self->conversation_opa_));
    const bool done = (dir > 0) ? (self->conversation_opa_ >= 255)
                                : (self->conversation_opa_ <= 0);
    if (done) {
        self->conversation_fade_dir_ = 0;
        if (dir < 0) {
            // Fade-out finished: actually hide the overlay objects.
            if (!self->conversation_ui_ready_) {
                return;
            }
            emote_lock(self->emote_handle_);
            if (self->conversation_status_) {
                gfx_obj_set_visible(self->conversation_status_, false);
            }
            for (int i = 0; i < kSiriBarCount; ++i) {
                if (self->conversation_bars_[i]) {
                    gfx_obj_set_visible(self->conversation_bars_[i], false);
                }
            }
            emote_unlock(self->emote_handle_);
        }
    } else {
        esp_timer_start_once(self->conversation_fade_timer_, 10000);
    }
}

// Apply the current overlay opacity to the status label and all bars.
void EmoteDisplay::ApplyConversationOpa(uint8_t opa) {
    if (!conversation_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    if (conversation_status_) {
        gfx_label_set_opa(conversation_status_, opa);
    }
    for (int i = 0; i < kSiriBarCount; ++i) {
        if (conversation_bars_[i]) {
            gfx_label_set_opa(conversation_bars_[i], opa);
        }
    }
    emote_unlock(emote_handle_);
}

// Emotion-learning flashcard lesson. The board drives the advance timing; here
// we just pause idle, play the demonstrated emotion's expression, and show a
// "name · index/total" caption over the pet face so the user can follow the
// sequence. Mirrors the conversation-overlay label pattern (solid dark caption
// over the pet; the round panel clips nothing at the bottom strip).
void EmoteDisplay::EnsureLessonUi() {
    if (!emote_handle_ || lesson_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    gfx_obj_t* status = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL,
                                                 "vocat_lesson_status");
    if (status) {
        gfx_label_set_text(status, "");
        gfx_label_set_color(status, kColorWhite);
        gfx_label_set_text_align(status, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(status, GFX_LABEL_LONG_CLIP);
        gfx_label_set_bg_enable(status, true);
        gfx_label_set_bg_color(status, kColorDarkBg);
        gfx_obj_set_size(status, 220, 30);
        gfx_obj_align(status, GFX_ALIGN_BOTTOM_MID, 0, -12);
        gfx_obj_set_visible(status, false);
    }
    lesson_status_ = status;
    lesson_ui_ready_ = true;
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ShowEmotionLesson(const char* name, int index, int total, const char* emotion) {
    EnsureLessonUi();
    // Pause idle so it does not overwrite the demonstrated emotion (mirrors
    // ShowReminder), then present the emotion and the name/progress caption.
    StopIdleAnimation();
    if (emotion && strlen(emotion) > 0) {
        SetEmotion(emotion);
    }
    if (!lesson_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    if (lesson_status_) {
        char caption[64];
        snprintf(caption, sizeof(caption), "%s · %d/%d", name ? name : "", index, total);
        gfx_label_set_text(lesson_status_, caption);
        gfx_obj_set_visible(lesson_status_, true);
    }
    emote_unlock(emote_handle_);
}

void EmoteDisplay::HideEmotionLesson() {
    if (!lesson_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    if (lesson_status_) {
        gfx_obj_set_visible(lesson_status_, false);
    }
    emote_unlock(emote_handle_);
    // Return to the standby presentation and restart the idle pet.
    RestoreFromReminder();
}

void EmoteDisplay::SetConversationSpeaking(bool speaking) {
    if (!conversation_active_ || !conversation_ui_ready_) {
        return;
    }
    conversation_speaking_ = speaking;
    emote_lock(emote_handle_);
    if (conversation_status_) {
        gfx_label_set_text(conversation_status_, speaking ? "正在说" : "正在听");
    }
    emote_unlock(emote_handle_);
    if (conversation_timer_ != nullptr) {
        esp_timer_stop(conversation_timer_);
        esp_timer_start_periodic(conversation_timer_, 100000);  // 10fps both modes
    }
}

void EmoteDisplay::ConversationTimerCallback(void* arg) {
    auto* self = static_cast<EmoteDisplay*>(arg);
    if (self != nullptr) {
        self->AnimateConversationRipple();
    }
}

void EmoteDisplay::AnimateConversationRipple() {
    if (!conversation_active_ || !conversation_ui_ready_) {
        return;
    }
    const float t = static_cast<float>(conversation_phase_++);
    const bool speaking = conversation_speaking_;
    // Voice energy per mode: speaking = tall, lively travelling waves; listening
    // = gentle breathing. The amplitude is smoothed (lerp toward the mode's
    // target) so switching between listening and speaking swells or settles the
    // wave gently instead of snapping the bar heights in place.
    const float kListenEnergy = static_cast<float>(kSiriBarHeightMaxListen) /
                                static_cast<float>(kSiriBarHeightMaxSpeaking);
    const float energy_target = speaking ? 1.0f : kListenEnergy;
    conversation_energy_ += (energy_target - conversation_energy_) * 0.12f;
    if (conversation_energy_ < 0.01f) {
        conversation_energy_ = 0.01f;
    }
    const float max_h = static_cast<float>(kSiriBarHeightMaxSpeaking) * conversation_energy_;
    const float speed = speaking ? 0.45f : 0.25f;
    const float cx = width_ / 2.0f;
    const int half = (kSiriBarCount - 1) / 2;  // 12: index of the centre bar
    emote_lock(emote_handle_);
    for (int i = 0; i < kSiriBarCount; ++i) {
        gfx_obj_t* bar = conversation_bars_[i];
        if (bar == nullptr) {
            continue;
        }
        const float dx = static_cast<float>((i - half) * kSiriBarSpacing);
        // Envelope: tallest at the centre, smooth falloff to the edges (cos
        // quarter-wave -> 1 centre, 0 edge).
        const float f = fabsf(dx) / static_cast<float>(half * kSiriBarSpacing);
        const float env = cosf(f * 1.5707963f);
        // Travelling wave: the phase advances with x, so the bars roll across
        // the spectrum like sound energy instead of swelling in place.
        const float wave = 0.5f + 0.5f * sinf(t * speed - static_cast<float>(i) * 0.38f);
        // Speaking adds a slower second harmonic so the spectrum has a lively,
        // speech-like beat rather than a uniform pump.
        float mod = 1.0f;
        if (speaking) {
            mod = 0.85f + 0.15f * sinf(t * 0.11f + static_cast<float>(i) * 0.21f);
        }
        float h = max_h * env * (0.25f + 0.75f * wave) * mod;
        if (h < static_cast<float>(kSiriBarHeightMin)) {
            h = static_cast<float>(kSiriBarHeightMin);
        }
        // Colour follows motion: hue drifts slowly over time and brightness
        // tracks the bar's height, so tall bars glow and the row shimmers.
        const float energy = h / max_h;
        gfx_label_set_bg_color(bar, SiriBarColor(i, t * 0.1f, energy));
        // The bar base sits on the round panel's bottom arc, so the row hugs
        // the circular edge (higher at the sides, lowest at the very bottom).
        const float y_base = (height_ / 2.0f) +
                             sqrtf(kSiriPanelRadius * kSiriPanelRadius - dx * dx);
        gfx_obj_set_size(bar, kSiriBarWidth, static_cast<uint16_t>(h));
        gfx_obj_set_pos(bar, static_cast<gfx_coord_t>(cx + dx - kSiriBarWidth / 2.0f),
                        static_cast<gfx_coord_t>(y_base - h));
    }
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content) {
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (emote_handle_ && content && strlen(content) > 0) {
        if ((std::strcmp(role, "system") == 0) && std::strstr(content, "xiaozhi.me")) {
            size_t len = strlen(content);
            char* new_content = new char[len + 1];
            strcpy(new_content, content);
            std::replace(new_content, new_content + len, static_cast<char>(0x0A),
                         static_cast<char>(0x20));
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
            delete[] new_content;
        } else {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
        }
    }
}

void EmoteDisplay::SetStatus(const char* const status) {
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            // Stop idle animation when listening
            StopIdleAnimation();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
            // Start idle animation
            StartIdleAnimation();
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            // Stop idle animation when speaking
            StopIdleAnimation();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            StopIdleAnimation();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        }
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all) {
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on) {
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image) {
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

bool EmoteDisplay::MountAssets(const char* partition_label) {
    if (emote_handle_ == nullptr || partition_label == nullptr) {
        return false;
    }
    const emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source =
            {
                .partition_label = partition_label,
            },
        .flags =
            {
                .mmap_enable = true,
            },
    };
    return emote_mount_assets(emote_handle_, &data) == ESP_OK;
}

void EmoteDisplay::UnmountAssets() {
    if (emote_handle_ != nullptr) {
        emote_unmount_assets(emote_handle_);
    }
}

bool EmoteDisplay::GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
    if (emote_handle_ == nullptr) {
        return false;
    }
    return emote_get_asset_data_by_name(emote_handle_, name.c_str(), &data, &size) == ESP_OK;
}

void EmoteDisplay::LoadAssets() {
    if (emote_handle_ != nullptr) {
        emote_load_assets(emote_handle_);
    }
}

void EmoteDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool EmoteDisplay::Lock(const int timeout_ms) {
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock() {}

bool EmoteDisplay::StopAnimDialog() {
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) {
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::RefreshAll() {
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

// Home hint: a faint caption under the pet face telling the child the core
// interactions. Built lazily like the other captions; shown while the standby
// idle animation runs (the Home face) and hidden whenever a higher-priority
// presentation takes the face.
void EmoteDisplay::EnsureHomeHint() {
    if (!emote_handle_ || home_hint_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    gfx_obj_t* hint =
        emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "vocat_home_hint");
    if (hint) {
        gfx_label_set_text(hint, "左右滑动 · 单击对话");
        gfx_label_set_color(hint, GFX_COLOR_HEX(0x8A9AA0));  // soft grey, unobtrusive
        gfx_label_set_text_align(hint, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(hint, GFX_LABEL_LONG_CLIP);
        gfx_label_set_bg_enable(hint, false);  // no backdrop: faint floating caption
        gfx_obj_set_size(hint, 220, 24);
        gfx_obj_align(hint, GFX_ALIGN_BOTTOM_MID, 0, -8);
        gfx_obj_set_visible(hint, false);
    }
    home_hint_ = hint;
    home_hint_ready_ = true;
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ShowHomeHint() {
    EnsureHomeHint();
    if (home_hint_ready_) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(home_hint_, true);
        emote_unlock(emote_handle_);
    }
}

void EmoteDisplay::HideHomeHint() {
    if (home_hint_ready_) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(home_hint_, false);
        emote_unlock(emote_handle_);
    }
}

void EmoteDisplay::StartIdleAnimation() {
    if (idle_anim_timer_ && !is_idle_) {
        is_idle_ = true;
        current_idle_index_ = 0;
        // Show first emotion immediately
        if (!idle_emotions_.empty()) {
            SetEmotion(idle_emotions_[current_idle_index_].c_str());
        }
        // Start timer to cycle emotions (every 3 seconds)
        esp_timer_start_periodic(idle_anim_timer_, 3000000);
        // Home standby: show the gesture hint under the pet.
        ShowHomeHint();
        ESP_LOGI(TAG, "Idle animation started");
    }
}

void EmoteDisplay::StopIdleAnimation() {
    if (idle_anim_timer_ && is_idle_) {
        esp_timer_stop(idle_anim_timer_);
        is_idle_ = false;
        // A higher-priority presentation is taking the face: hide the hint.
        HideHomeHint();
        SetEmotion("neutral");
        ESP_LOGI(TAG, "Idle animation stopped");
    }
}

void EmoteDisplay::IdleAnimTimerCallback(void* arg) {
    auto* self = static_cast<EmoteDisplay*>(arg);
    if (self && self->is_idle_ && !self->idle_emotions_.empty()) {
        self->current_idle_index_ = (self->current_idle_index_ + 1) % self->idle_emotions_.size();
        self->SetEmotion(self->idle_emotions_[self->current_idle_index_].c_str());
    }
}

}  // namespace emote