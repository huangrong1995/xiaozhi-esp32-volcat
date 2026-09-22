#include "display/esp_vocat_ui.h"

#include <cstdio>

#include <esp_log.h>

#include "display/emote_display.h"
#include "esp_lvgl_port.h"

// The board's built-in full-range CJK text font (font_puhui_20_4, from the
// xiaozhi-fonts component). Used for every label on the LVGL settings screen
// because the default LVGL montserrat set has no CJK glyphs and only size 14
// is compiled in by default.
LV_FONT_DECLARE(font_puhui_20_4);

// Log tag shared by RenderSwitch and EspVocatUi.
static const char* TAG = "esp-vocat";

// Solid-color stand-in for a real LVGL page, per ScreenId. Replaced by the real
// page screens in a later task; kept so a non-Home screen renders something.
static lv_color_t SpikePageColor(ScreenId id) {
    switch (id) {
        case ScreenId::EmotionLearning:
            return lv_color_hex(0xFF8FB1);  // pink
        case ScreenId::Settings:
            return lv_color_hex(0x63E6BE);  // mint
        case ScreenId::ConversationOverlay:
            return lv_color_hex(0xFFA94D);  // orange
        case ScreenId::Home:
        default:
            return lv_color_hex(0x1A1A2E);  // dark
    }
}

bool RenderSwitch::IoReadyCallback(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t* edata, void* user_ctx) {
    (void)panel_io;
    (void)edata;
    auto* self = static_cast<RenderSwitch*>(user_ctx);
    if (self == nullptr) {
        return false;
    }
    // Route the transfer-done signal to whichever renderer is the active owner.
    if (self->active_lvgl_.load() && self->lvgl_display_ != nullptr) {
        lvgl_port_flush_ready(self->lvgl_display_);
    } else if (self->emote_ != nullptr) {
        emote_notify_flush_finished(self->emote_->GetEmoteHandle());
    }
    return true;
}

RenderSwitch::RenderSwitch(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                           int width, int height, emote::EmoteDisplay* emote)
    : panel_(panel), panel_io_(panel_io), width_(width), height_(height), emote_(emote) {
    ESP_LOGI(TAG, "RenderSwitch: init LVGL on emote-owned panel");

    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    // The SPI panel_io stores exactly one on_color_trans_done callback and
    // silently overwrites the previous one (both emote and esp_lvgl_port each
    // want that slot). Claim it with our fan-out so emote keeps getting its
    // flush completion even before/after esp_lvgl_port registers its own.
    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = RenderSwitch::IoReadyCallback,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io_, &io_cbs, this);

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };
    lvgl_display_ = lvgl_port_add_disp(&display_cfg);
    if (lvgl_display_ == nullptr) {
        ESP_LOGE(TAG, "RenderSwitch: esp_lvgl_port_add_disp failed to attach existing panel");
        return;
    }

    // esp_lvgl_port registered its own io callback during add_disp; reclaim the
    // slot so our fan-out is the stable owner for both renderers.
    esp_lcd_panel_io_register_event_callbacks(panel_io_, &io_cbs, this);

    // LVGL starts stopped: emote owns the panel until ShowLvgl().
    lvgl_port_stop();
    active_lvgl_.store(false);
    ESP_LOGI(TAG, "RenderSwitch: ready; LVGL display %p attached to emote-owned panel",
             (void*)lvgl_display_);
}

RenderSwitch::~RenderSwitch() {
    // Return the panel to emote ownership before the switch goes away. LVGL is
    // left dormant (its io fan-out is intentionally kept registered for the
    // emote renderer, so the object must outlive any subsequent emote flush).
    ShowEmote();
}

void RenderSwitch::ShowEmote() {
    // 1) Stop LVGL first so it produces no further flushes.
    lvgl_port_stop();
    // 2) Return the panel to the emote renderer.
    active_lvgl_.store(false);
    if (emote_ != nullptr) {
        emote_->SetPanelWritesEnabled(true);
    }
    ESP_LOGI(TAG, "RenderSwitch: ShowEmote -> panel owned by emote");
}

void RenderSwitch::ShowLvgl(ScreenId id, const std::function<void()>& build) {
    // 1) Gate emote's panel writes first. Emote keeps rendering into its own
    //    buffer but drops panel flushes, so no emote write can race LVGL.
    if (emote_ != nullptr) {
        emote_->SetPanelWritesEnabled(false);
    }
    active_lvgl_.store(true);

    // 2) Build the requested page under the LVGL lock. A page screen (Settings)
    //    supplies its own build callback; otherwise fall back to the solid color.
    lvgl_port_lock(-1);
    if (build) {
        build();
    } else {
        lv_obj_t* scr = lv_screen_active();
        lv_obj_remove_style_all(scr);
        lv_obj_set_style_bg_color(scr, SpikePageColor(id), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_screen_load(scr);
    }
    lvgl_port_unlock();

    // 3) Resume LVGL so it renders and flushes the page.
    lvgl_port_resume();
    ESP_LOGI(TAG, "RenderSwitch: ShowLvgl(%d) -> panel owned by LVGL", static_cast<int>(id));
}

void RenderSwitch::ShowLvgl(ScreenId id) { ShowLvgl(id, {}); }

EspVocatUi::EspVocatUi(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                       int height, emote::EmoteDisplay* emote)
    : render_switch_(panel, panel_io, width, height, emote) {
    // The panel starts owned by emote; land on the Home pet face.
    ShowHome();
}

void EspVocatUi::ShowHome() {
    render_switch_.ShowEmote();
    current_ = ScreenId::Home;
}

void EspVocatUi::ShowScreen(ScreenId id) {
    if (id == ScreenId::Settings) {
        render_switch_.ShowLvgl(id, [this]() { BuildSettingsScreen(); });
    } else if (id == ScreenId::EmotionLearning) {
        render_switch_.ShowLvgl(id, [this]() { BuildEmotionLearningScreen(); });
    } else {
        render_switch_.ShowLvgl(id);
    }
    current_ = id;
}

void EspVocatUi::SetConversationActive(bool on) {
    conversation_active_ = on;
    ESP_LOGI(TAG, "EspVocatUi: conversation overlay %s", on ? "on" : "off");
}

// ---- Settings screen -------------------------------------------------------

void EspVocatUi::SettingsStepperEventCb(lv_event_t* e) {
    auto* target = static_cast<SettingsStepTarget*>(lv_event_get_user_data(e));
    if (target == nullptr || target->ui == nullptr) {
        return;
    }
    target->ui->ApplySettingsValue(target->is_brightness, target->delta);
}

void EspVocatUi::SettingsBackEventCb(lv_event_t* e) {
    auto* ui = static_cast<EspVocatUi*>(lv_event_get_user_data(e));
    if (ui == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "EspVocatUi: settings back pressed");
    if (ui->back_to_home_cb_) {
        ui->back_to_home_cb_();
    } else {
        ui->ShowHome();
    }
}

void EspVocatUi::BuildSettingsScreen() {
    // Build the object tree once and reuse it for the device lifetime; a later
    // ShowScreen just reloads and refreshes the cached labels.
    if (settings_screen_ != nullptr) {
        lv_screen_load(settings_screen_);
        RefreshSettingsValueLabel(true);
        RefreshSettingsValueLabel(false);
        return;
    }

    // Dark iPhone-style grouped list, inset to the round 360x360 panel.
    lv_obj_t* scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title 设置.
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // Upper-left back button. '<' is guaranteed in the CJK font's Latin range.
    lv_obj_t* back = lv_button_create(scr);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x9775FA), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_size(back, 40, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_add_event_cb(back, EspVocatUi::SettingsBackEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_label, &font_puhui_20_4, 0);
    lv_obj_center(back_label);

    // Two grouped rows: 亮度 (brightness) and 音量 (volume).
    BuildSettingsRow(scr, "亮度", &brightness_value_label_, true, 84);
    BuildSettingsRow(scr, "音量", &volume_value_label_, false, 180);

    settings_screen_ = scr;
    lv_screen_load(settings_screen_);
    RefreshSettingsValueLabel(true);
    RefreshSettingsValueLabel(false);
    ESP_LOGI(TAG, "EspVocatUi: settings screen built");
}

void EspVocatUi::BuildSettingsRow(lv_obj_t* scr, const char* name, lv_obj_t** value_label_out,
                                  bool is_brightness, int y_offset) {
    // Card container.
    lv_obj_t* card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 324, 84);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2A40), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);

    // Row name.
    lv_obj_t* name_label = lv_label_create(card);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(name_label, &font_puhui_20_4, 0);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 18, 0);

    // Center value label (refreshed on change).
    lv_obj_t* value_label = lv_label_create(card);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0xFF8FB1), 0);
    lv_obj_set_style_text_font(value_label, &font_puhui_20_4, 0);
    lv_obj_align(value_label, LV_ALIGN_CENTER, 0, 0);
    *value_label_out = value_label;

    // Minus (−) stepper.
    lv_obj_t* minus = lv_button_create(card);
    lv_obj_set_size(minus, 44, 44);
    lv_obj_align(minus, LV_ALIGN_LEFT_MID, 66, 0);
    lv_obj_set_style_bg_color(minus, lv_color_hex(0x9775FA), 0);
    lv_obj_set_style_bg_opa(minus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(minus, 22, 0);
    lv_obj_add_event_cb(minus, EspVocatUi::SettingsStepperEventCb, LV_EVENT_CLICKED,
                        new SettingsStepTarget{this, is_brightness, -1});
    lv_obj_t* minus_label = lv_label_create(minus);
    lv_label_set_text(minus_label, "-");
    lv_obj_set_style_text_color(minus_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(minus_label, &font_puhui_20_4, 0);
    lv_obj_center(minus_label);

    // Plus (+) stepper.
    lv_obj_t* plus = lv_button_create(card);
    lv_obj_set_size(plus, 44, 44);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(plus, lv_color_hex(0xFF8FB1), 0);
    lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(plus, 22, 0);
    lv_obj_add_event_cb(plus, EspVocatUi::SettingsStepperEventCb, LV_EVENT_CLICKED,
                        new SettingsStepTarget{this, is_brightness, +1});
    lv_obj_t* plus_label = lv_label_create(plus);
    lv_label_set_text(plus_label, "+");
    lv_obj_set_style_text_color(plus_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(plus_label, &font_puhui_20_4, 0);
    lv_obj_center(plus_label);
}

void EspVocatUi::ApplySettingsValue(bool is_brightness, int delta) {
    int& value = is_brightness ? brightness_ : volume_;
    int new_value = value + delta;
    if (new_value < 0) {
        new_value = 0;
    }
    if (new_value > 100) {
        new_value = 100;
    }
    if (new_value == value) {
        return;  // already at the clamp; do not fire an empty change
    }
    value = new_value;
    RefreshSettingsValueLabel(is_brightness);
    if (is_brightness) {
        if (brightness_change_cb_) {
            brightness_change_cb_(value);
        }
    } else if (volume_change_cb_) {
        volume_change_cb_(value);
    }
}

void EspVocatUi::RefreshSettingsValueLabel(bool is_brightness) {
    lv_obj_t* label = is_brightness ? brightness_value_label_ : volume_value_label_;
    int value = is_brightness ? brightness_ : volume_;
    if (label == nullptr) {
        return;  // screen not built yet; SetSettingsValue* just caches
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_label_set_text(label, buf);
}

void EspVocatUi::SetSettingsValueBrightness(int value) {
    brightness_ = value < 0 ? 0 : (value > 100 ? 100 : value);
    RefreshSettingsValueLabel(true);
}

void EspVocatUi::SetSettingsValueVolume(int value) {
    volume_ = value < 0 ? 0 : (value > 100 ? 100 : value);
    RefreshSettingsValueLabel(false);
}

// ---- Emotion learning screen -----------------------------------------------

void EspVocatUi::EmotionLearningStartEventCb(lv_event_t* e) {
    auto* target = static_cast<StartLearningTarget*>(lv_event_get_user_data(e));
    if (target == nullptr || target->ui == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "EspVocatUi: emotion learning 开始 pressed");
    if (target->ui->start_learning_cb_) {
        target->ui->start_learning_cb_();
    }
}

void EspVocatUi::BuildEmotionLearningScreen() {
    // Build the object tree once and reuse it for the device lifetime; a later
    // ShowScreen just reloads and refreshes the cached labels.
    if (emotion_screen_ != nullptr) {
        lv_screen_load(emotion_screen_);
        RefreshEmotionLearningName();
        return;
    }

    // Dark iPhone-style full-card learning page, inset to the round 360x360
    // panel. No pet face; this is a pure LVGL page.
    lv_obj_t* scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title 情绪学习.
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "情绪学习");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // Upper-left back button. Reuses the shared SettingsBackEventCb, which
    // fires back_to_home_cb_ (defaulting to ShowHome) - no second back hook.
    lv_obj_t* back = lv_button_create(scr);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x9775FA), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_size(back, 40, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_add_event_cb(back, EspVocatUi::SettingsBackEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_label, &font_puhui_20_4, 0);
    lv_obj_center(back_label);

    // Central current-emotion name label (large, lavender default; a full
    // emotion->color map is deferred to a later task).
    lv_obj_t* name_label = lv_label_create(scr);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0x9775FA), 0);
    lv_obj_set_style_text_font(name_label, &font_puhui_20_4, 0);
    lv_obj_align(name_label, LV_ALIGN_CENTER, 0, -30);
    emotion_name_label_ = name_label;
    RefreshEmotionLearningName();  // apply any name cached before the screen built

    // Large 开始 button.
    lv_obj_t* start = lv_button_create(scr);
    lv_obj_set_size(start, 220, 72);
    lv_obj_align(start, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_style_bg_color(start, lv_color_hex(0xFF8FB1), 0);
    lv_obj_set_style_bg_opa(start, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(start, 36, 0);
    lv_obj_add_event_cb(start, EspVocatUi::EmotionLearningStartEventCb, LV_EVENT_CLICKED,
                        new StartLearningTarget{this});
    lv_obj_t* start_label = lv_label_create(start);
    lv_label_set_text(start_label, "开始");
    lv_obj_set_style_text_color(start_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(start_label, &font_puhui_20_4, 0);
    lv_obj_center(start_label);

    emotion_screen_ = scr;
    lv_screen_load(emotion_screen_);
    ESP_LOGI(TAG, "EspVocatUi: emotion learning screen built");
}

void EspVocatUi::SetEmotionLearningName(const char* name) {
    // Cache unconditionally; the label only exists once the screen is built.
    emotion_name_ = name != nullptr ? name : "";
    RefreshEmotionLearningName();
}

void EspVocatUi::RefreshEmotionLearningName() {
    if (emotion_name_label_ == nullptr) {
        return;  // screen not built yet; name cached in emotion_name_
    }
    lvgl_port_lock(-1);
    lv_label_set_text(emotion_name_label_, emotion_name_.c_str());
    lvgl_port_unlock();
}
