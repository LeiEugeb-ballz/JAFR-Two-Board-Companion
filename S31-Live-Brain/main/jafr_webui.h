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
    uint32_t internal_free;
    uint32_t internal_minimum;
    uint32_t internal_largest;
    uint32_t psram_free;
    uint32_t psram_minimum;
    uint32_t allocation_failures;
    uint32_t last_failed_allocation;
    uint32_t last_failed_caps;
    uint32_t status_requests;
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
    char voice_mode[12];
    char cloud_state[16];
    bool cloud_key_configured;
    bool cloud_direct_active;
    bool cloud_fallback_active;
    bool cloud_provisioning_required;
    bool cloud_stt_connected;
    bool cloud_tts_connected;
    uint32_t cloud_chunks_sent;
    uint32_t cloud_chunks_dropped;
    uint32_t cloud_playback_drops;
    uint32_t cloud_turns_completed;
    uint32_t cloud_history_turns;
    uint32_t cloud_reconnects;
    uint32_t cloud_failures;
    uint32_t cloud_auth_failures;
    uint32_t cloud_turn_to_chat_ms;
    uint32_t cloud_chat_to_text_ms;
    uint32_t cloud_text_to_audio_ms;
    uint32_t cloud_total_audio_ms;
    uint32_t cloud_playback_stack;
    int cloud_last_http_status;
    char cloud_last_error[96];
} jafr_web_status_t;

void jafr_network_get_web_status(jafr_web_status_t *status);
esp_err_t jafr_network_set_gateway_config(bool automatic, const char *ip);
void jafr_audio_set_mic_muted(bool muted);
esp_err_t jafr_audio_set_output_route(const char *route);
esp_err_t jafr_webui_start(void);
