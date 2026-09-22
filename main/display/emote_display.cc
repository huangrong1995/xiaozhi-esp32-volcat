#include "emote_display.h"

// Standard C++ headers
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <tuple>
#include <unordered_map>

// Standard C headers
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

// Cute-colorful palette for the page-based UI. Values are RGB hex.
static const gfx_color_t kColorPink = GFX_COLOR_HEX(0xFF8FB1);
static const gfx_color_t kColorOrange = GFX_COLOR_HEX(0xFFA94D);
static const gfx_color_t kColorMint = GFX_COLOR_HEX(0x63E6BE);
static const gfx_color_t kColorLavender = GFX_COLOR_HEX(0x9775FA);
static const gfx_color_t kColorSoftYellow = GFX_COLOR_HEX(0xFFD43B);
static const gfx_color_t kColorWhite = GFX_COLOR_HEX(0xFFFFFF);
static const gfx_color_t kColorDarkBg = GFX_COLOR_HEX(0x1A1A2E);

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

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data,
                            emote_handle_t handle) {
    EmoteDisplay* self = static_cast<EmoteDisplay*>(emote_get_user_data(handle));
    if (self == nullptr) {
        return;
    }
    if (self->PanelWritesEnabled()) {
        esp_lcd_panel_draw_bitmap(self->PanelHandle(), x_start, y_start, x_end, y_end, data);
    } else {
        // The panel is currently owned by another renderer (LVGL). Drop the
        // frame but keep the emote render loop alive by acknowledging the
        // flush ourselves (the real draw's io-complete will not occur since
        // nothing was written to the panel).
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
    // Return to the standby/mode presentation by delegating to ShowChatStandby().
    ShowChatStandby();
}

void EmoteDisplay::EnsurePageUi() {
    if (!emote_handle_ || page_ui_ready_) {
        return;
    }

    emote_lock(emote_handle_);

    // Creates a named label, configures it for the round 360x360 screen, and
    // leaves it hidden until a page method shows it. A dark background makes
    // the cute-colored text readable over the pet face.
    auto create_label = [this](const char* name, const char* text, gfx_color_t color, int w, int h,
                               uint8_t align, gfx_coord_t xofs, gfx_coord_t yofs) -> gfx_obj_t* {
        gfx_obj_t* obj = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name);
        if (!obj) {
            return nullptr;
        }
        gfx_label_set_text(obj, text);
        gfx_label_set_color(obj, color);
        gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(obj, GFX_LABEL_LONG_CLIP);
        gfx_label_set_bg_enable(obj, true);
        gfx_label_set_bg_color(obj, kColorDarkBg);
        gfx_obj_set_size(obj, w, h);
        gfx_obj_align(obj, align, xofs, yofs);
        gfx_obj_set_visible(obj, false);
        return obj;
    };

    page_title_ =
        create_label("vocat_page_title", "", kColorPink, 320, 48, GFX_ALIGN_TOP_MID, 0, 40);
    page_hint_ = create_label("vocat_page_hint", "", kColorSoftYellow, 320, 36,
                              GFX_ALIGN_BOTTOM_MID, 0, -30);
    settings_brightness_ =
        create_label("vocat_set_brightness", "", kColorLavender, 260, 40, GFX_ALIGN_CENTER, 0, -40);
    settings_volume_ =
        create_label("vocat_set_volume", "", kColorLavender, 260, 40, GFX_ALIGN_CENTER, 0, 10);
    settings_value_ =
        create_label("vocat_set_value", "", kColorWhite, 260, 32, GFX_ALIGN_BOTTOM_MID, 0, -70);
    emotion_name_ = create_label("vocat_emotion_name", "", kColorSoftYellow, 320, 48,
                                 GFX_ALIGN_TOP_MID, 0, 120);

    // Publish under the lock so a concurrent reader never sees the half-built
    // set of page labels (page_ui_ready_ is atomic; a reader that samples it
    // true will only touch labels that are fully created).
    page_ui_ready_ = true;
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetPageUiVisible(bool visible) {
    EnsurePageUi();
    if (!page_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    if (page_title_)
        gfx_obj_set_visible(page_title_, visible);
    if (page_hint_)
        gfx_obj_set_visible(page_hint_, visible);
    if (settings_brightness_)
        gfx_obj_set_visible(settings_brightness_, visible);
    if (settings_volume_)
        gfx_obj_set_visible(settings_volume_, visible);
    if (settings_value_)
        gfx_obj_set_visible(settings_value_, visible);
    if (emotion_name_)
        gfx_obj_set_visible(emotion_name_, visible);
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ShowChatStandby() {
    ESP_LOGI(TAG, "ShowChatStandby");
    // Hide all page UI, restore the chat look, and restart the idle pet animation.
    SetPageUiVisible(false);
    StartIdleAnimation();
}

void EmoteDisplay::ShowFunctionPage() {
    ESP_LOGI(TAG, "ShowFunctionPage");
    EnsurePageUi();
    if (!page_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    // Function-page card: title + hint. Hide settings/emotion UI.
    if (page_title_) {
        gfx_label_set_text(page_title_, "情绪学习");
        gfx_obj_set_visible(page_title_, true);
    }
    if (page_hint_) {
        gfx_label_set_text(page_hint_, "长按开始 · 上滑返回");
        gfx_obj_set_visible(page_hint_, true);
    }
    if (settings_brightness_)
        gfx_obj_set_visible(settings_brightness_, false);
    if (settings_volume_)
        gfx_obj_set_visible(settings_volume_, false);
    if (settings_value_)
        gfx_obj_set_visible(settings_value_, false);
    if (emotion_name_)
        gfx_obj_set_visible(emotion_name_, false);
    emote_unlock(emote_handle_);
    // Warm face, stop idle animation.
    StopIdleAnimation();
    SetEmotion("happy");
}

void EmoteDisplay::ShowSettingsPage(int selected_index, bool adjusting, int value) {
    ESP_LOGI(TAG, "ShowSettingsPage: selected=%d adjusting=%d value=%d", selected_index, adjusting,
             value);
    EnsurePageUi();
    if (!page_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    // Title + brightness/volume rows; highlight the selected row.
    if (page_title_) {
        gfx_label_set_text(page_title_, "设置");
        gfx_obj_set_visible(page_title_, true);
    }
    if (page_hint_)
        gfx_obj_set_visible(page_hint_, false);
    if (settings_brightness_) {
        gfx_label_set_text(settings_brightness_, "亮度");
        gfx_obj_set_visible(settings_brightness_, true);
        const bool selected = (selected_index == 0);
        gfx_label_set_color(settings_brightness_, selected ? kColorWhite : kColorLavender);
        gfx_label_set_bg_color(settings_brightness_, selected ? kColorMint : kColorDarkBg);
    }
    if (settings_volume_) {
        gfx_label_set_text(settings_volume_, "音量");
        gfx_obj_set_visible(settings_volume_, true);
        const bool selected = (selected_index == 1);
        gfx_label_set_color(settings_volume_, selected ? kColorWhite : kColorLavender);
        gfx_label_set_bg_color(settings_volume_, selected ? kColorOrange : kColorDarkBg);
    }
    if (settings_value_) {
        if (adjusting) {
            gfx_label_set_text(settings_value_, std::to_string(value).c_str());
            gfx_obj_set_visible(settings_value_, true);
        } else {
            gfx_obj_set_visible(settings_value_, false);
        }
    }
    if (emotion_name_)
        gfx_obj_set_visible(emotion_name_, false);
    emote_unlock(emote_handle_);
    // Focused face, stop idle animation.
    StopIdleAnimation();
    SetEmotion("neutral");
}

void EmoteDisplay::ShowEmotionLearning(const char* emotion_name) {
    ESP_LOGI(TAG, "ShowEmotionLearning: %s", emotion_name);
    EnsurePageUi();
    if (!page_ui_ready_) {
        return;
    }
    emote_lock(emote_handle_);
    // Hide all other page UI, show the current emotion name.
    if (page_title_)
        gfx_obj_set_visible(page_title_, false);
    if (page_hint_)
        gfx_obj_set_visible(page_hint_, false);
    if (settings_brightness_)
        gfx_obj_set_visible(settings_brightness_, false);
    if (settings_volume_)
        gfx_obj_set_visible(settings_volume_, false);
    if (settings_value_)
        gfx_obj_set_visible(settings_value_, false);
    if (emotion_name_) {
        gfx_label_set_text(emotion_name_, emotion_name ? emotion_name : "");
        gfx_obj_set_visible(emotion_name_, true);
    }
    emote_unlock(emote_handle_);
    // Face emotion is applied by the caller via SetEmotion(); just stop idle.
    StopIdleAnimation();
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