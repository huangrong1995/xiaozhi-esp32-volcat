#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "display/esp_vocat_ui.h"
#include "esp_lvgl_port.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "esp_video.h"
#include "audio/audio_service.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "power_save_timer.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include "driver/touch_sensor_common.h"
#include "esp_idf_version.h"
#include <cinttypes>
#include <string_view>

#include <driver/i2c_master.h>
#include <cstdlib>
#include <string.h>
#include "i2c_device.h"
#include "i2c_bus.h"
#include "bmi270_api.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include "esp_lcd_touch_cst816s.h"
#include "touch.h"

extern "C" {
#include "touch_button_sensor.h"
#include "touch_slider_sensor.h"
}

#include "driver/temperature_sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "ESP-VoCat"

namespace Bmi270Motion {
static bmi270_handle_t bmi_handle_ = nullptr;

esp_err_t Initialize(i2c_bus_handle_t i2c_bus)
{
    if (bmi_handle_) {
        return ESP_OK;
    }
    if (!i2c_bus) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bmi270_sensor_create(i2c_bus, &bmi_handle_, bmi270_config_file,
                                         BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (ret != ESP_OK || !bmi_handle_) {
        ESP_LOGW(TAG, "BMI270 init failed: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    const uint8_t sens_list[] = {BMI2_ACCEL};
    int8_t rslt = bmi270_sensor_enable(sens_list, 1, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMI270 accel enable failed: %d", rslt);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BMI270 initialized");
    return ESP_OK;
}

bool ReadAccelRaw(struct bmi2_sens_data& accel)
{
    if (!bmi_handle_) {
        return false;
    }
    int8_t rslt = bmi2_get_sensor_data(&accel, bmi_handle_);
    return rslt == BMI2_OK;
}
} // namespace Bmi270Motion


temperature_sensor_handle_t temp_sensor = NULL;
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};
float tsens_value;
gpio_num_t AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_1;
gpio_num_t AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_1;
gpio_num_t QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_1;
gpio_num_t TOUCH_PAD2 = TOUCH_PAD2_1;
gpio_num_t UART1_TX = UART1_TX_1;
gpio_num_t UART1_RX = UART1_RX_1;

class Charge : public I2cDevice {
public:
    Charge(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[8];
    }
    ~Charge()
    {
        delete[] read_buffer_;
    }
    void Printcharge()
    {
        ReadRegs(0x08, read_buffer_, 2);
        ReadRegs(0x0c, read_buffer_ + 2, 2);
        ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &tsens_value));

        int16_t voltage = static_cast<uint16_t>(read_buffer_[1] << 8 | read_buffer_[0]);
        int16_t current = static_cast<int16_t>(read_buffer_[3] << 8 | read_buffer_[2]);
        
        // Use the variables to avoid warnings (can be removed if actual implementation uses them)
        (void)voltage;
        (void)current;
    }
    static void TaskFunction(void *pvParameters)
    {
        Charge* charge = static_cast<Charge*>(pvParameters);
        while (true) {
            charge->Printcharge();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
};

class Cst816s : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    enum TouchEvent {
        TOUCH_NONE,
        TOUCH_PRESS,
        TOUCH_RELEASE,
        TOUCH_HOLD
    };

    Cst816s(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[6];
        was_touched_ = false;
        press_count_ = 0;

        // Create touch interrupt semaphore
        touch_isr_mux_ = xSemaphoreCreateBinary();
        if (touch_isr_mux_ == NULL) {
            ESP_LOGE(TAG, "Failed to create touch semaphore");
        }
    }

    ~Cst816s()
    {
        delete[] read_buffer_;

        // Delete semaphore if it exists
        if (touch_isr_mux_ != NULL) {
            vSemaphoreDelete(touch_isr_mux_);
            touch_isr_mux_ = NULL;
        }
    }

    void UpdateTouchPoint()
    {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t &GetTouchPoint()
    {
        return tp_;
    }

    TouchEvent CheckTouchEvent()
    {
        bool is_touched = (tp_.num > 0);
        TouchEvent event = TOUCH_NONE;

        if (is_touched && !was_touched_) {
            // Press event (transition from not touched to touched)
            press_count_++;
            event = TOUCH_PRESS;
            ESP_LOGI(TAG, "TOUCH PRESS - count: %d, x: %d, y: %d", press_count_, tp_.x, tp_.y);
        } else if (!is_touched && was_touched_) {
            // Release event (transition from touched to not touched)
            event = TOUCH_RELEASE;
            ESP_LOGI(TAG, "TOUCH RELEASE - total presses: %d", press_count_);
        } else if (is_touched && was_touched_) {
            // Continuous touch (hold)
            event = TOUCH_HOLD;
            ESP_LOGD(TAG, "TOUCH HOLD - x: %d, y: %d", tp_.x, tp_.y);
        }

        // Update previous state
        was_touched_ = is_touched;
        return event;
    }

    int GetPressCount() const
    {
        return press_count_;
    }

    void ResetPressCount()
    {
        press_count_ = 0;
    }

    // Clear the press/release tracking so the next touch reads as a fresh
    // transition. Used on light-sleep wake so a stale LCD edge cannot produce
    // an accidental tap, mode entry, drink confirmation, or chat toggle.
    void ResetTouchState()
    {
        was_touched_ = false;
        press_count_ = 0;
    }

    // Semaphore management methods
    SemaphoreHandle_t GetTouchSemaphore()
    {
        return touch_isr_mux_;
    }

    bool WaitForTouchEvent(TickType_t timeout = portMAX_DELAY)
    {
        if (touch_isr_mux_ != NULL) {
            return xSemaphoreTake(touch_isr_mux_, timeout) == pdTRUE;
        }
        return false;
    }

    void NotifyTouchEvent()
    {
        if (touch_isr_mux_ != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(touch_isr_mux_, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;

    // Touch state tracking
    bool was_touched_;
    int press_count_;

    // Touch interrupt semaphore
    SemaphoreHandle_t touch_isr_mux_;
};

// Unified interaction gesture vocabulary.
enum class Gesture {
    None,
    Tap,
    LongPress,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Pet,
    Shake,
    ChatToggle,
};

// Device interaction modes.
enum class Mode { Chat, FunctionPage, SettingsPage, EmotionLearning };

class EspVocat : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_bus_handle_t shared_i2c_bus_handle_ = nullptr;
    Cst816s* cst816s_;
    Charge* charge_;
    Button boot_button_;
    Display* display_ = nullptr;
    // SPIKE (Task 1): render-switch feasibility demo. Owns the LVGL display on
    // the same panel/io that EmoteDisplay uses and hands the panel to each.
    RenderSwitch* render_switch_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    esp_timer_handle_t touchpad_timer_;
    esp_lcd_touch_handle_t tp;   // LCD touch handle
    EspVideo* camera_ = nullptr;
    TaskHandle_t charge_task_handle_ = nullptr;
    TaskHandle_t touch_task_handle_ = nullptr;
    TaskHandle_t imu_task_handle_ = nullptr;
    TaskHandle_t touch_slider_task_handle_ = nullptr;
    esp_timer_handle_t emotion_reset_timer_ = nullptr;
    esp_timer_handle_t reminder_timer_ = nullptr;
    bool bmi270_ready_ = false;
    touch_slider_handle_t touch_slider_handle_ = nullptr;
    touch_button_handle_t touch_button_handle_ = nullptr;

    // Emotion learning: index of currently displayed emotion
    size_t current_emotion_index_ = 0;

    // Emotion learning: list of emotions to show
    static const char* kEmotionLearningEmotions_[];
    static const char* kEmotionLearningNames_[];
    static constexpr size_t kEmotionLearningCount = 8;

    // Habit tracking state (persisted fields).
    struct HabitState {
        int64_t last_drink_time;  // Unix timestamp of last drink
        int drink_streak;         // Consecutive drink completions today
    };
    HabitState habit_state_ = {};

    // Habit configuration
    static constexpr int64_t kDrinkReminderIntervalMs = 60 * 60 * 1000;  // 60 minutes in ms

    // Unified reminder record. Exactly one reminder is presented at a time; a
    // second due reminder is deferred until the active one is handled.
    struct Reminder {
        enum class Type { Drink, Schedule };
        Type type;
        const char* message;
        const char* emotion;
        std::string_view sound;
        int64_t due_time_ms;
        bool handled;  // true once acknowledged or given up on
    };

    // Unified reminder queue state. The timer only enqueues due reminders;
    // ShowNextReminder()/DismissReminder() own presentation and acknowledgement.
    Reminder reminder_ = {};
    bool reminder_active_ = false;               // a reminder is currently shown
    bool reminder_pending_ = false;              // reminder awaits resolution (shown or snoozed)
    int reminder_snooze_count_ = 0;              // ignores for the active reminder
    int64_t reminder_shown_ms_ = 0;              // when the active reminder was last shown
    int64_t reminder_quiet_until_ms_ = 0;        // suppress a drink reminder until this time
    SemaphoreHandle_t reminder_mutex_ = nullptr; // guards all reminder state
    esp_timer_handle_t reminder_restore_timer_ = nullptr; // deferred standby-restore
    bool reminder_restore_pending_ = false;               // standby restore is armed
    static constexpr uint32_t kDrinkSuccessDurationMs = 3000;  // loving face duration (ms)
    static constexpr int kReminderMaxSnooze = 3; // bounded retries before going quiet
    static constexpr int64_t kReminderAutoSnoozeMs = 60 * 1000;  // re-remind after no action

    // Daily schedule reminders (hour, minute, message, emotion).
    struct ScheduleEntry {
        int hour;
        int minute;
        const char* message;
        const char* emotion;
    };
    static const ScheduleEntry kScheduleReminders_[];
    static constexpr size_t kScheduleCount = 5;

    static constexpr const char* kDrinkReminderMessage = "该喝水了~";
    static constexpr const char* kDrinkReminderEmotion = "delicious";  // water/drink-oriented

    // Touch swipe detection for CST816S
    bool swipe_start_recorded_ = false;
    int64_t touch_press_time_ms_ = 0;
    int swipe_start_x_ = 0;
    int swipe_start_y_ = 0;
    static constexpr int kSwipeThreshold = 80;  // Minimum swipe distance

    // Outer capacitive "pet" surface feedback cooldown (moved from a hidden
    // static local so it is not hidden state).
    static constexpr int64_t kOuterTouchCooldownUs = 1200000;  // 1200 ms
    int64_t outer_touch_last_us_ = 0;

    // Settings page state. settings_selected_index_ picks the active row
    // (0 = brightness, 1 = volume); settings_adjusting_ toggles whether the
    // highlight moves or the active setting's value is adjusted live.
    int settings_selected_index_ = 0;
    bool settings_adjusting_ = false;
    int volume_ = 50;  // cached output volume (0-100); used until codec is ready

    // Display presentation owner. Higher layers take priority and are restored
    // by ResetToStandby(), so transient/mode/reminder feedback do not clobber
    // each other or the standby idle animation (spec §3.4).
    enum class Presentation {
        Standby,            // idle pet animation (lowest priority)
        Mode,               // mode presentation (function/settings/emotion-learning)
        Reminder,           // active reminder (highest priority)
        TransientFeedback,  // short touch/shake feedback
    };
    Presentation presentation_ = Presentation::Standby;

    // Unified interaction mode state. mode_ is read/written only from the
    // interaction task/callback context (touch events and timer callbacks),
    // so it does not require locking.
    Mode mode_ = Mode::Chat;
    static constexpr uint64_t kModeIdleTimeoutMs = 30000;  // Idle timeout (ms) before returning to chat
    esp_timer_handle_t mode_idle_timer_ = nullptr;

    // Two-level power save (spec §6.1). Level 1 is a 1s tick that turns off the
    // display after DISPLAY_SLEEP_TIMEOUT_SECONDS of eligible idle while keeping
    // the wake word available. Level 2 reuses the common PowerSaveTimer for a
    // light sleep after LIGHT_SLEEP_TIMEOUT_SECONDS, gated by UpdatePowerSaveEligibility().
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_timer_handle_t display_sleep_timer_ = nullptr;
    int display_idle_ticks_ = 0;      // seconds since the last eligible interaction
    bool display_sleep_active_ = false;  // level-1 display sleep is on
    static constexpr int kDisplaySleepTimeoutSeconds = DISPLAY_SLEEP_TIMEOUT_SECONDS;
    static constexpr int kLightSleepTimeoutSeconds = LIGHT_SLEEP_TIMEOUT_SECONDS;

    static void mode_idle_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr || self->mode_ == Mode::Chat || self->reminder_active_) {
            return;
        }
        self->ExitToChat();
    }

    void OnGesture(Gesture gesture)
    {
        // Any normalized gesture is interaction: reset both power-save countdowns
        // and wake the display if it had fallen to level-1 sleep (spec §6.2).
        TouchPowerSaveActivity();
        ResetModeIdleTimer();
        // Reminder-first routing: while a reminder is active, its acknowledgement
        // owns all screen gestures; mode navigation is blocked until handled.
        if (reminder_active_) {
            HandleReminderGesture(gesture);
            return;
        }
        switch (mode_) {
        case Mode::Chat:
            HandleChatGesture(gesture);
            break;
        case Mode::FunctionPage:
            HandleFunctionPageGesture(gesture);
            break;
        case Mode::SettingsPage:
            HandleSettingsPageGesture(gesture);
            break;
        case Mode::EmotionLearning:
            HandleEmotionLearningGesture(gesture);
            break;
        }
    }

    void EnterMode(Mode mode)
    {
        if (mode_ == mode) {
            return;
        }
        mode_ = mode;
        ResetModeIdleTimer();
        // A mode is a high-priority presentation: it must not light-sleep (spec §6.1).
        UpdatePowerSaveEligibility();
    }

    void ExitToChat()
    {
        if (mode_ == Mode::Chat) {
            return;
        }
        mode_ = Mode::Chat;
        if (mode_idle_timer_ != nullptr) {
            esp_timer_stop(mode_idle_timer_);
        }
        // Back in the root Chat state: re-allow light sleep if no reminder is up.
        UpdatePowerSaveEligibility();
        // Return to the standby idle presentation (Chat → idle pet animation).
        ResetToStandby();
    }

    void ResetModeIdleTimer()
    {
        if (mode_idle_timer_ == nullptr) {
            return;
        }
        esp_timer_stop(mode_idle_timer_);
        esp_timer_start_once(mode_idle_timer_, kModeIdleTimeoutMs * 1000ULL);
    }

    // Return the display to the presentation appropriate for the current mode:
    // the idle pet animation in Chat, the current emotion/pose inside a mode.
    // Stops any transient animation and cannot permanently overwrite a mode
    // or reminder presentation, so feedback layers stay isolated (spec §3.4).
    void ResetToStandby()
    {
        if (display_ == nullptr) {
            return;
        }
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
        }
        auto* emote = static_cast<emote::EmoteDisplay*>(display_);
        switch (mode_) {
        case Mode::Chat:
            presentation_ = Presentation::Standby;
            emote->RestoreFromReminder();  // restarts the standby idle animation
            break;
        case Mode::EmotionLearning:
            presentation_ = Presentation::Mode;
            ShowEmotionLearningCurrent();
            break;
        case Mode::FunctionPage:
            presentation_ = Presentation::Mode;
            emote->ShowFunctionPage();
            break;
        case Mode::SettingsPage:
            presentation_ = Presentation::Mode;
            emote->ShowSettingsPage(settings_selected_index_, settings_adjusting_,
                                    CurrentSettingsValue());
            break;
        }
    }

    // Per-mode gesture handlers. Note: Pet and Shake must never change mode_.
    void HandleChatGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::SwipeLeft:
            EnterFunctionPage();
            break;
        case Gesture::SwipeRight:
            EnterSettingsPage();
            break;
        case Gesture::LongPress:
            Application::GetInstance().ToggleChatState();
            break;
        case Gesture::Tap:
            // Light touch feedback without leaving Chat.
            ShowTemporaryEmotion("happy", 1500);
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
            break;
        case Gesture::Shake:
            // "被摇晃" feedback: shake is meaningful only in Chat (spec §1.3).
            // Other modes' handlers ignore Shake, so it never advances a mode.
            ShowTemporaryEmotion("confused", 1800);
            ESP_LOGI(TAG, "Shake feedback (Chat mode)");
            break;
        default:
            // Ignore mode-only gestures (SwipeUp, SwipeDown, Pet, Shake,
            // ChatToggle, None). A swipe must never toggle chat.
            break;
        }
    }

    void HandleFunctionPageGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::SwipeUp:
            ExitToChat();
            break;
        case Gesture::LongPress:
            EnterEmotionLearningMode();
            break;
        case Gesture::Tap:
            // Light touch feedback.
            ShowTemporaryEmotion("happy", 1500);
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
            break;
        default:
            break;
        }
    }

    void HandleSettingsPageGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::SwipeUp:
            ExitToChat();
            break;
        case Gesture::SwipeLeft:
        case Gesture::SwipeRight:
            if (!settings_adjusting_) {
                // Move the highlight; with two settings rows, either swipe
                // toggles between brightness and volume.
                settings_selected_index_ = (settings_selected_index_ + 1) % 2;
                RenderSettingsPage();
            } else {
                // Adjust the active setting's value live (brightness or volume).
                int delta = (gesture == Gesture::SwipeRight) ? 5 : -5;
                AdjustSettingsValue(delta);
            }
            break;
        case Gesture::LongPress:
            settings_adjusting_ = !settings_adjusting_;
            RenderSettingsPage();
            break;
        case Gesture::Tap:
            // Light touch feedback.
            ShowTemporaryEmotion("happy", 1500);
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
            break;
        default:
            break;
        }
    }

    void HandleEmotionLearningGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::SwipeUp:
            ExitToChat();
            break;
        case Gesture::SwipeLeft:  // previous emotion
            current_emotion_index_ =
                (current_emotion_index_ + kEmotionLearningCount - 1) % kEmotionLearningCount;
            ShowEmotionLearningCurrent();
            break;
        case Gesture::SwipeRight:  // next emotion
            ShowEmotionLearningNext();
            break;
        case Gesture::Tap:  // feedback for the current emotion
            ShowEmotionLearningCurrent();
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
            break;
        case Gesture::LongPress:
            // Leave learning mode and start a conversation.
            ExitEmotionLearningMode();
            Application::GetInstance().ToggleChatState();
            break;
        default:
            break;
        }
    }

    // Gesture timing thresholds (ms)
    static constexpr int64_t kLongPressThresholdMs = 1200;

    static void emotion_reset_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        // Return to the presentation appropriate for the current mode instead of
        // blindly resetting to "neutral", which would wipe a mode/reminder state.
        if (self != nullptr) {
            self->ResetToStandby();
        }
    }

    void LoadHabitState()
    {
        Settings s("habit", false);
        habit_state_.last_drink_time = s.GetInt("last_drink", 0);
        habit_state_.drink_streak = s.GetInt("drink_streak", 0);
        ESP_LOGI(TAG, "Habit state loaded: last_drink=%" PRId64 ", streak=%d",
                  habit_state_.last_drink_time, habit_state_.drink_streak);
    }

    void SaveHabitState()
    {
        Settings s("habit", true);
        s.SetInt("last_drink", habit_state_.last_drink_time);
        s.SetInt("drink_streak", habit_state_.drink_streak);
        ESP_LOGI(TAG, "Habit state saved: last_drink=%" PRId64 ", streak=%d",
                  habit_state_.last_drink_time, habit_state_.drink_streak);
    }

    void MarkDrinkCompleted()
    {
        int64_t now_ms = esp_timer_get_time() / 1000;
        habit_state_.last_drink_time = now_ms;
        habit_state_.drink_streak++;
        SaveHabitState();

        ESP_LOGI(TAG, "Drink completed! Streak: %d", habit_state_.drink_streak);
        ShowTemporaryEmotion("loving", 3000);
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
    }

    void LockReminder()
    {
        if (reminder_mutex_ != nullptr) {
            xSemaphoreTake(reminder_mutex_, portMAX_DELAY);
        }
    }

    void UnlockReminder()
    {
        if (reminder_mutex_ != nullptr) {
            xSemaphoreGive(reminder_mutex_);
        }
    }

    // Cancel a pending deferred standby-restore. Callers must hold reminder_mutex_.
    void CancelReminderRestore()
    {
        reminder_restore_pending_ = false;
        if (reminder_restore_timer_ != nullptr) {
            esp_timer_stop(reminder_restore_timer_);
        }
    }

    // Schedule a one-shot deferred restore to the standby idle presentation.
    // Callers must hold reminder_mutex_.
    void ArmReminderRestore(uint32_t delay_ms)
    {
        reminder_restore_pending_ = true;
        if (reminder_restore_timer_ != nullptr) {
            esp_timer_stop(reminder_restore_timer_);
            esp_timer_start_once(reminder_restore_timer_,
                                 static_cast<uint64_t>(delay_ms) * 1000ULL);
        }
    }

    // Deferred standby-restore used after drink confirmation, so the display
    // returns to its presentation once the ~3s success face has been shown.
    // ResetToStandby() restores based on the current mode, so a mode change
    // made while the restore was pending is not clobbered by the idle animation.
    void RestoreFromReminderDeferred()
    {
        LockReminder();
        if (reminder_restore_pending_ && display_ != nullptr) {
            reminder_restore_pending_ = false;
            ResetToStandby();
        }
        UnlockReminder();
    }

    static void reminder_restore_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr) {
            return;
        }
        self->RestoreFromReminderDeferred();
    }

    // Non-blocking reminder presentation. Callers must hold reminder_mutex_.
    void ShowNextReminder()
    {
        if (!reminder_active_ || display_ == nullptr) {
            return;
        }
        // A due reminder is interaction: wake the display so it is actually
        // visible even if it had fallen to level-1 display sleep.
        TouchPowerSaveActivity();
        // A newly shown reminder supersedes any pending deferred standby-restore.
        CancelReminderRestore();
        presentation_ = Presentation::Reminder;
        static_cast<emote::EmoteDisplay*>(display_)->ShowReminder(reminder_.emotion);
        Application::GetInstance().GetAudioService().PlaySound(reminder_.sound);
        ESP_LOGI(TAG, "Reminder active: %s (%s)",
                 reminder_.message ? reminder_.message : "drink", reminder_.emotion);
    }

    // Callers must hold reminder_mutex_.
    void DismissReminder()
    {
        if (!reminder_active_) {
            return;
        }
        reminder_active_ = false;
        // A reminder must not light-sleep while shown; re-allow it once resolved.
        UpdatePowerSaveEligibility();
        ResetToStandby();
    }

    // Callers must hold reminder_mutex_.
    void PresentReminder(Reminder::Type type, const char* message, const char* emotion,
                         std::string_view sound, bool reset_snooze)
    {
        reminder_ = Reminder{type, message, emotion, sound, esp_timer_get_time() / 1000, false};
        if (reset_snooze) {
            reminder_snooze_count_ = 0;
        }
        reminder_active_ = true;
        reminder_pending_ = true;
        reminder_shown_ms_ = esp_timer_get_time() / 1000;
        // A reminder is a high-priority presentation: suspend light sleep while
        // it is shown (spec §6.1).
        UpdatePowerSaveEligibility();
        ShowNextReminder();
    }

    // Detect and enqueue due reminders. Presentation and acknowledgement are
    // serialized through reminder_mutex_ so the timer task and the touch task
    // cannot race on the reminder fields.
    void EnqueueDueReminders()
    {
        LockReminder();
        int64_t now_ms = esp_timer_get_time() / 1000;

        // Auto-snooze an unacknowledged reminder after a delay (bounded retry),
        // so an ignored reminder eventually goes quiet.
        if (reminder_active_ && (now_ms - reminder_shown_ms_) >= kReminderAutoSnoozeMs) {
            reminder_snooze_count_++;
            ESP_LOGI(TAG, "Reminder auto-snoozed (%d/%d)",
                     reminder_snooze_count_, kReminderMaxSnooze);
            if (reminder_snooze_count_ >= kReminderMaxSnooze) {
                reminder_.handled = true;
                reminder_pending_ = false;
                reminder_quiet_until_ms_ = now_ms + kDrinkReminderIntervalMs;
            }
            DismissReminder();
            UnlockReminder();
            return;
        }
        if (reminder_active_) {
            // One reminder is shown; a newly due reminder is deferred until this
            // one is handled.
            UnlockReminder();
            return;
        }

        // Re-present a snoozed (unhandled) drink reminder without resetting the
        // snooze counter. It stays pending until acknowledged or given up, so an
        // ignored reminder reaches kReminderMaxSnooze and goes quiet.
        if (reminder_pending_ && reminder_.type == Reminder::Type::Drink) {
            reminder_active_ = true;
            reminder_shown_ms_ = now_ms;
            ShowNextReminder();
            UnlockReminder();
            return;
        }

        // Fresh drink reminder: only after a resolved/quiet cycle. The counter
        // resets here, on a genuinely new interval.
        bool drink_due = (habit_state_.last_drink_time == 0 ||
                          (now_ms - habit_state_.last_drink_time) >= kDrinkReminderIntervalMs);
        if (drink_due && now_ms >= reminder_quiet_until_ms_) {
            PresentReminder(Reminder::Type::Drink, kDrinkReminderMessage, kDrinkReminderEmotion,
                            Lang::Sounds::OGG_POPUP, true);
            UnlockReminder();
            return;
        }

        // Daily schedule reminders.
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        for (size_t i = 0; i < kScheduleCount; ++i) {
            const ScheduleEntry& r = kScheduleReminders_[i];
            if (timeinfo.tm_hour == r.hour && timeinfo.tm_min == r.minute) {
                PresentReminder(Reminder::Type::Schedule, r.message, r.emotion,
                                Lang::Sounds::OGG_POPUP, true);
                UnlockReminder();
                return;
            }
        }
        UnlockReminder();
    }

    static void reminder_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr) {
            return;
        }
        // The timer only enqueues; presentation and acknowledgement are owned by
        // the reminder path (ShowNextReminder/DismissReminder via gestures), all
        // serialized through reminder_mutex_.
        self->EnqueueDueReminders();
    }

    void HandleReminderGesture(Gesture gesture)
    {
        LockReminder();
        switch (gesture) {
        case Gesture::Tap:
            if (reminder_.type == Reminder::Type::Drink) {
                // Screen tap confirms "done drinking". MarkDrinkCompleted plays the
                // success feedback and leaves the success face up for its ~3s
                // (emotion_reset_timer returns to neutral), so do NOT force an
                // idle restore on top of it.
                MarkDrinkCompleted();
                reminder_.handled = true;
                reminder_pending_ = false;
                reminder_active_ = false;
                // Return to the standby idle animation after the ~3s success face,
                // so idle pet animation resumes instead of freezing on neutral.
                ArmReminderRestore(kDrinkSuccessDurationMs);
            } else {
                // Schedule acknowledgement: feedback only, no habit change.
                Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
                reminder_.handled = true;
                reminder_pending_ = false;
                DismissReminder();
            }
            break;
        case Gesture::LongPress:
            // Ignore/snooze the reminder; bounded retry, then quiet.
            reminder_snooze_count_++;
            ESP_LOGI(TAG, "Reminder ignored (%d/%d)",
                     reminder_snooze_count_, kReminderMaxSnooze);
            if (reminder_snooze_count_ >= kReminderMaxSnooze) {
                reminder_.handled = true;
                reminder_pending_ = false;
                reminder_quiet_until_ms_ = esp_timer_get_time() / 1000 + kDrinkReminderIntervalMs;
                ESP_LOGI(TAG, "Reminder given up after %d ignores; quiet", reminder_snooze_count_);
            }
            DismissReminder();
            break;
        default:
            // Mode navigation (swipes) is blocked while a reminder is active.
            break;
        }
        UnlockReminder();
    }

    void InitializeReminderTimer()
    {
        esp_timer_create_args_t timer_args = {
            .callback = &reminder_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "reminder",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&timer_args, &reminder_timer_);
        // Check every minute (60 seconds = 60000000 us)
        esp_timer_start_periodic(reminder_timer_, 60 * 1000000);
        ESP_LOGI(TAG, "Reminder timer started");
    }

    void ShowTemporaryEmotion(const char* emotion, uint32_t duration_ms)
    {
        if (display_ == nullptr || emotion == nullptr) {
            return;
        }
        presentation_ = Presentation::TransientFeedback;
        display_->SetEmotion(emotion);
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_start_once(emotion_reset_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL);
        }
    }

    // Outer capacitive "pet" surface: feedback-only touch. A single recognized
    // touch is routed as Gesture::Pet once per debounced interaction (cooldown
    // in outer_touch_last_us_). Pet never changes mode, toggles chat, advances
    // an emotion, or records a drink. (Waking the device is handled separately.)
    void HandleOuterTouchPet()
    {
        const int64_t now = esp_timer_get_time();
        if ((now - outer_touch_last_us_) < kOuterTouchCooldownUs) {
            return;
        }
        outer_touch_last_us_ = now;
        OnGesture(Gesture::Pet);
        // While a reminder is active, it is the highest-priority presentation
        // (spec §3.4): suppress pet feedback so it does not overwrite it.
        if (!reminder_active_) {
            ShowTemporaryEmotion("happy", 2000);
        }
    }

    // ---- Two-level power save (spec §6.1 / §6.2) ------------------------------

    // Called on every interaction (gesture, BOOT, reminder, mode entry, audio):
    // reset both power-save countdowns and wake the display from level-1 sleep.
    void TouchPowerSaveActivity()
    {
        display_idle_ticks_ = 0;
        if (power_save_timer_ != nullptr) {
            // Resets its count and, if the chip woke from a light sleep, exits
            // sleep mode (restoring wake word, CPU config, and the display).
            power_save_timer_->WakeUp();
        }
        WakeDisplay();
    }

    // Level-1: turn off the display and backlight. Wake word/audio stay on.
    void EnterDisplayPowerSave()
    {
        if (display_sleep_active_) {
            return;
        }
        display_sleep_active_ = true;
        if (display_ != nullptr) {
            display_->SetPowerSaveMode(true);
        }
        if (backlight_ != nullptr) {
            backlight_->SetBrightness(0);
        }
        ESP_LOGI(TAG, "Display sleep after %ds idle (wake word stays on)",
                 kDisplaySleepTimeoutSeconds);
    }

    // Restore the backlight and the presentation for the current mode.
    void WakeDisplay()
    {
        if (!display_sleep_active_) {
            return;
        }
        display_sleep_active_ = false;
        if (backlight_ != nullptr) {
            backlight_->RestoreBrightness();
        }
        if (display_ != nullptr) {
            display_->SetPowerSaveMode(false);
        }
        ResetToStandby();
        ESP_LOGI(TAG, "Display woken");
    }

    // Arm the wake sources for level-2 light sleep. The outer capacitive pad is
    // the preferred wake source (spec §6.2): esp_sleep_enable_touchpad_wakeup()
    // uses the TOUCH_PAD1/TOUCH_PAD2 channels the cap sensor already configured.
    void ConfigureWakeSources()
    {
        esp_sleep_enable_touchpad_wakeup();
#if VOCAT_ENABLE_GPIO_WAKEUP
        // Fallback wake sources: LCD touch INT (GPIO10) and BOOT (GPIO0), both
        // active-low. ESP32-S3 light-sleep GPIO wake is level based and each pin
        // is armed individually with gpio_wakeup_enable(). Left off until the
        // board's polarity and wake capability are confirmed on hardware.
        gpio_wakeup_enable(TP_PIN_NUM_INT, GPIO_WAKEUP_LOW);
        gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_WAKEUP_LOW);
        esp_sleep_enable_gpio_wakeup();
#endif
    }

    // Step 5: after a light-sleep wake, the first edge can be stale. Reset the
    // LCD touch tracking so the wake touch cannot yield an accidental tap, mode
    // entry, drink confirmation, or chat toggle, and discard any latched cap-pad
    // wake edge. Safe to call from the light-sleep exit path (never from inside
    // a cap-sensor callback). The cap surface only ever yields a feedback-only
    // Pet, so its own edges are benign.
    void ClearStaleWakeTouch()
    {
        if (cst816s_ != nullptr) {
            cst816s_->ResetTouchState();
        }
        swipe_start_recorded_ = false;
        touch_pad_clear_status();
    }

    // Gate level-2 light sleep to the root Chat mode outside a reminder; the
    // PowerSaveTimer itself additionally requires Application::CanEnterSleepMode().
    void UpdatePowerSaveEligibility()
    {
        if (power_save_timer_ == nullptr) {
            return;
        }
        bool eligible = (mode_ == Mode::Chat) && !reminder_active_;
        power_save_timer_->SetEnabled(eligible);
    }

    static void display_sleep_tick_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self != nullptr) {
            self->OnDisplaySleepTick();
        }
    }

    // Level-1 countdown. Only the root Chat mode, with no active reminder and a
    // standby presentation, and only when the application is genuinely idle
    // (kDeviceStateIdle, no audio/protocol activity), may sleep the display.
    void OnDisplaySleepTick()
    {
        if (display_sleep_active_) {
            return;  // already at level 1
        }
        auto& app = Application::GetInstance();
        bool eligible = (mode_ == Mode::Chat) && !reminder_active_ &&
                        (presentation_ == Presentation::Standby) && app.CanEnterSleepMode();
        if (!eligible) {
            display_idle_ticks_ = 0;
            return;
        }
        display_idle_ticks_++;
        if (display_idle_ticks_ >= kDisplaySleepTimeoutSeconds) {
            EnterDisplayPowerSave();
        }
    }

    void InitializePowerSave()
    {
        // Level-2 light sleep via the common PowerSaveTimer. A real cpu_max_freq
        // makes it lower the CPU and (when CONFIG_PM_ENABLE is set) light-sleep,
        // disabling wake-word/audio input through its existing helper behaviour.
        power_save_timer_ = new PowerSaveTimer(240, kLightSleepTimeoutSeconds, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Entering light sleep (level 2)");
            // Build on level 1: ensure the display/backlight are off.
            display_sleep_active_ = true;
            if (display_ != nullptr) {
                display_->SetPowerSaveMode(true);
            }
            if (backlight_ != nullptr) {
                backlight_->SetBrightness(0);
            }
            ConfigureWakeSources();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Exiting light sleep (woke)");
            ClearStaleWakeTouch();
            // Re-enable the audio codec input, mirroring the level-2 enter
            // path that disabled it (spec §6.2).
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr) {
                codec->EnableInput(true);
            }
            WakeDisplay();
        });

        // Level-1 display sleep: separate 1s periodic timer, wake word stays on.
        esp_timer_create_args_t sleep_timer_args = {
            .callback = &EspVocat::display_sleep_tick_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sleep_1s",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&sleep_timer_args, &display_sleep_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(display_sleep_timer_, 1000000));

        // Begin counting to light sleep; Chat root with no reminder is eligible.
        UpdatePowerSaveEligibility();
        if (power_save_timer_ != nullptr) {
            power_save_timer_->SetEnabled(true);
        }
    }

    static void imu_event_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr || !self->bmi270_ready_) {
            vTaskDelete(NULL);
            return;
        }

        struct bmi2_sens_data prev = {};
        struct bmi2_sens_data cur = {};
        bool has_prev = false;
        int64_t last_shake_ms = 0;
        constexpr int kShakeDeltaThreshold = 20000;
        constexpr int64_t kShakeCooldownMs = 2000;

        while (true) {
            if (Bmi270Motion::ReadAccelRaw(cur)) {
                if (has_prev) {
                    int dx = abs(static_cast<int>(cur.acc.x) - static_cast<int>(prev.acc.x));
                    int dy = abs(static_cast<int>(cur.acc.y) - static_cast<int>(prev.acc.y));
                    int dz = abs(static_cast<int>(cur.acc.z) - static_cast<int>(prev.acc.z));
                    int shake_score = dx + dy + dz;

                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (shake_score > kShakeDeltaThreshold && (now_ms - last_shake_ms) > kShakeCooldownMs) {
                        last_shake_ms = now_ms;
                        // Shake is a mode-aware gesture: OnGesture() resets the idle
                        // timer and only Chat handles it (spec §1.3/§2.1), so a shake
                        // never silently extends a mode.
                        self->OnGesture(Gesture::Shake);
                    }
                }
                prev = cur;
                has_prev = true;
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }

    void InitializeI2c()
    {
        i2c_config_t i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master = {
                .clk_speed = 400000,
            },
            .clk_flags = 0,
        };
        shared_i2c_bus_handle_ = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
        if (!shared_i2c_bus_handle_) {
            ESP_LOGE(TAG, "Failed to create shared I2C bus");
            ESP_ERROR_CHECK(ESP_FAIL);
        }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && !CONFIG_I2C_BUS_BACKWARD_CONFIG
        i2c_bus_ = i2c_bus_get_internal_bus_handle(shared_i2c_bus_handle_);
#else
#error "ESP-VoCat board requires i2c_bus_get_internal_bus_handle() support"
#endif
        if (!i2c_bus_) {
            ESP_LOGE(TAG, "Failed to get I2C master handle");
            ESP_ERROR_CHECK(ESP_FAIL);
        }

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
    }
    uint8_t DetectPcbVersion()
        {
            gpio_config_t gpio_conf = {
                .pin_bit_mask = (1ULL << CORDEC_POWER_CTRL),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            ESP_ERROR_CHECK(gpio_config(&gpio_conf));
            ESP_ERROR_CHECK(gpio_set_level(CORDEC_POWER_CTRL, 0));
            vTaskDelay(pdMS_TO_TICKS(50));

            bool codec_alive = (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK);
            uint8_t pcb_version = 0;
            if (codec_alive) {
                ESP_LOGI(TAG, "PCB version V1.0");
                pcb_version = 0;
            } else {
                ESP_ERROR_CHECK(gpio_set_level(CORDEC_POWER_CTRL, 1));
                vTaskDelay(pdMS_TO_TICKS(50));
                codec_alive = (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK);
                if (codec_alive) {
                    ESP_LOGI(TAG, "PCB version V1.2");
                    pcb_version = 1;
                    AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_2;
                    AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_2;
                    QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_2;
                    TOUCH_PAD2 = TOUCH_PAD2_2;
                    UART1_TX = UART1_TX_2;
                    UART1_RX = UART1_RX_2;
                } else {
                    ESP_LOGE(TAG, "PCB version detection error");
                }
            }
            return pcb_version;
        }

    static void touch_isr_callback(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad != nullptr) {
            touchpad->NotifyTouchEvent();
        }
    }

    static void touch_event_task(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad == nullptr) {
            ESP_LOGE(TAG, "Invalid touchpad pointer in touch_event_task");
            vTaskDelete(NULL);
            return;
        }

        while (true) {
            if (touchpad->WaitForTouchEvent()) {
                auto &app = Application::GetInstance();
                auto &board = (EspVocat &)Board::GetInstance();

                ESP_LOGD(TAG, "Touch event, TP_PIN_NUM_INT: %d", gpio_get_level(TP_PIN_NUM_INT));
                touchpad->UpdateTouchPoint();
                auto touch_event = touchpad->CheckTouchEvent();
                auto& touch_point = touchpad->GetTouchPoint();

                if (touch_event == Cst816s::TOUCH_PRESS) {
                    // Record press timestamp and start position for later recognition
                    board.touch_press_time_ms_ = esp_timer_get_time() / 1000;
                    board.swipe_start_x_ = touch_point.x;
                    board.swipe_start_y_ = touch_point.y;
                    board.swipe_start_recorded_ = true;
                }

                if (touch_event == Cst816s::TOUCH_RELEASE) {
                    // Startup special case: first touch release enters WiFi config.
                    if (app.GetDeviceState() == kDeviceStateStarting) {
                        board.swipe_start_recorded_ = false;
                        board.EnterWifiConfigMode();
                        continue;
                    }

                    // Normalize the release into a single screen gesture and route it.
                    Gesture gesture = Gesture::Tap;
                    if (board.swipe_start_recorded_) {
                        int64_t now_ms = esp_timer_get_time() / 1000;
                        int64_t duration_ms = now_ms - board.touch_press_time_ms_;
                        int delta_x = touch_point.x - board.swipe_start_x_;
                        int delta_y = touch_point.y - board.swipe_start_y_;
                        ESP_LOGI(TAG, "Touch release: start(%d,%d) end(%d,%d) delta(%d,%d) dur=%" PRId64 " ms",
                                  board.swipe_start_x_, board.swipe_start_y_,
                                  touch_point.x, touch_point.y, delta_x, delta_y, duration_ms);

                        if (duration_ms >= kLongPressThresholdMs) {
                            gesture = Gesture::LongPress;
                        } else if (abs(delta_x) >= kSwipeThreshold && abs(delta_x) > abs(delta_y)) {
                            gesture = (delta_x > 0) ? Gesture::SwipeRight : Gesture::SwipeLeft;
                        } else if (abs(delta_y) >= kSwipeThreshold && abs(delta_y) > abs(delta_x)) {
                            gesture = (delta_y > 0) ? Gesture::SwipeDown : Gesture::SwipeUp;
                        } else {
                            gesture = Gesture::Tap;
                        }
                        board.swipe_start_recorded_ = false;
                    }
                    board.OnGesture(gesture);
                }
            }
        }
    }

    void InitializeCharge()
    {
        charge_ = new Charge(i2c_bus_, 0x55);
        xTaskCreatePinnedToCore(Charge::TaskFunction, "batterydecTask", 3 * 1024, charge_, 6, &charge_task_handle_, 0);
    }

    void InitializeCst816sTouchPad()
    {
        cst816s_ = new Cst816s(i2c_bus_, 0x15);

        xTaskCreatePinnedToCore(touch_event_task, "touch_task", 4 * 1024, cst816s_, 5, &touch_task_handle_, 1);

        const gpio_config_t int_gpio_config = {
            .pin_bit_mask = (1ULL << TP_PIN_NUM_INT),
            .mode = GPIO_MODE_INPUT,
            // .intr_type = GPIO_INTR_NEGEDGE
            .intr_type = GPIO_INTR_ANYEDGE
        };
        gpio_config(&int_gpio_config);
        gpio_install_isr_service(0);
        gpio_intr_enable(TP_PIN_NUM_INT);
        gpio_isr_handler_add(TP_PIN_NUM_INT, EspVocat::touch_isr_callback, cst816s_);
    }

    void InitializeBmi270()
    {
        esp_err_t imu_ret = Bmi270Motion::Initialize(shared_i2c_bus_handle_);
        if (imu_ret == ESP_OK) {
            bmi270_ready_ = true;
            xTaskCreatePinnedToCore(imu_event_task, "imu_task", 4 * 1024, this, 4, &imu_task_handle_, 1);
        } else {
            ESP_LOGW(TAG, "BMI270 unavailable, shake emotion disabled");
        }
    }

    static uint32_t TouchChannelFromPadGpio(gpio_num_t gpio)
    {
        if (gpio == GPIO_NUM_NC) {
            return 0;
        }
        if (gpio >= GPIO_NUM_1 && gpio <= GPIO_NUM_14) {
            return static_cast<uint32_t>(gpio);
        }
        return 0;
    }

    static void touch_slider_event_callback(touch_slider_handle_t handle, touch_slider_event_t event, int32_t data, void* cb_arg)
    {
        (void)handle;
        auto* self = static_cast<EspVocat*>(cb_arg);
        if (self == nullptr || self->display_ == nullptr) {
            return;
        }
        if (event != TOUCH_SLIDER_EVENT_POSITION) {
            ESP_LOGI(TAG, "Touch slider evt=%d data=%" PRId32, static_cast<int>(event), data);
        }

        bool gesture = false;
        if (event == TOUCH_SLIDER_EVENT_LEFT_SWIPE || event == TOUCH_SLIDER_EVENT_RIGHT_SWIPE) {
            gesture = true;
        } else if (event == TOUCH_SLIDER_EVENT_RELEASE) {
            gesture = true;
        }

        if (!gesture) {
            return;
        }

        // The outer capacitive surface is a feedback-only "pet" input.
        self->HandleOuterTouchPet();
    }

    void EnterEmotionLearningMode()
    {
        if (mode_ == Mode::EmotionLearning) {
            return;
        }
        EnterMode(Mode::EmotionLearning);
        current_emotion_index_ = 0;
        ShowEmotionLearningCurrent();
        // Play sound to indicate entering learning mode
        auto& audio = Application::GetInstance().GetAudioService();
        audio.PlaySound(Lang::Sounds::OGG_POPUP);
        vTaskDelay(pdMS_TO_TICKS(150));
        audio.PlaySound(Lang::Sounds::OGG_SUCCESS);
        ESP_LOGI(TAG, "Entered emotion learning mode");
    }

    void ExitEmotionLearningMode()
    {
        if (mode_ != Mode::EmotionLearning) {
            return;
        }
        ExitToChat();
        ShowTemporaryEmotion("happy", 2000);
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
        ESP_LOGI(TAG, "Exited emotion learning mode");
    }

    void ShowEmotionLearningCurrent()
    {
        if (display_ != nullptr) {
            // Emotion-learning is a Mode presentation: it must not be clobbered
            // by transient feedback or the standby idle animation.
            presentation_ = Presentation::Mode;
            const char* emotion = kEmotionLearningEmotions_[current_emotion_index_];
            display_->SetEmotion(emotion);
            // Render the emotion name label on the learning page.
            static_cast<emote::EmoteDisplay*>(display_)->ShowEmotionLearning(
                kEmotionLearningNames_[current_emotion_index_]);
            ESP_LOGI(TAG, "Emotion: %s (%s)", emotion,
                     kEmotionLearningNames_[current_emotion_index_]);
        }
    }

    void ShowEmotionLearningNext()
    {
        current_emotion_index_ = (current_emotion_index_ + 1) % kEmotionLearningCount;
        ShowEmotionLearningCurrent();
    }

    void EnterFunctionPage()
    {
        if (mode_ == Mode::FunctionPage) {
            return;
        }
        EnterMode(Mode::FunctionPage);
        if (display_ != nullptr) {
            static_cast<emote::EmoteDisplay*>(display_)->ShowFunctionPage();
        }
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
        ESP_LOGI(TAG, "Entered function page");
    }

    void EnterSettingsPage()
    {
        if (mode_ == Mode::SettingsPage) {
            return;
        }
        settings_selected_index_ = 0;
        settings_adjusting_ = false;
        // Seed the cached volume from the codec so the displayed value matches
        // actual output (the codec default may differ from the initial value).
        volume_ = Application::GetInstance().GetAudioService().GetOutputVolume();
        EnterMode(Mode::SettingsPage);
        RenderSettingsPage();
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
        ESP_LOGI(TAG, "Entered settings page");
    }

    // The active setting's current value: backlight brightness for row 0
    // (from backlight_), output volume (from the cached volume_) for row 1.
    int CurrentSettingsValue() const
    {
        if (settings_selected_index_ == 0) {
            return backlight_ != nullptr ? static_cast<int>(backlight_->brightness()) : 0;
        }
        return volume_;
    }

    // Adjust and live-apply the active setting, clamped to 0-100.
    void AdjustSettingsValue(int delta)
    {
        if (settings_selected_index_ == 0) {
            int value = (backlight_ != nullptr ? static_cast<int>(backlight_->brightness()) : 0) + delta;
            value = value < 0 ? 0 : (value > 100 ? 100 : value);
            if (backlight_ != nullptr) {
                backlight_->SetBrightness(static_cast<uint8_t>(value));
            }
        } else {
            volume_ += delta;
            volume_ = volume_ < 0 ? 0 : (volume_ > 100 ? 100 : volume_);
            Application::GetInstance().GetAudioService().SetOutputVolume(volume_);
        }
        RenderSettingsPage();
    }

    // Redraw the settings page from the current state.
    void RenderSettingsPage()
    {
        if (display_ == nullptr) {
            return;
        }
        static_cast<emote::EmoteDisplay*>(display_)->ShowSettingsPage(
            settings_selected_index_, settings_adjusting_, CurrentSettingsValue());
    }

    static void touch_button_event_callback(touch_button_handle_t handle, uint32_t channel, touch_state_t state, void* cb_arg)
    {
        (void)handle;
        (void)channel;
        auto* self = static_cast<EspVocat*>(cb_arg);
        if (self == nullptr || self->display_ == nullptr) {
            return;
        }

        // Single-pad (PCB v1.0) outer capacitive surface: a touch is recognized
        // once per debounced interaction, on release. It is feedback-only (Pet)
        // and never changes mode, toggles chat, advances an emotion, or records
        // a drink.
        if (state == TOUCH_STATE_INACTIVE) {
            self->HandleOuterTouchPet();
        }
    }

    static void touch_cap_poll_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        while (true) {
            if (self != nullptr) {
                if (self->touch_slider_handle_ != nullptr) {
                    touch_slider_sensor_handle_events(self->touch_slider_handle_);
                } else if (self->touch_button_handle_ != nullptr) {
                    touch_button_sensor_handle_events(self->touch_button_handle_);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void InitializeCapacitiveTouchPads()
    {
        if (TOUCH_PAD1 == GPIO_NUM_NC) {
            ESP_LOGW(TAG, "Capacitive touch disabled: TOUCH_PAD1 NC");
            return;
        }

        const uint32_t ch1 = TouchChannelFromPadGpio(TOUCH_PAD1);
        if (ch1 == 0) {
            ESP_LOGW(TAG, "TOUCH_PAD1 GPIO %d is not a touch channel (expect GPIO1..GPIO14)", (int)TOUCH_PAD1);
            return;
        }

        if (TOUCH_PAD2 != GPIO_NUM_NC) {
            const uint32_t ch2 = TouchChannelFromPadGpio(TOUCH_PAD2);
            if (ch2 == 0) {
                ESP_LOGW(TAG, "TOUCH_PAD2 GPIO %d is not a touch channel", (int)TOUCH_PAD2);
                return;
            }

            static uint32_t slider_ch[2];
            static float slider_thr[2];
            slider_ch[0] = ch1;
            slider_ch[1] = ch2;
            slider_thr[0] = 0.004f;
            slider_thr[1] = 0.006f;

            touch_slider_config_t sld_cfg = {
                .channel_num = 2,
                .channel_list = slider_ch,
                .channel_threshold = slider_thr,
                .channel_gold_value = nullptr,
                .debounce_times = 1,
                .filter_reset_times = 5,
                .position_range = 10000,
                .calculate_window = 2,
                .swipe_threshold = 28.f,
                .swipe_hysterisis = 22.f,
                .swipe_alpha = 0.9f,
                .skip_lowlevel_init = false,
            };
            esp_err_t err = touch_slider_sensor_create(&sld_cfg, &touch_slider_handle_, touch_slider_event_callback, this);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "touch_slider_sensor_create failed: %s", esp_err_to_name(err));
                touch_slider_handle_ = nullptr;
                return;
            }
            xTaskCreatePinnedToCore(touch_cap_poll_task, "touch_cap", 3072, this, 3, &touch_slider_task_handle_, 1);
            ESP_LOGI(TAG, "Touch slider (PCB v1.2+): PAD1 GPIO%d ch%u, PAD2 GPIO%d ch%u",
                     (int)TOUCH_PAD1, (unsigned)slider_ch[0], (int)TOUCH_PAD2, (unsigned)slider_ch[1]);
            return;
        }

        static uint32_t btn_ch[1];
        static float btn_thr[1];
        btn_ch[0] = ch1;
        btn_thr[0] = 0.004f;

        touch_button_config_t btn_cfg = {
            .channel_num = 1,
            .channel_list = btn_ch,
            .channel_threshold = btn_thr,
            .channel_gold_value = nullptr,
            .debounce_times = 2,
            .skip_lowlevel_init = false,
        };
        esp_err_t err = touch_button_sensor_create(&btn_cfg, &touch_button_handle_, touch_button_event_callback, this);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch_button_sensor_create failed: %s", esp_err_to_name(err));
            touch_button_handle_ = nullptr;
            return;
        }
        xTaskCreatePinnedToCore(touch_cap_poll_task, "touch_cap", 3072, this, 3, &touch_slider_task_handle_, 1);
        ESP_LOGI(TAG, "Touch button (PCB v1.0): TOUCH_PAD1 GPIO%d ch%u", (int)TOUCH_PAD1, (unsigned)btn_ch[0]);
    }

    void InitializeSpi()
    {
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                                  QSPI_PIN_NUM_LCD_DATA0,
                                                                                  QSPI_PIN_NUM_LCD_DATA1,
                                                                                  QSPI_PIN_NUM_LCD_DATA2,
                                                                                  QSPI_PIN_NUM_LCD_DATA3,
                                                                                  QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeSt77916Display(uint8_t pcb_version)
    {

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        const esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));
        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
            .flags = {
                .reset_active_high = pcb_version,
            },
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
        backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        // ---- SPIKE (Task 1): prove emote and LVGL can alternate on the same
        // ST77916 panel. RenderSwitch initializes LVGL on the same panel/io
        // handles that EmoteDisplay owns, then the boot path demonstrates a
        // round-trip: emote 2s -> LVGL Settings 2s -> emote. See
        // main/display/esp_vocat_ui.h for the gate model.
        render_switch_ = new RenderSwitch(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                          static_cast<emote::EmoteDisplay*>(display_));
        render_switch_->ShowEmote();
        vTaskDelay(pdMS_TO_TICKS(2000));
        render_switch_->ShowLvgl(ScreenId::Settings);
        vTaskDelay(pdMS_TO_TICKS(2000));
        render_switch_->ShowEmote();
        // Spike demo done. The RenderSwitch stays alive (LVGL dormant) so the
        // app continues on the emote pet face exactly as before. Visual
        // tear/flicker is deferred to the real-device acceptance (Task 8).
#endif
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            // A BOOT press is interaction: wake the display / exit light sleep.
            TouchPowerSaveActivity();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        gpio_config_t power_gpio_config = {
            .pin_bit_mask = (BIT64(POWER_CTRL)),
            .mode = GPIO_MODE_OUTPUT,

        };
        ESP_ERROR_CHECK(gpio_config(&power_gpio_config));

        gpio_set_level(POWER_CTRL, 0);
    }

#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    void InitializeCamera() {
        esp_video_init_usb_uvc_config_t usb_uvc_config = {
            .uvc = {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
            .usb = {
                .init_usb_host_lib = true,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
        };

        esp_video_init_config_t video_config = {
            .usb_uvc = &usb_uvc_config,
        };

        camera_ = new EspVideo(video_config);
    }
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE

public:
    ~EspVocat() {
        // Stop tasks
        if (charge_task_handle_ != nullptr) {
            vTaskDelete(charge_task_handle_);
        }
        if (touch_task_handle_ != nullptr) {
            vTaskDelete(touch_task_handle_);
        }
        if (imu_task_handle_ != nullptr) {
            vTaskDelete(imu_task_handle_);
        }
        if (touch_slider_task_handle_ != nullptr) {
            vTaskDelete(touch_slider_task_handle_);
            touch_slider_task_handle_ = nullptr;
        }
        if (touch_slider_handle_ != nullptr) {
            touch_slider_sensor_delete(touch_slider_handle_);
            touch_slider_handle_ = nullptr;
        }
        if (touch_button_handle_ != nullptr) {
            touch_button_sensor_delete(touch_button_handle_);
            touch_button_handle_ = nullptr;
        }

        // Delete objects
        delete charge_;
        delete cst816s_;
        delete display_;
        // Note: backlight_ (PwmBacklight) and camera_ (EspVideo) are not deleted here
        // because their base classes (Backlight, Camera) don't have virtual destructors.
        // Since EspVocat is a singleton that lives for the device lifetime, this is acceptable.

        // Remove GPIO ISR handler
        gpio_isr_handler_remove(TP_PIN_NUM_INT);
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_delete(emotion_reset_timer_);
            emotion_reset_timer_ = nullptr;
        }
        if (mode_idle_timer_ != nullptr) {
            esp_timer_stop(mode_idle_timer_);
            esp_timer_delete(mode_idle_timer_);
            mode_idle_timer_ = nullptr;
        }
        if (reminder_timer_ != nullptr) {
            esp_timer_stop(reminder_timer_);
            esp_timer_delete(reminder_timer_);
            reminder_timer_ = nullptr;
        }
        // Stop/delete reminder_restore_timer_ before reminder_mutex_: its
        // callback locks reminder_mutex_, so it must not fire after the mutex
        // is freed.
        if (reminder_restore_timer_ != nullptr) {
            esp_timer_stop(reminder_restore_timer_);
            esp_timer_delete(reminder_restore_timer_);
            reminder_restore_timer_ = nullptr;
        }
        if (reminder_mutex_ != nullptr) {
            vSemaphoreDelete(reminder_mutex_);
            reminder_mutex_ = nullptr;
        }
        if (display_sleep_timer_ != nullptr) {
            esp_timer_stop(display_sleep_timer_);
            esp_timer_delete(display_sleep_timer_);
            display_sleep_timer_ = nullptr;
        }
        delete power_save_timer_;
        power_save_timer_ = nullptr;

        // Disable temperature sensor
        if (temp_sensor != NULL) {
            temperature_sensor_disable(temp_sensor);
            temperature_sensor_uninstall(temp_sensor);
            temp_sensor = NULL;
        }
    }

    EspVocat() : boot_button_(BOOT_BUTTON_GPIO)
    {
        const esp_timer_create_args_t emotion_timer_args = {
            .callback = &EspVocat::emotion_reset_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "emotion_rst",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&emotion_timer_args, &emotion_reset_timer_));

        const esp_timer_create_args_t mode_idle_timer_args = {
            .callback = &EspVocat::mode_idle_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "mode_idle",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&mode_idle_timer_args, &mode_idle_timer_));

        const esp_timer_create_args_t reminder_restore_timer_args = {
            .callback = &EspVocat::reminder_restore_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "rmd_rst",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&reminder_restore_timer_args, &reminder_restore_timer_));

        if (reminder_mutex_ == nullptr) {
            reminder_mutex_ = xSemaphoreCreateMutex();
        }

        InitializeI2c();
        uint8_t pcb_version = DetectPcbVersion();
        InitializeCharge();
        InitializeCst816sTouchPad();
        InitializeBmi270();

        InitializeSpi();
        InitializeSt77916Display(pcb_version);
        InitializeButtons();
        InitializeCapacitiveTouchPads();
        LoadHabitState();
        InitializeReminderTimer();
        InitializePowerSave();
#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
        InitializeCamera();
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    Cst816s* GetTouchpad()
    {
        return cst816s_;
    }

    virtual Backlight* GetBacklight() override
    {
        return backlight_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

// Static array definitions for emotion learning
const char* EspVocat::kEmotionLearningEmotions_[] = {
    "happy", "sad", "angry", "shocked",
    "confused", "sleepy", "loving", "neutral"
};
const char* EspVocat::kEmotionLearningNames_[] = {
    "开心", "伤心", "生气", "惊讶",
    "困惑", "困了", "喜欢", "平静"
};

// Daily schedule reminders: 8:00 wake, 12:00 lunch, 14:00 nap, 18:00 dinner, 21:00 sleep.
const EspVocat::ScheduleEntry EspVocat::kScheduleReminders_[] = {
    {8, 0, "该起床了~", "happy"},
    {12, 0, "该吃午饭了~", "happy"},
    {14, 0, "该睡午觉了~", "sleepy"},
    {18, 0, "该吃晚饭了~", "happy"},
    {21, 0, "该睡觉了~", "sleepy"},
};

// ============================================================================
// SPIKE (Task 1): RenderSwitch implementation.
// Proof-of-concept only - Task 2 re-homes these methods into their own .cc.
// The gate model is documented in main/display/esp_vocat_ui.h.
// ============================================================================

// Solid-color stand-in for a real page, per ScreenId.
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

DECLARE_BOARD(EspVocat);
