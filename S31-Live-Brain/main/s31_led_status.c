#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "s31_led_status.h"
#include "s31_graphics.h"

#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "S31_LED_STATUS";

static led_strip_handle_t s_led_strip = NULL;
static s31_led_state_t s_current_state = S31_LED_STATE_BOOTING;
static TaskHandle_t s_led_task_handle = NULL;
static bool s_initialized = false;

typedef struct {
    uint8_t idle_r, idle_g, idle_b;
    uint8_t listening_r, listening_g, listening_b;
    uint8_t thinking_r, thinking_g, thinking_b;
    uint8_t speaking_r, speaking_g, speaking_b;
} s31_led_theme_t;

static s31_led_theme_t s_default_theme = {
    .idle_r = 0, .idle_g = 25, .idle_b = 0,
    .listening_r = 60, .listening_g = 0, .listening_b = 220,
    .thinking_r = 200, .thinking_g = 120, .thinking_b = 0,
    .speaking_r = 0, .speaking_g = 220, .speaking_b = 120,
};

static s31_led_theme_t s_active_theme;

static const char *STATE_NAMES[] = {
    [S31_LED_STATE_BOOTING]                   = "BOOTING",
    [S31_LED_STATE_SELF_TEST]                 = "SELF_TEST",
    [S31_LED_STATE_CONNECTING]                = "CONNECTING",
    [S31_LED_STATE_WIFI_ASSOCIATED_WAITING_IP]= "WIFI_ASSOCIATED_WAITING_IP",
    [S31_LED_STATE_IP_READY]                  = "IP_READY",
    [S31_LED_STATE_GATEWAY_READY]             = "GATEWAY_READY",
    [S31_LED_STATE_READY]                     = "READY",
    [S31_LED_STATE_IDLE]                      = "IDLE",
    [S31_LED_STATE_LISTENING]                 = "LISTENING",
    [S31_LED_STATE_PROCESSING]                = "PROCESSING",
    [S31_LED_STATE_THINKING]                  = "THINKING",
    [S31_LED_STATE_SPEAKING]                  = "SPEAKING",
    [S31_LED_STATE_BARGE_IN]                  = "BARGE_IN",
    [S31_LED_STATE_ERROR]                     = "ERROR",
    [S31_LED_STATE_DISCONNECTED]              = "DISCONNECTED",
    [S31_LED_STATE_CUSTOM_COLOR]              = "CUSTOM_COLOR",
};

static uint8_t s_custom_r = 0;
static uint8_t s_custom_g = 0;
static uint8_t s_custom_b = 0;

static void load_nvs_theme(void)
{
    s_active_theme = s_default_theme;
    nvs_handle_t handle;
    if (nvs_open("led_theme", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(s31_led_theme_t);
        nvs_get_blob(handle, "theme", &s_active_theme, &size);
        nvs_close(handle);
        ESP_LOGI(TAG, "Loaded custom LED behavior theme from NVS");
    }
}

static void save_nvs_theme(void)
{
    nvs_handle_t handle;
    if (nvs_open("led_theme", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "theme", &s_active_theme, sizeof(s31_led_theme_t));
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved custom LED behavior theme to NVS!");
    }
}

static void s31_led_set_rgb(uint32_t red, uint32_t green, uint32_t blue)
{
    if (s_led_strip) {
        led_strip_set_pixel(s_led_strip, 0, red, green, blue);
        led_strip_refresh(s_led_strip);
    }
}

static void s31_led_animation_task(void *pvParameters)
{
    uint32_t step = 0;

    while (1) {
        s31_led_state_t state = s_current_state;
        step++;

        switch (state) {
        case S31_LED_STATE_BOOTING: {
            float factor = (sinf((float)step * 0.1f) + 1.0f) * 0.5f;
            uint32_t b = (uint32_t)(factor * 150.0f);
            s31_led_set_rgb(0, 0, b);
            break;
        }

        case S31_LED_STATE_SELF_TEST: {
            float factor = (sinf((float)step * 0.2f) + 1.0f) * 0.5f;
            uint32_t b = (uint32_t)(factor * 255.0f); // Brighter blue
            s31_led_set_rgb(0, 0, b);
            break;
        }

        case S31_LED_STATE_CONNECTING: {
            float factor = (sinf((float)step * 0.4f) + 1.0f) * 0.5f;
            uint32_t val = (uint32_t)(factor * 180.0f);
            s31_led_set_rgb(0, val, val);
            break;
        }

        case S31_LED_STATE_WIFI_ASSOCIATED_WAITING_IP: {
            float factor = (sinf((float)step * 0.25f) + 1.0f) * 0.5f;
            uint32_t r = (uint32_t)(factor * 180.0f);
            uint32_t g = (uint32_t)(factor * 120.0f);
            s31_led_set_rgb(r, g, 0);
            break;
        }

        case S31_LED_STATE_IP_READY: {
            s31_led_set_rgb(0, 40, 40);
            break;
        }

        case S31_LED_STATE_GATEWAY_READY:
        case S31_LED_STATE_READY:
        case S31_LED_STATE_IDLE: {
            float factor = (sinf((float)step * 0.1f) + 1.0f) * 0.5f;
            uint32_t r = (uint32_t)(factor * s_active_theme.idle_r);
            uint32_t g = (uint32_t)(factor * s_active_theme.idle_g);
            uint32_t b = (uint32_t)(factor * s_active_theme.idle_b);
            s31_led_set_rgb(r, g, b);
            break;
        }

        case S31_LED_STATE_LISTENING: {
            float factor = (sinf((float)step * 0.3f) + 1.0f) * 0.5f;
            uint32_t r = (uint32_t)(factor * s_active_theme.listening_r);
            uint32_t g = (uint32_t)(factor * s_active_theme.listening_g);
            uint32_t b = (uint32_t)(factor * s_active_theme.listening_b);
            s31_led_set_rgb(r, g, b);
            break;
        }

        case S31_LED_STATE_PROCESSING:
        case S31_LED_STATE_THINKING: {
            float factor = (sinf((float)step * 0.15f) + 1.0f) * 0.5f;
            uint32_t r = (uint32_t)(factor * s_active_theme.thinking_r);
            uint32_t g = (uint32_t)(factor * s_active_theme.thinking_g);
            uint32_t b = (uint32_t)(factor * s_active_theme.thinking_b);
            s31_led_set_rgb(r, g, b);
            break;
        }

        case S31_LED_STATE_SPEAKING: {
            float factor = (sinf((float)step * 0.35f) + 1.0f) * 0.5f;
            uint32_t r = (uint32_t)(factor * s_active_theme.speaking_r);
            uint32_t g = (uint32_t)(factor * s_active_theme.speaking_g);
            uint32_t b = (uint32_t)(factor * s_active_theme.speaking_b);
            s31_led_set_rgb(r, g, b);
            break;
        }

        case S31_LED_STATE_BARGE_IN: {
            // Quick bright red flash
            bool on = (step % 10) < 5;
            s31_led_set_rgb(on ? 255 : 0, 0, 0);
            break;
        }

        case S31_LED_STATE_ERROR: {
            // Rapid flashing Red (5.0 Hz fast blink)
            bool on = (step % 4) < 2;
            s31_led_set_rgb(on ? 255 : 0, 0, 0);
            break;
        }

        case S31_LED_STATE_DISCONNECTED: {
            // Distinct double-blink Orange pattern
            uint32_t phase = step % 30;
            bool on = (phase == 0 || phase == 1 || phase == 4 || phase == 5);
            s31_led_set_rgb(on ? 220 : 0, on ? 80 : 0, 0);
            break;
        }

        case S31_LED_STATE_CUSTOM_COLOR: {
            s31_led_set_rgb(s_custom_r, s_custom_g, s_custom_b);
            break;
        }

        default:
            s31_led_set_rgb(0, 0, 0);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(30)); // 30ms step time (~33 FPS animation rate)
    }
}

esp_err_t s31_led_status_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing S31 Status LED on GPIO %d (WS2812 RGB LED)...", S31_STATUS_LED_GPIO);

    led_strip_config_t strip_config = {
        .strip_gpio_num = S31_STATUS_LED_GPIO,
        .max_leds = 1, // Single onboard RGB LED (D7)
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WS2812 LED strip device: %s", esp_err_to_name(ret));
        return ret;
    }

    led_strip_clear(s_led_strip);
    load_nvs_theme();
    s_current_state = S31_LED_STATE_BOOTING;
    s_initialized = true;

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        s31_led_animation_task,
        "s31_led_task",
        2048,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_led_task_handle,
        0
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create s31_led_task FreeRTOS task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "S31 Status LED subsystem initialized successfully (State: %s)", STATE_NAMES[s_current_state]);
    return ESP_OK;
}

void s31_led_status_set_state(s31_led_state_t state)
{
    if (state < S31_LED_STATE_MAX && state != s_current_state) {
        ESP_LOGI(TAG, "Status LED State Change: %s -> %s", STATE_NAMES[s_current_state], STATE_NAMES[state]);
        s_current_state = state;
        switch (state) {
        case S31_LED_STATE_IDLE:
        case S31_LED_STATE_READY:
        case S31_LED_STATE_GATEWAY_READY:
            s31_graphics_expression("neutral");
            break;
        case S31_LED_STATE_LISTENING:
            s31_graphics_expression("listening");
            break;
        case S31_LED_STATE_PROCESSING:
        case S31_LED_STATE_THINKING:
            s31_graphics_expression("thinking");
            break;
        case S31_LED_STATE_SPEAKING:
            s31_graphics_expression("speaking");
            break;
        case S31_LED_STATE_ERROR:
        case S31_LED_STATE_DISCONNECTED:
            s31_graphics_expression("error");
            break;
        default:
            break;
        }
    }
}

s31_led_state_t s31_led_status_get_state(void)
{
    return s_current_state;
}

const char *s31_led_status_get_state_name(s31_led_state_t state)
{
    if (state < S31_LED_STATE_MAX) {
        return STATE_NAMES[state];
    }
    return "UNKNOWN";
}

void s31_led_status_set_custom_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    s_custom_r = red;
    s_custom_g = green;
    s_custom_b = blue;
    s_current_state = S31_LED_STATE_CUSTOM_COLOR;
    ESP_LOGI(TAG, "Custom LED Color Applied: R=%d, G=%d, B=%d", red, green, blue);
}

void s31_led_status_set_state_color(s31_led_state_t state, uint8_t r, uint8_t g, uint8_t b)
{
    if (state == S31_LED_STATE_IDLE || state == S31_LED_STATE_GATEWAY_READY) {
        s_active_theme.idle_r = r; s_active_theme.idle_g = g; s_active_theme.idle_b = b;
    } else if (state == S31_LED_STATE_LISTENING) {
        s_active_theme.listening_r = r; s_active_theme.listening_g = g; s_active_theme.listening_b = b;
    } else if (state == S31_LED_STATE_THINKING) {
        s_active_theme.thinking_r = r; s_active_theme.thinking_g = g; s_active_theme.thinking_b = b;
    } else if (state == S31_LED_STATE_SPEAKING) {
        s_active_theme.speaking_r = r; s_active_theme.speaking_g = g; s_active_theme.speaking_b = b;
    }
    save_nvs_theme();
}

void s31_led_status_factory_reset_theme(void)
{
    s_active_theme = s_default_theme;
    nvs_handle_t handle;
    if (nvs_open("led_theme", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Factory reset LED theme defaults restored!");
}
