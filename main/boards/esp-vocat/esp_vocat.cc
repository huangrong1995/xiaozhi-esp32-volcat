#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "esp_video.h"
#include "audio/audio_service.h"
#include "assets/lang_config.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include "esp_idf_version.h"
#include <cinttypes>

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
enum class Mode { Chat, EmotionLearning, Yoga };

class EspVocat : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_bus_handle_t shared_i2c_bus_handle_ = nullptr;
    Cst816s* cst816s_;
    Charge* charge_;
    Button boot_button_;
    Display* display_ = nullptr;
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

    // Gesture detection state
    int64_t touch_press_time_ = 0;
    int64_t touch_last_release_time_ = 0;
    bool touch_is_pressed_ = false;

    // Triple-tap detection for emotion learning mode
    int tap_count_ = 0;
    int64_t tap_first_time_ = 0;
    size_t current_emotion_index_ = 0;

    // Emotion learning: list of emotions to show
    static const char* kEmotionLearningEmotions_[];
    static const char* kEmotionLearningNames_[];
    static constexpr size_t kEmotionLearningCount = 8;

    // Habit tracking state
    struct HabitState {
        int64_t last_drink_time;     // Unix timestamp of last drink
        int drink_streak;            // Consecutive drink completions today
        bool drink_reminder_shown;   // Whether reminder is shown for current interval
    };
    HabitState habit_state_ = {};

    // Habit configuration
    static constexpr int64_t kDrinkReminderIntervalMs = 60 * 60 * 1000;  // 60 minutes in ms

    // Touch swipe detection for CST816S
    bool swipe_start_recorded_ = false;
    int64_t touch_press_time_ms_ = 0;
    int swipe_start_x_ = 0;
    int swipe_start_y_ = 0;
    static constexpr int kSwipeThreshold = 80;  // Minimum swipe distance

    // Yoga challenge mode
    size_t current_pose_index_ = 0;
    static const char* kYogaPoseNames_[];
    static const char* kYogaPoseDirections_[];
    static constexpr size_t kYogaPoseCount = 6;

    // Unified interaction mode state. mode_ is read/written only from the
    // interaction task/callback context (touch events and timer callbacks),
    // so it does not require locking.
    Mode mode_ = Mode::Chat;
    static constexpr uint64_t kModeIdleTimeoutMs = 30000;  // Idle timeout (ms) before returning to chat
    esp_timer_handle_t mode_idle_timer_ = nullptr;

    static void mode_idle_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr || self->mode_ == Mode::Chat) {
            return;
        }
        self->ExitToChat();
    }

    void OnGesture(Gesture gesture)
    {
        ResetModeIdleTimer();
        switch (mode_) {
        case Mode::Chat:
            HandleChatGesture(gesture);
            break;
        case Mode::EmotionLearning:
            HandleEmotionLearningGesture(gesture);
            break;
        case Mode::Yoga:
            HandleYogaGesture(gesture);
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
    }

    void ResetModeIdleTimer()
    {
        if (mode_idle_timer_ == nullptr) {
            return;
        }
        esp_timer_stop(mode_idle_timer_);
        esp_timer_start_once(mode_idle_timer_, kModeIdleTimeoutMs * 1000ULL);
    }

    // Per-mode gesture handlers. Note: Pet and Shake must never change mode_.
    void HandleChatGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::Tap:
            Application::GetInstance().ToggleChatState();
            break;
        case Gesture::SwipeLeft:
            EnterEmotionLearningMode();
            break;
        case Gesture::SwipeRight:
            EnterYogaChallengeMode();
            break;
        case Gesture::LongPress:
            // Short cancel/help feedback without muting or recording drink.
            ShowTemporaryEmotion("confused", 2000);
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
            ESP_LOGI(TAG, "Long press help/cancel feedback");
            break;
        default:
            // Ignore mode-only gestures (SwipeUp, SwipeDown, Pet, Shake,
            // ChatToggle, None). A swipe must never toggle chat.
            break;
        }
    }

    void HandleEmotionLearningGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::SwipeLeft:  // next emotion
            ShowEmotionLearningNext();
            break;
        case Gesture::SwipeRight:  // previous emotion
            current_emotion_index_ =
                (current_emotion_index_ + kEmotionLearningCount - 1) % kEmotionLearningCount;
            ShowEmotionLearningCurrent();
            break;
        case Gesture::Tap:  // feedback for the current emotion
            ShowEmotionLearningCurrent();
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
            break;
        case Gesture::LongPress:
            ExitToChat();
            break;
        default:
            break;
        }
    }

    void HandleYogaGesture(Gesture gesture)
    {
        switch (gesture) {
        case Gesture::SwipeLeft:  // next pose
            current_pose_index_ = (current_pose_index_ + 1) % kYogaPoseCount;
            ShowYogaPose();
            break;
        case Gesture::SwipeRight:  // previous pose
            current_pose_index_ =
                (current_pose_index_ + kYogaPoseCount - 1) % kYogaPoseCount;
            ShowYogaPose();
            break;
        case Gesture::Tap:  // replay the current pose
            ShowYogaPose();
            break;
        case Gesture::LongPress:
            ExitToChat();
            break;
        default:
            break;
        }
    }

    // Gesture timing thresholds (ms)
    static constexpr int64_t kDoubleTapThresholdMs = 350;
    static constexpr int64_t kLongPressThresholdMs = 1200;

    static void emotion_reset_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self && self->display_ != nullptr) {
            self->display_->SetEmotion("neutral");
        }
    }

    void LoadHabitState()
    {
        Settings s("habit", false);
        habit_state_.last_drink_time = s.GetInt("last_drink", 0);
        habit_state_.drink_streak = s.GetInt("drink_streak", 0);
        habit_state_.drink_reminder_shown = false;
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

    void CheckDrinkReminder()
    {
        int64_t now_ms = esp_timer_get_time() / 1000;

        // If no drink recorded today, or interval has passed, show reminder
        if (habit_state_.last_drink_time == 0 ||
            (now_ms - habit_state_.last_drink_time) >= kDrinkReminderIntervalMs) {
            if (!habit_state_.drink_reminder_shown) {
                ESP_LOGI(TAG, "Drink reminder: time to drink water!");
                ShowTemporaryEmotion("confused", 5000);
                Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
                vTaskDelay(pdMS_TO_TICKS(500));
                Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
                habit_state_.drink_reminder_shown = true;
            }
        }
    }

    void MarkDrinkCompleted()
    {
        int64_t now_ms = esp_timer_get_time() / 1000;
        habit_state_.last_drink_time = now_ms;
        habit_state_.drink_streak++;
        habit_state_.drink_reminder_shown = false;
        SaveHabitState();

        ESP_LOGI(TAG, "Drink completed! Streak: %d", habit_state_.drink_streak);
        ShowTemporaryEmotion("loving", 3000);
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
    }

    static void reminder_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr) {
            return;
        }

        // Check drink reminder
        self->CheckDrinkReminder();

        // Get current time
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        int current_hour = timeinfo.tm_hour;
        int current_min = timeinfo.tm_min;

        // Check reminder times: 8:00 wake up, 12:00 lunch, 14:00 nap, 18:00 dinner, 21:00 sleep
        // Format: hour, minute, reminder message, emotion
        struct Reminder {
            int hour;
            int minute;
            const char* message;
            const char* emotion;
        };

        static const Reminder reminders[] = {
            {8, 0, "该起床了~", "happy"},
            {12, 0, "该吃午饭了~", "happy"},
            {14, 0, "该睡午觉了~", "sleepy"},
            {18, 0, "该吃晚饭了~", "happy"},
            {21, 0, "该睡觉了~", "sleepy"},
        };

        for (const auto& r : reminders) {
            if (current_hour == r.hour && current_min == r.minute) {
                ESP_LOGI(TAG, "Reminder: %s", r.message);
                self->ShowTemporaryEmotion(r.emotion, 5000);
                Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
                vTaskDelay(pdMS_TO_TICKS(500));
                Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
                break;
            }
        }
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
        display_->SetEmotion(emotion);
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_start_once(emotion_reset_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL);
        }
    }

    void ShowHappyTouchFeedback()
    {
        static int64_t s_last_us = 0;
        constexpr int64_t kCooldownUs = 1200000;
        const int64_t now = esp_timer_get_time();
        if ((now - s_last_us) < kCooldownUs) {
            return;
        }
        s_last_us = now;
        ShowTemporaryEmotion("happy", 2000);
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
                        // "dizzy/nauseated" are not guaranteed in current assets, use supported fallback.
                        self->ShowTemporaryEmotion("confused", 1800);
                    }
                }
                prev = cur;
                has_prev = true;
            }
            // Check yoga pose match periodically
            self->ProcessImuForYoga();
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

        self->ShowHappyTouchFeedback();
    }

    void ShowSingleTapFeedback()
    {
        ShowTemporaryEmotion("happy", 2000);
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
    }

    void ShowDoubleTapFeedback()
    {
        ShowTemporaryEmotion("shocked", 2500);
        auto& audio = Application::GetInstance().GetAudioService();
        audio.PlaySound(Lang::Sounds::OGG_POPUP);
        vTaskDelay(pdMS_TO_TICKS(150));
        audio.PlaySound(Lang::Sounds::OGG_POPUP);
    }

    void ShowLongPressFeedback()
    {
        // Toggle mute mode on long press
        static bool is_muted = false;
        is_muted = !is_muted;
        if (is_muted) {
            ShowTemporaryEmotion("angry", 2000);
        } else {
            ShowTemporaryEmotion("happy", 2000);
        }
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_VIBRATION);
        ESP_LOGI(TAG, "Long press: device %s", is_muted ? "MUTED" : "UNMUTED");
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
            const char* emotion = kEmotionLearningEmotions_[current_emotion_index_];
            display_->SetEmotion(emotion);
            ESP_LOGI(TAG, "Emotion: %s (%s)", emotion, kEmotionLearningNames_[current_emotion_index_]);
        }
    }

    void ShowEmotionLearningNext()
    {
        current_emotion_index_ = (current_emotion_index_ + 1) % kEmotionLearningCount;
        ShowEmotionLearningCurrent();
    }

    void EnterYogaChallengeMode()
    {
        if (mode_ == Mode::Yoga) {
            return;
        }
        EnterMode(Mode::Yoga);
        current_pose_index_ = 0;
        ShowYogaPose();
        Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
        ESP_LOGI(TAG, "Entered yoga challenge mode");
    }

    void ExitYogaChallengeMode()
    {
        if (mode_ != Mode::Yoga) {
            return;
        }
        ExitToChat();
        ShowTemporaryEmotion("happy", 2000);
        ESP_LOGI(TAG, "Exited yoga challenge mode");
    }

    void ShowYogaPose()
    {
        if (display_ != nullptr) {
            const char* direction = kYogaPoseDirections_[current_pose_index_];
            // Use DrawArrow to directly draw arrow on LCD panel
            static_cast<emote::EmoteDisplay*>(display_)->DrawArrow(direction);
            ESP_LOGI(TAG, "Yoga pose: %s (%s)", kYogaPoseNames_[current_pose_index_], direction);
        }
    }

    void CheckYogaPoseMatch(const char* motion_direction)
    {
        if (mode_ != Mode::Yoga) {
            return;
        }
        const char* target = kYogaPoseDirections_[current_pose_index_];
        if (strcmp(motion_direction, target) == 0) {
            ESP_LOGI(TAG, "Correct! Moving to next pose");
            ShowTemporaryEmotion("happy", 1000);
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_SUCCESS);
            current_pose_index_ = (current_pose_index_ + 1) % kYogaPoseCount;
            vTaskDelay(pdMS_TO_TICKS(500));
            ShowYogaPose();
        }
    }

    void ProcessImuForYoga()
    {
        if (mode_ != Mode::Yoga || !bmi270_ready_) {
            return;
        }
        static int64_t last_check_ms = 0;
        constexpr int64_t kCooldownMs = 500;  // Only check every 500ms
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_check_ms < kCooldownMs) {
            return;
        }
        last_check_ms = now_ms;

        // Read accelerometer data
        struct bmi2_sens_data data = {};
        if (Bmi270Motion::ReadAccelRaw(data)) {
            // Determine tilt direction
            int acc_x = data.acc.x;
            int acc_y = data.acc.y;
            const char* direction = nullptr;
            // Check dominant tilt
            if (abs(acc_x) > abs(acc_y)) {
                if (acc_x > 5000) {
                    direction = "right";
                } else if (acc_x < -5000) {
                    direction = "left";
                }
            } else {
                if (acc_y > 5000) {
                    direction = "down";
                } else if (acc_y < -5000) {
                    direction = "up";
                }
            }
            if (direction != nullptr) {
                CheckYogaPoseMatch(direction);
            }
        }
    }

    static void touch_button_event_callback(touch_button_handle_t handle, uint32_t channel, touch_state_t state, void* cb_arg)
    {
        (void)handle;
        auto* self = static_cast<EspVocat*>(cb_arg);
        if (self == nullptr || self->display_ == nullptr) {
            return;
        }

        int64_t now = esp_timer_get_time() / 1000;

        if (state == TOUCH_STATE_ACTIVE) {
            self->touch_press_time_ = now;
            self->touch_is_pressed_ = true;

            // Track tap count for triple-tap detection
            if (self->tap_count_ == 0) {
                self->tap_first_time_ = now;
            }
            self->tap_count_++;

            ESP_LOGD(TAG, "Touch PRESS ch=%" PRIu32 " (tap %d)", channel, self->tap_count_);
        } else if (state == TOUCH_STATE_INACTIVE) {
            // Detect release by state change from ACTIVE to INACTIVE
            if (!self->touch_is_pressed_) {
                return;
            }
            self->touch_is_pressed_ = false;
            int64_t press_duration = now - self->touch_press_time_;

            // Long press detection (release after holding > threshold)
            if (press_duration >= kLongPressThresholdMs) {
                ESP_LOGI(TAG, "Gesture: LONG PRESS (%" PRId64 " ms)", press_duration);
                if (self->mode_ == Mode::EmotionLearning) {
                    self->ExitEmotionLearningMode();
                } else {
                    // Mark drink completed and toggle mute
                    self->MarkDrinkCompleted();
                    self->ShowLongPressFeedback();
                }
            }
            // Single tap - check triple-tap timeout first
            else {
                int64_t time_since_first_tap = now - self->tap_first_time_;
                if (time_since_first_tap > kDoubleTapThresholdMs + 200) {
                    // Timeout exceeded, reset and treat as single tap
                    ESP_LOGI(TAG, "Gesture: SINGLE TAP (timeout reset)");
                    if (self->mode_ == Mode::EmotionLearning) {
                        self->ShowEmotionLearningNext();
                    } else {
                        self->ShowSingleTapFeedback();
                    }
                    self->tap_count_ = 0;
                } else if (self->mode_ == Mode::EmotionLearning) {
                    // In emotion learning mode, single tap immediately advances
                    ESP_LOGI(TAG, "Gesture: SINGLE TAP (emotion learning)");
                    self->ShowEmotionLearningNext();
                    self->tap_count_ = 0;
                }
                // else: wait for more taps (potential double/triple tap)
            }

            self->touch_last_release_time_ = now;
        }

        // Check for triple tap after each release
        if (self->tap_count_ >= 3) {
            int64_t time_since_first = now - self->tap_first_time_;
            if (time_since_first <= kDoubleTapThresholdMs + 200) {
                ESP_LOGI(TAG, "Gesture: TRIPLE TAP");
                if (self->mode_ == Mode::EmotionLearning) {
                    // Already in mode, do nothing or restart
                    self->current_emotion_index_ = 0;
                    self->ShowEmotionLearningCurrent();
                } else {
                    self->EnterEmotionLearningMode();
                }
                self->tap_count_ = 0;
            }
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
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
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

// Static array definitions for yoga challenge
const char* EspVocat::kYogaPoseNames_[] = {
    "向上倾斜", "向下倾斜", "向左倾斜", "向右倾斜", "向上倾斜", "向下倾斜"
};
const char* EspVocat::kYogaPoseDirections_[] = {
    "up", "down", "left", "right", "up", "down"
};

DECLARE_BOARD(EspVocat);
