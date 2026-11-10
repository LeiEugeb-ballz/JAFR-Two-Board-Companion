#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    JAFR_VOICE_MODE_PC = 0,
    JAFR_VOICE_MODE_DIRECT = 1,
} jafr_voice_mode_t;

typedef enum {
    JAFR_CLOUD_DISABLED = 0,
    JAFR_CLOUD_CONNECTING,
    JAFR_CLOUD_LISTENING,
    JAFR_CLOUD_THINKING,
    JAFR_CLOUD_SPEAKING,
    JAFR_CLOUD_ERROR,
} jafr_cloud_state_t;

typedef struct {
    jafr_voice_mode_t mode;
    jafr_cloud_state_t state;
    bool key_configured;
    bool direct_active;
    bool fallback_active;
    bool provisioning_required;
    bool stt_connected;
    bool tts_connected;
    uint32_t audio_chunks_sent;
    uint32_t audio_chunks_dropped;
    uint32_t playback_queue_drops;
    uint32_t turns_completed;
    uint32_t history_turns;
    uint32_t reconnects;
    uint32_t consecutive_failures;
    uint32_t auth_failures;
    uint32_t turn_to_chat_ms;
    uint32_t chat_to_first_text_ms;
    uint32_t text_to_first_audio_ms;
    uint32_t total_first_audio_ms;
    uint32_t playback_stack_free;
    int last_http_status;
    char last_error[96];
} jafr_cloud_status_t;

typedef void (*jafr_cloud_audio_cb_t)(const int16_t *samples, size_t sample_count);
typedef void (*jafr_cloud_state_cb_t)(jafr_cloud_state_t state);

esp_err_t jafr_cloud_voice_init(jafr_cloud_audio_cb_t audio_cb,
                                jafr_cloud_state_cb_t state_cb);
bool jafr_cloud_voice_is_direct(void);
bool jafr_cloud_voice_submit_pcm(const int16_t *samples, size_t sample_count);
void jafr_cloud_voice_barge_in(void);
esp_err_t jafr_cloud_voice_configure(jafr_voice_mode_t mode,
                                     const char *api_key,
                                     bool clear_key);
esp_err_t jafr_cloud_voice_configure_persona(const char *persona,
                                             const char *facts);
void jafr_cloud_voice_get_persona(char *persona, size_t persona_size,
                                  char *facts, size_t facts_size);
void jafr_cloud_voice_get_status(jafr_cloud_status_t *status);
const char *jafr_cloud_state_name(jafr_cloud_state_t state);
