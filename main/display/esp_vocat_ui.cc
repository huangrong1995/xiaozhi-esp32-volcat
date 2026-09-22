#include "display/esp_vocat_ui.h"

#include <esp_log.h>

#include "display/emote_display.h"
#include "esp_lvgl_port.h"

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

void RenderSwitch::ShowLvgl(ScreenId id) {
    // 1) Gate emote's panel writes first. Emote keeps rendering into its own
    //    buffer but drops panel flushes, so no emote write can race LVGL.
    if (emote_ != nullptr) {
        emote_->SetPanelWritesEnabled(false);
    }
    active_lvgl_.store(true);

    // 2) Build the requested page as a solid-color screen and load it.
    lvgl_port_lock(-1);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, SpikePageColor(id), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_screen_load(scr);
    lvgl_port_unlock();

    // 3) Resume LVGL so it renders and flushes the page.
    lvgl_port_resume();
    ESP_LOGI(TAG, "RenderSwitch: ShowLvgl(%d) -> panel owned by LVGL", static_cast<int>(id));
}

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
    render_switch_.ShowLvgl(id);
    current_ = id;
}

void EspVocatUi::SetConversationActive(bool on) {
    conversation_active_ = on;
    ESP_LOGI(TAG, "EspVocatUi: conversation overlay %s", on ? "on" : "off");
}
