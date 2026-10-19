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
static const gfx_color_t kColorDarkBg = GFX_COLOR_HEX(0x1A1A2E);

// Conversation "voice ripple" palette. Each ring is a single color that gets
// cooler as it moves outward: warm pink nearest the pet -> magenta -> cyan at
// the edge, so the ripple reads as sound energy fading as it travels.
static const gfx_color_t kRippleColor[kRippleRings] = {
    GFX_COLOR_HEX(0xFF8FB1),  // pink   (inner ring, near the pet)
    GFX_COLOR_HEX(0xE06BFA),  // magenta (mid ring)
    GFX_COLOR_HEX(0x64E3FF),  // cyan   (outer ring)
};

// "Voice ripple" geometry. kRippleCenterY is the shared ring center just below
// the pet's face; line segments are only placed on the lower arc (y >= center)
// so they never climb over the pet above. Each ring has a base radius plus a
// voice-driven outward surge that grows stronger toward the outside
// (kRippleTravel), so ripples visibly spread from inner to outer. Each ring's
// lower arc is tiled from kRippleSegCounts[r] small squares, spaced to overlap
// by ~1px so the row joins into smooth continuous ripple lines. Angles are
// degrees from the right axis (0 = right, 90 = straight down).
static constexpr int kRippleSquareSize = 3;
static constexpr int kRippleCenterY = 268;
static constexpr int kRippleBaseRadius[kRippleRings] = {28, 48, 68};
static constexpr float kRippleTravel[kRippleRings] = {1.0f, 1.3f, 1.6f};
static constexpr int kRippleMinAngle = 25;
static constexpr int kRippleMaxAngle = 155;
static constexpr int kRippleMaxRadius = 88;  // cap so the bottom arc stays on screen

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
      idle_emotions_({"happy", "confused", "angry", "shocked"}) {
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
    // the reminder emotion.
    StopIdleAnimation();
    SetEmotion(emotion);
}

void EmoteDisplay::RestoreFromReminder() {
    // Return to the standby presentation. StartIdleAnimation() is a no-op when
    // idle is already running, so this is safe to call at any time.
    StartIdleAnimation();
}

// Conversation "voice ripple" overlay: concentric rings fanning outward from a
// point just below the pet, composited over the live pet face (panel stays
// owned by emote, so the pet keeps animating). Emote can't draw arcs, so each
// ring's lower arc is tiled from small overlapping squares (kRippleSegCounts[r]
// per ring), colored by radius and rippling outward with the voice (see
// AnimateConversationRipple). The squares are named labels with a solid
// background, created once and cached. Only the lower arc is drawn (y >= center)
// so the ripples never climb over the pet's face above.
void EmoteDisplay::EnsureConversationUi() {
    if (!emote_handle_ || conversation_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    const int cx = width_ / 2;
    int flat = 0;
    for (int r = 0; r < kRippleRings; ++r) {
        for (int s = 0; s < kRippleSegCounts[r]; ++s, ++flat) {
            char name[24];
            snprintf(name, sizeof(name), "vocat_rip_r%d_s%d", r, s);
            gfx_obj_t* sq =
                emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name);
            if (sq) {
                gfx_label_set_text(sq, "");
                gfx_label_set_bg_enable(sq, true);
                gfx_label_set_bg_color(sq, kRippleColor[r]);
                // Park each square at its ring's bottom until the first tick.
                gfx_obj_set_size(sq, kRippleSquareSize, kRippleSquareSize);
                gfx_obj_set_pos(sq, cx - kRippleSquareSize / 2,
                                kRippleCenterY + kRippleBaseRadius[r] - kRippleSquareSize / 2);
            }
            conversation_segs_[flat] = sq;
        }
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
        gfx_obj_set_size(status, 120, 28);
        gfx_obj_align(status, GFX_ALIGN_BOTTOM_MID, 0, -6);
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
    emote_lock(emote_handle_);
    if (conversation_status_) {
        gfx_label_set_text(conversation_status_, speaking ? "正在说" : "正在听");
        gfx_obj_set_visible(conversation_status_, true);
    }
    for (int i = 0; i < kRippleSegTotal; ++i) {
        if (conversation_segs_[i]) {
            gfx_obj_set_visible(conversation_segs_[i], true);
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
    esp_timer_start_periodic(conversation_timer_, speaking ? 100000 : 200000);
}

void EmoteDisplay::HideConversationOverlay() {
    ESP_LOGI(TAG, "HideConversationOverlay");
    conversation_active_ = false;
    if (conversation_timer_ != nullptr) {
        esp_timer_stop(conversation_timer_);
    }
    if (!conversation_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    if (conversation_status_) {
        gfx_obj_set_visible(conversation_status_, false);
    }
    for (int i = 0; i < kRippleSegTotal; ++i) {
        if (conversation_segs_[i]) {
            gfx_obj_set_visible(conversation_segs_[i], false);
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
        esp_timer_start_periodic(conversation_timer_, speaking ? 100000 : 200000);
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
    // Ripple energy per mode: how far the rings surge outward and how fast the
    // wave rolls. Speaking = strong + lively outward spread; listening = gentle.
    float amp, speed;
    if (speaking) {
        amp = 22.0f;
        speed = 0.65f;
    } else {
        amp = 9.0f;
        speed = 0.35f;
    }
    constexpr float kD2R = 0.01745329252f;  // pi / 180
    const int w = width_;
    const int cx = w / 2;
    const float a_min = static_cast<float>(kRippleMinAngle) * kD2R;
    emote_lock(emote_handle_);
    int flat = 0;
    for (int r = 0; r < kRippleRings; ++r) {
        // Outward-traveling wave: outer rings lag the inner ones (phase offset)
        // and surge further (kRippleTravel), so the ripples read as spreading
        // out from the pet instead of swelling all at once.
        const float wave = 0.5f + 0.5f * sinf(t * speed + r * 1.15f);
        float radius = static_cast<float>(kRippleBaseRadius[r]) + amp * kRippleTravel[r] * wave;
        if (radius > static_cast<float>(kRippleMaxRadius)) {
            radius = static_cast<float>(kRippleMaxRadius);
        }
        const int count = kRippleSegCounts[r];
        const float a_step = static_cast<float>(kRippleMaxAngle - kRippleMinAngle) * kD2R /
                             static_cast<float>(count - 1);
        for (int s = 0; s < count; ++s, ++flat) {
            gfx_obj_t* sq = conversation_segs_[flat];
            if (sq == nullptr) {
                continue;
            }
            // Place equal-size squares along the arc. Each ring's count is sized
            // so adjacent squares overlap by ~1px even at max surge, so the row
            // reads as a smooth continuous line rather than separate dots.
            const float a = a_min + a_step * static_cast<float>(s);
            const int x = static_cast<int>(static_cast<float>(cx) + radius * cosf(a)) -
                          kRippleSquareSize / 2;
            const int y = static_cast<int>(static_cast<float>(kRippleCenterY) + radius * sinf(a)) -
                          kRippleSquareSize / 2;
            gfx_obj_set_pos(sq, x, y);
        }
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

void EmoteDisplay::StartIdleAnimation() {
    if (idle_anim_timer_ && !is_idle_) {
        is_idle_ = true;
        current_idle_index_ = 0;
        // Show first emotion immediately
        if (!idle_emotions_.empty()) {
            SetEmotion(idle_emotions_[current_idle_index_].c_str());
        }
        // Start timer to cycle emotions (every 3 seconds)
        esp_timer_start_periodic(idle_anim_timer_, 30000000);
        ESP_LOGI(TAG, "Idle animation started");
    }
}

void EmoteDisplay::StopIdleAnimation() {
    if (idle_anim_timer_ && is_idle_) {
        esp_timer_stop(idle_anim_timer_);
        is_idle_ = false;
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