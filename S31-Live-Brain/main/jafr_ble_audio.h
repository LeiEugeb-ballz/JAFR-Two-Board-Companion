#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define JAFR_BLE_AUDIO_SAMPLE_RATE       16000
#define JAFR_BLE_AUDIO_SAMPLES_PER_FRAME 320

typedef struct {
    bool initialized;
    bool scanning;
    bool connected;
    bool ready;
    uint16_t mtu;
    uint32_t queued_frames;
    uint32_t sent_frames;
    uint32_t sent_bytes;
    uint32_t queue_drops;
    uint32_t write_failures;
    uint32_t reconnects;
    uint32_t last_sequence;
} jafr_ble_audio_status_t;

esp_err_t jafr_ble_audio_init(void);
bool jafr_ble_audio_submit_pcm(const int16_t *samples, size_t sample_count);
bool jafr_ble_audio_is_ready(void);
void jafr_ble_audio_get_status(jafr_ble_audio_status_t *status);

