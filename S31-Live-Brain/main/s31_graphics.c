#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "s31_graphics.h"

#define GRAPHICS_UART UART_NUM_1
#define GRAPHICS_TX_GPIO 43 /* J2 pin 17; board schematic, not a verified cable */
#define GRAPHICS_BAUD 115200
#define GRAPHICS_QUEUE_DEPTH 8
#define GRAPHICS_LINE_MAX 40

static const char *TAG = "s31_graphics";
static QueueHandle_t s_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_last[GRAPHICS_LINE_MAX];
static int64_t s_last_blink_us;
static bool s_espnow_ready;
static uint32_t s_espnow_accepted;
static uint32_t s_espnow_failed;
static uint32_t s_espnow_delivered;
static uint32_t s_espnow_delivery_failed;
static char s_espnow_last[GRAPHICS_LINE_MAX] = "none";
static const uint8_t s_espnow_broadcast[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static const uint8_t s_waveshare_mac[ESP_NOW_ETH_ALEN] = {
    0x28, 0x84, 0x85, 0x57, 0x2d, 0x2c
};

typedef struct { char line[GRAPHICS_LINE_MAX]; } graphics_message_t;

static void espnow_send_complete(const esp_now_send_info_t *tx_info,
                                 esp_now_send_status_t status)
{
    if (!tx_info || !tx_info->des_addr ||
        memcmp(tx_info->des_addr, s_waveshare_mac, ESP_NOW_ETH_ALEN) != 0) {
        return; /* Broadcast callback is deliberately not receiver evidence. */
    }
    portENTER_CRITICAL(&s_lock);
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_espnow_delivered++;
    } else {
        s_espnow_delivery_failed++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t espnow_add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0; /* Current STA channel. Waveshare is on channel 1 now. */
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    return err == ESP_ERR_ESPNOW_EXIST ? ESP_OK : err;
}

static void espnow_send_line(const char *line, size_t length)
{
    if (!s_espnow_ready) return;
    esp_err_t err = esp_now_send(s_espnow_broadcast, (const uint8_t *)line, length);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW broadcast rejected: %s", esp_err_to_name(err));
    }
    err = esp_now_send(s_waveshare_mac, (const uint8_t *)line, length);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_espnow_accepted++;
        size_t copy_len = length;
        if (copy_len > 0 && line[copy_len - 1] == '\n') copy_len--;
        if (copy_len >= sizeof(s_espnow_last)) copy_len = sizeof(s_espnow_last) - 1;
        memcpy(s_espnow_last, line, copy_len);
        s_espnow_last[copy_len] = '\0';
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "ESP-NOW accepted: %.*s", (int)length - 1, line);
    } else {
        portENTER_CRITICAL(&s_lock);
        s_espnow_failed++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "ESP-NOW unicast rejected: %s", esp_err_to_name(err));
    }
}

static bool enqueue(const char *line)
{
    if (!s_queue || !line) return false;
    graphics_message_t message = {0};
    size_t length = strlen(line);
    if (length == 0 || length >= sizeof(message.line)) return false;
    memcpy(message.line, line, length + 1);
    bool queued = false;
    portENTER_CRITICAL(&s_lock);
    if (strcmp(s_last, message.line) != 0 && xQueueSend(s_queue, &message, 0) == pdTRUE) {
        memcpy(s_last, message.line, length + 1);
        queued = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return queued;
}

static void graphics_tx_task(void *arg)
{
    graphics_message_t message;
    while (1) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) == pdTRUE) {
            /* Only this low-priority task touches UART. No receiver is required. */
            size_t length = strlen(message.line);
            int written = uart_write_bytes(GRAPHICS_UART, message.line, length);
            if (written == (int)length) {
                ESP_LOGI(TAG, "TX accepted: %.*s", (int)length - 1, message.line);
            } else {
                ESP_LOGW(TAG, "TX rejected: %d/%u bytes", written, (unsigned)length);
            }
            espnow_send_line(message.line, length);
        }
    }
}

esp_err_t s31_graphics_init(void)
{
    if (s_queue) return ESP_OK;
    uart_config_t config = {
        .baud_rate = GRAPHICS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(GRAPHICS_UART, &config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(GRAPHICS_UART, GRAPHICS_TX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(GRAPHICS_UART, 256, 256, 0, NULL, 0);
    if (err != ESP_OK) return err;
    s_queue = xQueueCreate(GRAPHICS_QUEUE_DEPTH, sizeof(graphics_message_t));
    if (!s_queue) {
        uart_driver_delete(GRAPHICS_UART);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(graphics_tx_task, "graphics_tx", 2048, NULL, 1, NULL, 1) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        uart_driver_delete(GRAPHICS_UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART1 TX GPIO%d, %d baud", GRAPHICS_TX_GPIO, GRAPHICS_BAUD);
    return ESP_OK;
}

esp_err_t s31_graphics_network_ready(void)
{
    if (s_espnow_ready) return ESP_OK;
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_now_register_send_cb(espnow_send_complete);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW send callback failed: %s", esp_err_to_name(err));
        return err;
    }
    err = espnow_add_peer(s_espnow_broadcast);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW broadcast peer failed: %s", esp_err_to_name(err));
        return err;
    }
    err = espnow_add_peer(s_waveshare_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW Waveshare peer failed: %s", esp_err_to_name(err));
        return err;
    }
    s_espnow_ready = true;
    ESP_LOGI(TAG, "ESP-NOW graphics ready, Waveshare STA 28:84:85:57:2d:2c");
    return ESP_OK;
}

bool s31_graphics_look(int8_t x, int8_t y)
{
    if (x < -100 || x > 100 || y < -100 || y > 100) return false;
    char line[GRAPHICS_LINE_MAX];
    /* Frozen protocol uses a comma so the receiver can distinguish the pair
     * without accepting ambiguous whitespace or partial coordinates. */
    snprintf(line, sizeof(line), "LOOK %d,%d\n", x, y);
    return enqueue(line);
}

bool s31_graphics_blink(void)
{
    if (!s_queue) return false;
    int64_t now = esp_timer_get_time();
    bool queued = false;
    graphics_message_t message = {.line = "BLINK\n"};
    portENTER_CRITICAL(&s_lock);
    if (now - s_last_blink_us >= 250000 && xQueueSend(s_queue, &message, 0) == pdTRUE) {
        s_last_blink_us = now;
        queued = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return queued;
}

bool s31_graphics_expression(const char *name)
{
    if (!name) return false;
    size_t length = strlen(name);
    if (length == 0 || length > 20) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    char line[GRAPHICS_LINE_MAX];
    snprintf(line, sizeof(line), "EXPRESSION %s\n", name);
    return enqueue(line);
}

bool s31_graphics_sleep(void) { return enqueue("SLEEP\n"); }
bool s31_graphics_wake(void) { return enqueue("WAKE\n"); }

void s31_graphics_get_transport_status(bool *ready, uint32_t *accepted,
                                       uint32_t *failed, uint32_t *delivered,
                                       uint32_t *delivery_failed, char *last,
                                       size_t last_size)
{
    portENTER_CRITICAL(&s_lock);
    if (ready) *ready = s_espnow_ready;
    if (accepted) *accepted = s_espnow_accepted;
    if (failed) *failed = s_espnow_failed;
    if (delivered) *delivered = s_espnow_delivered;
    if (delivery_failed) *delivery_failed = s_espnow_delivery_failed;
    if (last && last_size > 0) snprintf(last, last_size, "%s", s_espnow_last);
    portEXIT_CRITICAL(&s_lock);
}
