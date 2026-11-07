#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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
} jafr_web_status_t;

void jafr_network_get_web_status(jafr_web_status_t *status);
esp_err_t jafr_network_set_gateway_config(bool automatic, const char *ip);
void jafr_audio_set_mic_muted(bool muted);
esp_err_t jafr_webui_start(void);
