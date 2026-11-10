#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    JAFR_AUDIO_ROUTE_AUTO = 0,
    JAFR_AUDIO_ROUTE_S31 = 1,
    JAFR_AUDIO_ROUTE_DUALEYE = 2,
} jafr_audio_route_t;

typedef struct {
    char wifi_ip[16];
    char ethernet_ip[16];
    char gateway_ip[16];
    bool ethernet_link;
    bool gateway_auto;
    bool gateway_discovered;
    bool espnow_ready;
    bool mic_muted;
    uint32_t espnow_accepted;
    uint32_t espnow_failed;
    uint32_t espnow_delivered;
    uint32_t espnow_delivery_failed;
    char espnow_last[40];
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t free_heap;
    double mic_rms;
    bool ble_audio_initialized;
    bool ble_audio_scanning;
    bool ble_audio_connected;
    bool ble_audio_ready;
    uint16_t ble_audio_mtu;
    uint32_t ble_audio_sent;
    uint32_t ble_audio_dropped;
    uint32_t ble_audio_failures;
    char audio_route[12];
    char audio_output[16];
} jafr_web_status_t;

void jafr_network_get_web_status(jafr_web_status_t *status);
esp_err_t jafr_network_set_gateway_config(bool automatic, const char *ip);
void jafr_audio_set_mic_muted(bool muted);
esp_err_t jafr_audio_set_output_route(const char *route);
esp_err_t jafr_webui_start(void);
