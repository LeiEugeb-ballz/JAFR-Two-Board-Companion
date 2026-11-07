#ifndef S31_LED_STATUS_H
#define S31_LED_STATUS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ESP32-S31 Function-CoreBoard-1 Certified Status LED Mapping:
 *        GPIO 60 connected via level shifter Q1 to WS2812 RGB LED (D7 DIN pin 4).
 */
#define S31_STATUS_LED_GPIO   60

typedef enum {
    S31_LED_STATE_BOOTING = 0,
    S31_LED_STATE_SELF_TEST,
    S31_LED_STATE_CONNECTING,
    S31_LED_STATE_WIFI_ASSOCIATED_WAITING_IP,
    S31_LED_STATE_IP_READY,
    S31_LED_STATE_GATEWAY_READY,
    S31_LED_STATE_READY,
    S31_LED_STATE_IDLE,
    S31_LED_STATE_LISTENING,
    S31_LED_STATE_PROCESSING,
    S31_LED_STATE_THINKING,
    S31_LED_STATE_SPEAKING,
    S31_LED_STATE_BARGE_IN,
    S31_LED_STATE_ERROR,
    S31_LED_STATE_DISCONNECTED,
    S31_LED_STATE_CUSTOM_COLOR,
    S31_LED_STATE_MAX
} s31_led_state_t;

/**
 * @brief Set custom RGB color on the WS2812 status LED.
 * @param red Red component (0-255).
 * @param green Green component (0-255).
 * @param blue Blue component (0-255).
 */
void s31_led_status_set_custom_rgb(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Set persistent color for a specific behavior state (IDLE, LISTENING, THINKING, SPEAKING).
 * @param state Target state enum.
 * @param r Red (0-255).
 * @param g Green (0-255).
 * @param b Blue (0-255).
 */
void s31_led_status_set_state_color(s31_led_state_t state, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Reset all LED behavior state colors to factory defaults and erase NVS config.
 */
void s31_led_status_factory_reset_theme(void);

/**
 * @brief Initialize the S31 addressable status LED subsystem (WS2812 on GPIO 60)
 *        and launch the independent animation task.
 * @return ESP_OK on success, appropriate esp_err_t on failure.
 */
esp_err_t s31_led_status_init(void);

/**
 * @brief Set the active operational status state.
 * @param state The target status state.
 */
void s31_led_status_set_state(s31_led_state_t state);

/**
 * @brief Get the current operational status state.
 * @return Active s31_led_state_t.
 */
s31_led_state_t s31_led_status_get_state(void);

/**
 * @brief Convert a status state enum to string name.
 * @param state The state enum.
 * @return Const string pointer to state name.
 */
const char *s31_led_status_get_state_name(s31_led_state_t state);

#ifdef __cplusplus
}
#endif

#endif // S31_LED_STATUS_H
