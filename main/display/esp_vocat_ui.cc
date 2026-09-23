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

// ===== Shared visual language (one place; applied by all three screens and the
// conversation overlay). Unified palette + rounded-corner card language tuned to
// the 360x360 round panel. Typography hierarchy (title > content > action) is
// expressed with the existing font_puhui_20_4 via color / weight / spacing -
// the OTA slot has no room for a larger CJK font, so sizes are not scaled. ====
namespace {
// Palette constants from the binding design spec (deep bg + multicolor accents).
constexpr uint32_t kDeepBg = 0x1A1A2E;
constexpr uint32_t kPink = 0xFF8FB1;
constexpr uint32_t kOrange = 0xFFA94D;
[[maybe_unused]] constexpr uint32_t kMint = 0x63E6BE;
constexpr uint32_t kLavender = 0x9775FA;
[[maybe_unused]] constexpr uint32_t kSoftYellow = 0xFFD43B;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kCardBg = 0x2A2A40;  // layered card surface on the deep bg

// Rounded-corner language matching the panel curvature.
constexpr lv_coord_t kRadiusCard = 22;           // cards
constexpr lv_coord_t kRadiusPrimaryBtn = 36;     // large accent buttons
constexpr lv_coord_t kRadiusRoundBtn = 22;       // back / ‹› / stepper buttons
constexpr lv_coord_t kRadiusBar = 4;             // waveform bars

// Screen-transition animation budget (lightweight, keeps within frame budget on
// the RGB565 round panel; deliberately not applied to the conversation overlay
// so its waveform timer is never disturbed).
constexpr uint32_t kScreenFadeMs = 200;

// Shared style objects, constructed once via EnsureSharedStyles() (LVGL v9
// pointer API), then attached to widgets with lv_obj_add_style.
lv_style_t g_style_screen_bg;    // 0x1A1A2E opaque page background
lv_style_t g_style_overlay_bg;   // translucent page background (conversation)
lv_style_t g_style_card;         // rounded, softly shadowed card
lv_style_t g_style_overlay_card; // translucent card (conversation)
lv_style_t g_style_title;        // white full-opacity heading
lv_style_t g_style_body;         // white 80% body/content
lv_style_t g_style_primary_btn;  // pink accent (主按钮 / 开始)
lv_style_t g_style_accent_btn;   // lavender accent (back / ‹› / −)
lv_style_t g_style_stepper_btn;  // pink accent stepper (＋)
lv_style_t g_style_accent_text;  // pink value / highlight text
lv_style_t g_style_emotion_name; // lavender central emotion label
lv_style_t g_style_btn_label;    // white on-button glyph

void InitSharedStyles() {
    lv_style_init(&g_style_screen_bg);
    lv_style_set_bg_color(&g_style_screen_bg, lv_color_hex(kDeepBg));
    lv_style_set_bg_opa(&g_style_screen_bg, LV_OPA_COVER);

    lv_style_init(&g_style_overlay_bg);
    lv_style_set_bg_color(&g_style_overlay_bg, lv_color_hex(kDeepBg));
    lv_style_set_bg_opa(&g_style_overlay_bg, LV_OPA_90);

    lv_style_init(&g_style_card);
    lv_style_set_bg_color(&g_style_card, lv_color_hex(kCardBg));
    lv_style_set_bg_opa(&g_style_card, LV_OPA_COVER);
    lv_style_set_radius(&g_style_card, kRadiusCard);
    lv_style_set_shadow_width(&g_style_card, 16);
    lv_style_set_shadow_opa(&g_style_card, LV_OPA_30);
    lv_style_set_shadow_color(&g_style_card, lv_color_hex(0x000000));

    lv_style_init(&g_style_overlay_card);
    lv_style_set_bg_color(&g_style_overlay_card, lv_color_hex(kCardBg));
    lv_style_set_bg_opa(&g_style_overlay_card, LV_OPA_60);
    lv_style_set_radius(&g_style_overlay_card, kRadiusCard);
    lv_style_set_shadow_width(&g_style_overlay_card, 16);
    lv_style_set_shadow_opa(&g_style_overlay_card, LV_OPA_30);
    lv_style_set_shadow_color(&g_style_overlay_card, lv_color_hex(0x000000));

    lv_style_init(&g_style_title);
    lv_style_set_text_color(&g_style_title, lv_color_hex(kWhite));
    lv_style_set_text_font(&g_style_title, &font_puhui_20_4);

    lv_style_init(&g_style_body);
    lv_style_set_text_color(&g_style_body, lv_color_hex(kWhite));
    lv_style_set_text_opa(&g_style_body, LV_OPA_80);
    lv_style_set_text_font(&g_style_body, &font_puhui_20_4);

    lv_style_init(&g_style_primary_btn);
    lv_style_set_bg_color(&g_style_primary_btn, lv_color_hex(kPink));
    lv_style_set_bg_opa(&g_style_primary_btn, LV_OPA_COVER);
    lv_style_set_radius(&g_style_primary_btn, kRadiusPrimaryBtn);

    lv_style_init(&g_style_accent_btn);
    lv_style_set_bg_color(&g_style_accent_btn, lv_color_hex(kLavender));
    lv_style_set_bg_opa(&g_style_accent_btn, LV_OPA_COVER);
    lv_style_set_radius(&g_style_accent_btn, kRadiusRoundBtn);

    lv_style_init(&g_style_stepper_btn);
    lv_style_set_bg_color(&g_style_stepper_btn, lv_color_hex(kPink));
    lv_style_set_bg_opa(&g_style_stepper_btn, LV_OPA_COVER);
    lv_style_set_radius(&g_style_stepper_btn, kRadiusRoundBtn);

    lv_style_init(&g_style_accent_text);
    lv_style_set_text_color(&g_style_accent_text, lv_color_hex(kPink));
    lv_style_set_text_font(&g_style_accent_text, &font_puhui_20_4);

    lv_style_init(&g_style_emotion_name);
    lv_style_set_text_color(&g_style_emotion_name, lv_color_hex(kLavender));
    lv_style_set_text_font(&g_style_emotion_name, &font_puhui_20_4);

    lv_style_init(&g_style_btn_label);
    lv_style_set_text_color(&g_style_btn_label, lv_color_hex(kWhite));
    lv_style_set_text_font(&g_style_btn_label, &font_puhui_20_4);
}

void EnsureSharedStyles() {
    static bool inited = false;
    if (!inited) {
        InitSharedStyles();
        inited = true;
    }
}
}  // namespace

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
    // This callback runs in the SPI ISR. It is shared by emote and esp_lvgl_port
    // for the panel's single on_color_trans_done slot.
    //
    // ALWAYS clear LVGL's pending flush first. taskLVGL is a tight loop that
    // paints the default screen as soon as add_disp creates it — i.e. before
    // active_lvgl_ is set true — and its SPI transfer relies on this callback to
    // release disp->flushing. If this were gated on active_lvgl_, that first
    // render would leave flushing latched and taskLVGL would spin in
    // wait_for_flushing forever (holding the LVGL lock, so ShowLvgl then blocks
    // and every swipe/long-press to a non-Home screen freezes). lv_disp_flush_ready
    // is idempotent and a safe no-op when LVGL is idle, so clearing it for an
    // emote-owned transfer is harmless.
    lv_display_t* lvgl_disp = self->lvgl_display_;
    if (lvgl_disp == nullptr) {
        // The very first boot render can land before add_disp's return has been
        // stored into lvgl_display_; fall back to the display being rendered.
        lvgl_disp = lv_display_get_default();
    }
    if (lvgl_disp != nullptr) {
        lvgl_port_flush_ready(lvgl_disp);
    }
    // Acknowledge emote's flush completion on EVERY color transfer, regardless of
    // which renderer owns the panel. emote_notify_flush_finished only sets the
    // gfx WAIT_FLUSH_DONE event bit (gfx_render_part_area clears it before each
    // real flush), so a spurious ack when emote has no pending flush is a
    // harmless no-op. We call it unconditionally because gfx_core pends on
    // WAIT_FLUSH_DONE with the gfx mutex held; gating the ack on active_lvgl_
    // let an emote write that was in flight across an ownership flip (SetPanelWritesEnabled
    // committed a real draw_bitmap, then active_lvgl_ turned true before its
    // ISR) go forever unacked -> gfx_core hung on the mutex -> every subsequent
    // SetEmotion (idle animation on the esp_timer task) blocked -> clock_timer
    // never fired -> the whole app froze (SystemInfo/SetEmotion logs went silent).
    if (self->emote_ != nullptr) {
        emote_notify_flush_finished(self->emote_->GetEmoteHandle());
    }
    return true;
}

RenderSwitch::RenderSwitch(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                           int width, int height, emote::EmoteDisplay* emote)
    : panel_(panel), panel_io_(panel_io), width_(width), height_(height), emote_(emote) {
    ESP_LOGI(TAG, "RenderSwitch: init LVGL on emote-owned panel");

    // Prevent emote from starting a panel transfer while esp_lvgl_port is
    // attaching LVGL to the same SPI panel. add_disp starts taskLVGL before it
    // returns, so leaving emote enabled here creates a boot-time bus race that
    // can corrupt the ST77916 address window and produce full-screen stripes.
    if (emote_ != nullptr) {
        emote_->SetPanelWritesEnabled(false);
    }

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
        // LVGL may have repainted only its damaged regions before ownership
        // changed. Force emote to redraw the complete frame so the round panel
        // cannot retain LVGL pixels as a white halo around the pet expression.
        emote_->RefreshAll();
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

    // 3) Force a full repaint of the freshly-claimed panel. The page screens are
    //    cached: on reload a cached widget tree that hasn't changed since LVGL
    //    last drew it is NOT re-invalidated by LVGL, so when ownership cycles
    //    back from emote (whose full-frame round pet face overwrote the panel)
    //    LVGL would repaint nothing and leave emote's pixels as residue ("一些
    //    区域没有刷新"). Clearing the whole active screen on every LVGL handoff
    //    ensures the new owner repaints 100% of the round panel. One-shot per
    //    transition (only the screen, not continuous full-refresh), so it does
    //    not add per-frame flush cost.
    lv_obj_invalidate(lv_screen_active());

    lvgl_port_unlock();

    // 3) Resume LVGL so it renders and flushes the page.
    lvgl_port_resume();
    ESP_LOGI(TAG, "RenderSwitch: ShowLvgl(%d) -> panel owned by LVGL", static_cast<int>(id));
}

void RenderSwitch::ShowLvgl(ScreenId id) { ShowLvgl(id, {}); }

EspVocatUi::EspVocatUi(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                       int height, emote::EmoteDisplay* emote)
    : render_switch_(panel, panel_io, width, height, emote) {
    // Build the shared style set once (RenderSwitch has already run lv_init).
    EnsureSharedStyles();
    // Bind a pointer input device to the LVGL display so the page screens'
    // on-screen controls receive taps (fed by the board's touch task). LVGL is
    // still stopped here (emote owns the panel), so creating the indev is safe.
    SetupTouchInput();
    // The panel starts owned by emote; land on the Home pet face.
    ShowHome();
}

void EspVocatUi::ShowHome() {
    render_switch_.ShowEmote();
    current_ = ScreenId::Home;
}

// Create a pointer input device so the page screens' on-screen controls can be
// clicked. The read callback returns the last point fed by the board's touch
// task (FeedTouch). LVGL polls this indev only while it owns the panel; while
// the Home emote face owns it, the LVGL task is stopped and nothing reads it.
void EspVocatUi::SetupTouchInput() {
    lv_display_t* disp = render_switch_.lvgl_display();
    if (disp == nullptr) {
        ESP_LOGE(TAG, "SetupTouchInput: no LVGL display to bind indev to");
        return;
    }
    touch_indev_ = lv_indev_create();
    lv_indev_set_type(touch_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev_, EspVocatUi::TouchInputReadCallback);
    lv_indev_set_user_data(touch_indev_, this);
    lv_indev_set_display(touch_indev_, disp);
    ESP_LOGI(TAG, "SetupTouchInput: pointer indev %p bound to display %p", (void*)touch_indev_,
             (void*)disp);
}

void EspVocatUi::TouchInputReadCallback(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<EspVocatUi*>(lv_indev_get_user_data(indev));
    if (self == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // Atomics: each read is current (seq_cst). A press/release boundary update
    // is only off by the latency of the two latch fields updating in sequence,
    // which never splits a point+state pair into a torn, unclickable one.
    data->point.x = self->touch_x_.load();
    data->point.y = self->touch_y_.load();
    data->state = self->touch_pressed_.load() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void EspVocatUi::FeedTouch(int x, int y, bool pressed) {
    touch_x_.store(x);
    touch_y_.store(y);
    touch_pressed_.store(pressed);
}

void EspVocatUi::ShowScreen(ScreenId id) {
    if (id == ScreenId::Settings) {
        render_switch_.ShowLvgl(id, [this]() { BuildSettingsScreen(); });
    } else if (id == ScreenId::EmotionLearning) {
        render_switch_.ShowLvgl(id, [this]() { BuildEmotionLearningScreen(); });
    } else if (id == ScreenId::ConversationOverlay) {
        render_switch_.ShowLvgl(id, [this]() { BuildConversationOverlayScreen(); });
    } else {
        render_switch_.ShowLvgl(id);
    }
    current_ = id;
}

void EspVocatUi::SetConversationActive(bool on) {
    if (conversation_active_ == on) {
        return;  // no state change; keep the current presentation and callback
    }
    conversation_active_ = on;
    if (on) {
        // Siri-style: the full-screen overlay replaces the Home pet face and is
        // the sole owner while a conversation is active.
        ShowScreen(ScreenId::ConversationOverlay);
        lvgl_port_lock(-1);
        if (conversation_timer_ != nullptr) {
            lv_timer_resume(conversation_timer_);
        }
        lvgl_port_unlock();
        ESP_LOGI(TAG, "EspVocatUi: conversation overlay on");
    } else {
        // Any leave returns to the Home pet face and fires the dialog-gone
        // callback so esp-vocat can force-stop the running dialogue.
        lvgl_port_lock(-1);
        if (conversation_timer_ != nullptr) {
            lv_timer_pause(conversation_timer_);
        }
        lvgl_port_unlock();
        ShowHome();
        if (dialog_gone_cb_) {
            dialog_gone_cb_();
        }
        ESP_LOGI(TAG, "EspVocatUi: conversation overlay off (dialog gone fired)");
    }
}

void EspVocatUi::SetSpeaking(bool speaking) {
    lvgl_port_lock(-1);
    speaking_ = speaking;
    ApplyConversationState();
    lvgl_port_unlock();
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
        lv_screen_load_anim(settings_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, kScreenFadeMs, 0,
                            false);
        RefreshSettingsValueLabel(true);
        RefreshSettingsValueLabel(false);
        return;
    }

    // Dark iPhone-style grouped list, inset to the round 360x360 panel. Each
    // page owns its own screen object (lv_obj_create(NULL), the LVGL 9 idiom),
    // never lv_screen_active() — otherwise pages would alias the shared default
    // screen and stack stale children across reloads.
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &g_style_screen_bg, 0);

    // Title 设置.
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "设置");
    lv_obj_add_style(title, &g_style_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // Upper-left back button. '<' is guaranteed in the CJK font's Latin range.
    lv_obj_t* back = lv_button_create(scr);
    lv_obj_add_style(back, &g_style_accent_btn, 0);
    lv_obj_set_size(back, 40, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_add_event_cb(back, EspVocatUi::SettingsBackEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_add_style(back_label, &g_style_btn_label, 0);
    lv_obj_center(back_label);

    // Two grouped rows: 亮度 (brightness) and 音量 (volume).
    BuildSettingsRow(scr, "亮度", &brightness_value_label_, true, 84);
    BuildSettingsRow(scr, "音量", &volume_value_label_, false, 180);

    settings_screen_ = scr;
    lv_screen_load_anim(settings_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, kScreenFadeMs, 0, false);
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
    lv_obj_add_style(card, &g_style_card, 0);

    // Row name (content tier: white 80%).
    lv_obj_t* name_label = lv_label_create(card);
    lv_label_set_text(name_label, name);
    lv_obj_add_style(name_label, &g_style_body, 0);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 18, 0);

    // Center value label (refreshed on change), pink accent.
    lv_obj_t* value_label = lv_label_create(card);
    lv_obj_add_style(value_label, &g_style_accent_text, 0);
    lv_obj_align(value_label, LV_ALIGN_CENTER, 0, 0);
    *value_label_out = value_label;

    // Minus (−) stepper (lavender accent).
    lv_obj_t* minus = lv_button_create(card);
    lv_obj_set_size(minus, 44, 44);
    lv_obj_align(minus, LV_ALIGN_LEFT_MID, 66, 0);
    lv_obj_add_style(minus, &g_style_accent_btn, 0);
    lv_obj_add_event_cb(minus, EspVocatUi::SettingsStepperEventCb, LV_EVENT_CLICKED,
                        new SettingsStepTarget{this, is_brightness, -1});
    lv_obj_t* minus_label = lv_label_create(minus);
    lv_label_set_text(minus_label, "-");
    lv_obj_add_style(minus_label, &g_style_btn_label, 0);
    lv_obj_center(minus_label);

    // Plus (+) stepper (pink accent).
    lv_obj_t* plus = lv_button_create(card);
    lv_obj_set_size(plus, 44, 44);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_add_style(plus, &g_style_stepper_btn, 0);
    lv_obj_add_event_cb(plus, EspVocatUi::SettingsStepperEventCb, LV_EVENT_CLICKED,
                        new SettingsStepTarget{this, is_brightness, +1});
    lv_obj_t* plus_label = lv_label_create(plus);
    lv_label_set_text(plus_label, "+");
    lv_obj_add_style(plus_label, &g_style_btn_label, 0);
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

void EspVocatUi::EmotionLearningStepEventCb(lv_event_t* e) {
    auto* target = static_cast<EmotionStepTarget*>(lv_event_get_user_data(e));
    if (target == nullptr || target->ui == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "EspVocatUi: emotion step %s pressed", target->is_next ? "next" : "prev");
    if (target->is_next && target->ui->emotion_next_cb_) {
        target->ui->emotion_next_cb_();
    } else if (!target->is_next && target->ui->emotion_prev_cb_) {
        target->ui->emotion_prev_cb_();
    }
}

void EspVocatUi::BuildEmotionLearningScreen() {
    // Build the object tree once and reuse it for the device lifetime; a later
    // ShowScreen just reloads and refreshes the cached labels.
    if (emotion_screen_ != nullptr) {
        lv_screen_load_anim(emotion_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, kScreenFadeMs, 0,
                            false);
        if (emotion_name_label_ != nullptr) {
            lv_label_set_text(emotion_name_label_, emotion_name_.c_str());
        }
        return;
    }

    // Dark iPhone-style full-card learning page, inset to the round 360x360
    // panel. No pet face; this is a pure LVGL page. Own dedicated screen object
    // (lv_obj_create(NULL)), never lv_screen_active(), so this page's children
    // never alias another page's children on the shared default screen.
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &g_style_screen_bg, 0);

    // Title 情绪学习.
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "情绪学习");
    lv_obj_add_style(title, &g_style_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // Upper-left back button. Reuses the shared SettingsBackEventCb, which
    // fires back_to_home_cb_ (defaulting to ShowHome) - no second back hook.
    lv_obj_t* back = lv_button_create(scr);
    lv_obj_add_style(back, &g_style_accent_btn, 0);
    lv_obj_set_size(back, 40, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_add_event_cb(back, EspVocatUi::SettingsBackEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_add_style(back_label, &g_style_btn_label, 0);
    lv_obj_center(back_label);

    // Left ‹ / right › round emotion-step buttons flanking the current name.
    // Each fires the board's prev/next callback (which cycles the emotion index
    // and calls SetEmotionLearningName). Round buttons avoid the panel's edge.
    lv_obj_t* prev = lv_button_create(scr);
    lv_obj_add_style(prev, &g_style_accent_btn, 0);
    lv_obj_set_size(prev, 64, 64);
    lv_obj_align(prev, LV_ALIGN_LEFT_MID, 12, -30);
    lv_obj_add_event_cb(prev, EspVocatUi::EmotionLearningStepEventCb, LV_EVENT_CLICKED,
                        new EmotionStepTarget{this, false});
    lv_obj_t* prev_label = lv_label_create(prev);
    lv_label_set_text(prev_label, "<");
    lv_obj_add_style(prev_label, &g_style_btn_label, 0);
    lv_obj_center(prev_label);

    lv_obj_t* next = lv_button_create(scr);
    lv_obj_add_style(next, &g_style_accent_btn, 0);
    lv_obj_set_size(next, 64, 64);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, -12, -30);
    lv_obj_add_event_cb(next, EspVocatUi::EmotionLearningStepEventCb, LV_EVENT_CLICKED,
                        new EmotionStepTarget{this, true});
    lv_obj_t* next_label = lv_label_create(next);
    lv_label_set_text(next_label, ">");
    lv_obj_add_style(next_label, &g_style_btn_label, 0);
    lv_obj_center(next_label);

    // Central current-emotion name label (lavender accent; a full
    // emotion->color map is deferred to a later task).
    lv_obj_t* name_label = lv_label_create(scr);
    lv_obj_add_style(name_label, &g_style_emotion_name, 0);
    lv_obj_align(name_label, LV_ALIGN_CENTER, 0, -30);
    emotion_name_label_ = name_label;
    if (!emotion_name_.empty()) {
        lv_label_set_text(emotion_name_label_, emotion_name_.c_str());
    }

    // Large 开始 button.
    lv_obj_t* start = lv_button_create(scr);
    lv_obj_set_size(start, 220, 72);
    lv_obj_align(start, LV_ALIGN_CENTER, 0, 90);
    lv_obj_add_style(start, &g_style_primary_btn, 0);
    lv_obj_add_event_cb(start, EspVocatUi::EmotionLearningStartEventCb, LV_EVENT_CLICKED,
                        new StartLearningTarget{this});
    lv_obj_t* start_label = lv_label_create(start);
    lv_label_set_text(start_label, "开始");
    lv_obj_add_style(start_label, &g_style_btn_label, 0);
    lv_obj_center(start_label);

    emotion_screen_ = scr;
    lv_screen_load_anim(emotion_screen_, LV_SCREEN_LOAD_ANIM_FADE_IN, kScreenFadeMs, 0, false);
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

// ---- Conversation overlay (Siri-style, pure LVGL, no pet face) --------------

void EspVocatUi::ConversationWaveformTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<EspVocatUi*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    self->AnimateConversationWaveform();
}

void EspVocatUi::AnimateConversationWaveform() {
    // Listening uses short, calm bars; speaking uses taller/faster bars. Purely
    // visual pseudorandom squiggle - no audio energy is sampled here.
    const int min_h = speaking_ ? 8 : 4;
    const int max_h = speaking_ ? 32 : 16;
    const uint32_t range = static_cast<uint32_t>(max_h - min_h);
    ++waveform_phase_;
    for (int i = 0; i < kWaveBarCount; ++i) {
        lv_obj_t* bar = waveform_bars_[i];
        if (bar == nullptr) {
            continue;
        }
        // Deterministic scatter so neighbouring bars differ at each tick.
        const uint32_t r = (waveform_phase_ * 2654435761u) + static_cast<uint32_t>(i) * 40321u;
        lv_obj_set_height(bar, static_cast<lv_coord_t>(min_h + (r % (range + 1))));
    }
}

void EspVocatUi::ApplyConversationState() {
    // The caller holds the LVGL lock when called from outside the LVGL task.
    if (conversation_status_label_ != nullptr) {
        lv_label_set_text(conversation_status_label_, speaking_ ? "正在说" : "正在听");
    }
    if (conversation_timer_ != nullptr) {
        // Speaking animates faster (100ms) than listening (200ms).
        lv_timer_set_period(conversation_timer_, speaking_ ? 100 : 200);
    }
}

void EspVocatUi::BuildConversationOverlayScreen() {
    // Build the object tree once and reuse it for the device lifetime; a later
    // ShowScreen just reloads and reapplies the cached speaking state.
    if (conversation_screen_ != nullptr) {
        lv_screen_load(conversation_screen_);
        ApplyConversationState();
        return;
    }

    // Full-screen translucent dark scene that entirely replaces the Home pet
    // face while talking (Siri-style). No pet face is composited here. No
    // screen-load animation is used for this overlay so the waveform timer is
    // never disturbed. Own dedicated screen object (lv_obj_create(NULL)) so the
    // overlay never aliases a page screen's children on the shared default.
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &g_style_overlay_bg, 0);

    // Translucent bottom card holding the status label and the waveform.
    lv_obj_t* card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 324, 140);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_style(card, &g_style_overlay_card, 0);

    // Status label: 正在听 (listening) / 正在说 (speaking), toggled by SetSpeaking.
    lv_obj_t* label = lv_label_create(card);
    lv_obj_add_style(label, &g_style_title, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -24);
    conversation_status_label_ = label;

    // Waveform: a row of short rounded bars whose heights the timer varies.
    const int mid = kWaveBarCount / 2;
    for (int i = 0; i < kWaveBarCount; ++i) {
        lv_obj_t* bar = lv_obj_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 8, 12);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kOrange), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, kRadiusBar, 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i - mid) * 14, -10);
        waveform_bars_[i] = bar;
    }

    // Drive the waveform with a periodic LVGL timer (starts paused; resumed by
    // SetConversationActive(true), paused again on leave).
    conversation_timer_ = lv_timer_create(EspVocatUi::ConversationWaveformTimerCb, 200, this);
    lv_timer_pause(conversation_timer_);

    conversation_screen_ = scr;
    lv_screen_load(conversation_screen_);
    ApplyConversationState();
    ESP_LOGI(TAG, "EspVocatUi: conversation overlay built");
}
