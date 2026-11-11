#include "jafr_cloud_voice.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "lwip/netdb.h"
#include "nvs.h"

#define CLOUD_HOST "api.mistral.ai"
#define STT_URI "wss://" CLOUD_HOST "/v1/audio/transcriptions/realtime?model=voxtral-mini-transcribe-realtime-3"
#define TTS_URI "wss://api.mistral.ai/v1/audio/speech/realtime?model=voxtral-mini-tts-3-0"
#define CHAT_URI "https://api.mistral.ai/v1/chat/completions"
#define CHAT_MODEL "mistral-small-latest"
#define TTS_VOICE "voice_447"

#define CLOUD_READY_STT BIT0
#define CLOUD_READY_TTS BIT1
#define CLOUD_RECONFIGURE BIT2
#define CLOUD_TURN_READY BIT3
#define CLOUD_BARGE_IN BIT4
#define CLOUD_TRANSPORT_FAILED BIT5
#define CLOUD_TTS_IDLE BIT6

#define MIC_BATCH_SAMPLES 320 /* 20 ms at 16 kHz */
#define MIC_JSON_PREFIX "{\"type\":\"input_audio.append\",\"audio\":\""
#define MIC_BASE64_CAP (((MIC_BATCH_SAMPLES * sizeof(int16_t) + 2) / 3) * 4 + 1)
#define TRANSCRIPT_CAP 2048
#define RESPONSE_CAP 3072
#define WS_MESSAGE_CAP 12288
#define HISTORY_TURNS 8
#define HISTORY_TEXT_CAP 220
#define PERSONA_CAP 1536
#define FACTS_CAP 768
#define PLAYBACK_STREAM_BYTES 12288
#define PLAYBACK_READ_SAMPLES 640

static const char *TAG = "JAFR_CLOUD";
static const char *DEFAULT_PERSONA =
    "You are Jaf-R, pronounced Jafr: Bladder's sharp British desktop companion and robotics co-pilot. "
    "Speak naturally, warmly and confidently. Light teasing is welcome. Your DualEye displays are your eyes; "
    "their listening, thinking, speaking and emotional reactions happen automatically. Never describe yourself "
    "as an AI, never narrate LED colours, and never claim a physical action succeeded unless the device reports it.";
static const char *VOICE_RULES =
    "Voice delivery rules: default to one to three short spoken sentences and front-load the answer. "
    "Expand only when explicitly asked for detail, a story, or an explanation. Keep one idea per sentence. "
    "Use natural contractions and punctuation that gives speech room to breathe. Never use markdown, lists, "
    "URLs, emoji, stage directions, or document-like wording. Do not routinely ask whether the user wants more.";

typedef struct {
    char user[HISTORY_TEXT_CAP];
    char assistant[HISTORY_TEXT_CAP];
} history_turn_t;

typedef struct {
    char *data;
    size_t cap;
    size_t len;
} ws_message_t;

typedef struct {
    char line[6144];
    size_t len;
    char response[RESPONSE_CAP];
    size_t response_len;
} sse_context_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_mic_transport_lock;
static EventGroupHandle_t s_events;
static StreamBufferHandle_t s_playback_stream;
static StaticStreamBuffer_t s_playback_stream_control;
static uint8_t *s_playback_stream_storage;
static TaskHandle_t s_playback_task;
static jafr_cloud_status_t s_status;
static char s_api_key[192];
static char s_persona[PERSONA_CAP];
static char s_facts[FACTS_CAP];
static history_turn_t s_history[HISTORY_TURNS];
static size_t s_history_start;
static size_t s_history_count;
static esp_websocket_client_handle_t s_stt;
static esp_websocket_client_handle_t s_tts;
static ws_message_t s_stt_message;
static ws_message_t s_tts_message;
static int16_t s_mic_batch[MIC_BATCH_SAMPLES];
/* Written only by the microphone transport task. */
static char s_mic_json[sizeof(MIC_JSON_PREFIX) - 1 + MIC_BASE64_CAP + 2] = MIC_JSON_PREFIX;
static size_t s_mic_count;
static char s_transcript[TRANSCRIPT_CAP];
static size_t s_transcript_len;
static volatile int64_t s_turn_finalize_at_us;
static volatile int64_t s_turn_end_us;
static volatile int64_t s_chat_start_us;
static volatile int64_t s_first_text_us;
static volatile int64_t s_first_audio_us;
static volatile int64_t s_retry_at_us;
static volatile bool s_cancel_generation;
static volatile bool s_stopping_websockets;
static volatile bool s_pending_auth_failure;
static jafr_cloud_audio_cb_t s_audio_cb;
static jafr_cloud_state_cb_t s_state_cb;
static int16_t s_resample_tail[2];
static size_t s_resample_tail_count;

static void set_state(jafr_cloud_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    xSemaphoreGive(s_lock);
    if (s_state_cb) s_state_cb(state);
}

static void set_error(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = JAFR_CLOUD_ERROR;
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s",
             message ? message : "unknown cloud error");
    xSemaphoreGive(s_lock);
    ESP_LOGE(TAG, "%s", message ? message : "unknown cloud error");
    if (s_state_cb) s_state_cb(JAFR_CLOUD_ERROR);
}

static void *cloud_malloc(size_t bytes)
{
    void *memory = heap_caps_get_total_size(MALLOC_CAP_SPIRAM)
        ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    return memory ? memory : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

static void copy_compact(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    destination[0] = '\0';
    if (!source) return;
    size_t out = 0;
    bool pending_space = false;
    while (*source && out + 1 < capacity) {
        unsigned char c = (unsigned char)*source++;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            pending_space = out > 0;
            continue;
        }
        if (pending_space && out + 1 < capacity) destination[out++] = ' ';
        pending_space = false;
        destination[out++] = (char)c;
    }
    destination[out] = '\0';
}

static void history_add(const char *user, const char *assistant)
{
    if (!user || !*user || !assistant || !*assistant) return;
    size_t slot;
    if (s_history_count < HISTORY_TURNS) {
        slot = (s_history_start + s_history_count) % HISTORY_TURNS;
        s_history_count++;
    } else {
        slot = s_history_start;
        s_history_start = (s_history_start + 1) % HISTORY_TURNS;
    }
    copy_compact(s_history[slot].user, sizeof(s_history[slot].user), user);
    copy_compact(s_history[slot].assistant, sizeof(s_history[slot].assistant), assistant);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.history_turns = (uint32_t)s_history_count;
    xSemaphoreGive(s_lock);
}

static void mark_direct_ready(void)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    if ((bits & (CLOUD_READY_STT | CLOUD_READY_TTS)) !=
        (CLOUD_READY_STT | CLOUD_READY_TTS)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.direct_active = true;
    s_status.fallback_active = false;
    s_status.provisioning_required = false;
    s_status.consecutive_failures = 0;
    s_status.last_http_status = 0;
    s_status.last_error[0] = '\0';
    s_status.state = JAFR_CLOUD_LISTENING;
    xSemaphoreGive(s_lock);
    if (s_state_cb) s_state_cb(JAFR_CLOUD_LISTENING);
}

static void record_transport_failure(const char *message, int http_status)
{
    const bool auth_failure = http_status == 401 || http_status == 403;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = JAFR_CLOUD_ERROR;
    s_status.direct_active = false;
    s_status.fallback_active = true;
    s_status.last_http_status = http_status;
    s_status.consecutive_failures++;
    if (auth_failure) {
        s_status.auth_failures++;
        s_status.provisioning_required = true;
        s_pending_auth_failure = true;
    }
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s",
             message ? message : "cloud transport failure");
    xSemaphoreGive(s_lock);
    if (http_status) ESP_LOGE(TAG, "%s HTTP %d", message, http_status);
    else ESP_LOGE(TAG, "%s", message ? message : "cloud transport failure");
    if (s_state_cb) s_state_cb(JAFR_CLOUD_ERROR);
    xEventGroupSetBits(s_events, CLOUD_TRANSPORT_FAILED);
}

const char *jafr_cloud_state_name(jafr_cloud_state_t state)
{
    switch (state) {
    case JAFR_CLOUD_CONNECTING: return "connecting";
    case JAFR_CLOUD_LISTENING: return "listening";
    case JAFR_CLOUD_THINKING: return "thinking";
    case JAFR_CLOUD_SPEAKING: return "speaking";
    case JAFR_CLOUD_ERROR: return "error";
    case JAFR_CLOUD_DISABLED:
    default: return "disabled";
    }
}

static void ws_message_reset(ws_message_t *message)
{
    message->len = 0;
    if (message->data && message->cap) message->data[0] = '\0';
}

static bool ws_message_append(ws_message_t *message, const esp_websocket_event_data_t *event)
{
    if (event->payload_len <= 0 || event->payload_len >= WS_MESSAGE_CAP) return false;
    size_t required = (size_t)event->payload_len + 1;
    if (required > message->cap) {
        char *next = realloc(message->data, required);
        if (!next) return false;
        message->data = next;
        message->cap = required;
    }
    if (event->payload_offset == 0) message->len = 0;
    if ((size_t)event->payload_offset + (size_t)event->data_len >= message->cap) return false;
    memcpy(message->data + event->payload_offset, event->data_ptr, event->data_len);
    size_t end = (size_t)event->payload_offset + (size_t)event->data_len;
    if (end > message->len) message->len = end;
    message->data[message->len] = '\0';
    return end == (size_t)event->payload_len;
}

static int websocket_send_json(esp_websocket_client_handle_t client, cJSON *root)
{
    if (!client || !root || !esp_websocket_client_is_connected(client)) return -1;
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return -1;
    int sent = esp_websocket_client_send_text(client, json, strlen(json), pdMS_TO_TICKS(3000));
    cJSON_free(json);
    return sent;
}

static void configure_stt(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *format = cJSON_AddObjectToObject(session, "audio_format");
    cJSON_AddStringToObject(format, "encoding", "pcm_s16le");
    cJSON_AddNumberToObject(format, "sample_rate", 16000);
    cJSON_AddNumberToObject(session, "target_streaming_delay_ms", 320);
    cJSON_AddStringToObject(session, "language", "en");
    cJSON *turn = cJSON_AddObjectToObject(session, "turn_detection");
    cJSON_AddNumberToObject(turn, "start_threshold", 0.5);
    cJSON_AddNumberToObject(turn, "eager_end_threshold", 0.4);
    cJSON_AddNumberToObject(turn, "end_threshold", 0.33);
    cJSON_AddNumberToObject(turn, "min_silence_ms", 450);
    cJSON_AddNumberToObject(turn, "end_timeout_ms", 2500);
    if (websocket_send_json(s_stt, root) < 0) set_error("STT session configuration failed");
    cJSON_Delete(root);
}

static void configure_tts(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(session, "voice", TTS_VOICE);
    cJSON_AddStringToObject(session, "streaming_mode", "responsive");
    cJSON_AddStringToObject(session, "response_format", "pcm");
    if (websocket_send_json(s_tts, root) < 0) set_error("TTS session configuration failed");
    cJSON_Delete(root);
}

static void playback_task(void *arg)
{
    (void)arg;
    int16_t samples[PLAYBACK_READ_SAMPLES];
    while (true) {
        size_t bytes = xStreamBufferReceive(s_playback_stream, samples, sizeof(samples),
                                            pdMS_TO_TICKS(250));
        if (bytes && s_audio_cb && !s_cancel_generation) {
            s_audio_cb(samples, bytes / sizeof(int16_t));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.playback_stack_free = uxTaskGetStackHighWaterMark(NULL);
        xSemaphoreGive(s_lock);
    }
}

static void dispatch_tts_pcm(const uint8_t *pcm, size_t bytes)
{
    // Voxtral raw PCM is float32 LE at 24 kHz; the speaker takes signed PCM16.
    if (!s_audio_cb || !pcm || !bytes || bytes % sizeof(float)) return;
    size_t incoming = bytes / sizeof(float);
    size_t total = s_resample_tail_count + incoming;
    int16_t *combined = cloud_malloc(total * sizeof(int16_t));
    if (!combined) return;
    memcpy(combined, s_resample_tail, s_resample_tail_count * sizeof(int16_t));
    for (size_t i = 0; i < incoming; ++i) {
        float sample;
        memcpy(&sample, pcm + i * sizeof(sample), sizeof(sample));
        if (!isfinite(sample)) sample = 0.0f;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        combined[s_resample_tail_count + i] = (int16_t)(sample * 32767.0f);
    }

    size_t groups = total / 3;
    size_t out_count = groups * 2;
    int16_t *out = cloud_malloc(out_count * sizeof(int16_t));
    if (!out) {
        free(combined);
        return;
    }
    for (size_t i = 0; i < groups; ++i) {
        int32_t a = combined[i * 3];
        int32_t b = combined[i * 3 + 1];
        int32_t c = combined[i * 3 + 2];
        out[i * 2] = (int16_t)a;
        out[i * 2 + 1] = (int16_t)((b + c) / 2);
    }
    s_resample_tail_count = total - groups * 3;
    if (s_resample_tail_count) {
        memcpy(s_resample_tail, combined + groups * 3,
               s_resample_tail_count * sizeof(int16_t));
    }
    free(combined);
    if (!s_cancel_generation && out_count) {
        const size_t out_bytes = out_count * sizeof(int16_t);
        size_t queued = xStreamBufferSend(s_playback_stream, out, out_bytes,
                                          pdMS_TO_TICKS(80));
        if (queued != out_bytes) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.playback_queue_drops++;
            xSemaphoreGive(s_lock);
        }
    }
    free(out);
}

static void parse_tts_event(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "session.created") == 0) {
        configure_tts();
    } else if (strcmp(type->valuestring, "session.updated") == 0) {
        xEventGroupSetBits(s_events, CLOUD_READY_TTS | CLOUD_TTS_IDLE);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.tts_connected = true;
        xSemaphoreGive(s_lock);
        mark_direct_ready();
    } else if (strcmp(type->valuestring, "speech.audio.delta") == 0) {
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_data");
        if (cJSON_IsString(audio) && !s_cancel_generation) {
            size_t source_len = strlen(audio->valuestring);
            size_t cap = (source_len * 3) / 4 + 4;
            uint8_t *decoded = cloud_malloc(cap);
            size_t decoded_len = 0;
            if (decoded && mbedtls_base64_decode(decoded, cap, &decoded_len,
                    (const unsigned char *)audio->valuestring, source_len) == 0) {
                if (s_first_audio_us == 0) {
                    s_first_audio_us = esp_timer_get_time();
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_status.text_to_first_audio_ms = s_first_text_us > 0
                        ? (uint32_t)((s_first_audio_us - s_first_text_us) / 1000) : 0;
                    s_status.total_first_audio_ms = s_turn_end_us > 0
                        ? (uint32_t)((s_first_audio_us - s_turn_end_us) / 1000) : 0;
                    xSemaphoreGive(s_lock);
                }
                set_state(JAFR_CLOUD_SPEAKING);
                dispatch_tts_pcm(decoded, decoded_len);
            }
            free(decoded);
        }
    } else if (strcmp(type->valuestring, "speech.audio.done") == 0) {
        xEventGroupSetBits(s_events, CLOUD_TTS_IDLE);
        if (!s_cancel_generation) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.turns_completed++;
            xSemaphoreGive(s_lock);
            set_state(JAFR_CLOUD_LISTENING);
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *message = error ? cJSON_GetObjectItemCaseSensitive(error, "message") : NULL;
        record_transport_failure(cJSON_IsString(message) ? message->valuestring : "TTS service error", 0);
    }
    cJSON_Delete(root);
}

/* Mistral audio deltas can exceed the control-message limit by an order of
 * magnitude. Decode the audio_data string as the WebSocket delivers it. */
static bool stream_tts_delta(const esp_websocket_event_data_t *event)
{
    static bool audio_frame;
    static bool audio_ended;
    static unsigned char encoded[512];
    static size_t used;
    const char *data = event->data_ptr;
    const char *end = data + event->data_len;
    if (event->payload_offset == 0) {
        audio_frame = false;
        audio_ended = false;
        used = 0;
        const char field[] = "\"audio_data\"";
        for (; (size_t)(end - data) >= sizeof(field) - 1; ++data) {
            if (memcmp(data, field, sizeof(field) - 1) == 0) break;
        }
        if ((size_t)(end - data) < sizeof(field) - 1) return false;
        data += sizeof(field) - 1;
        while (data < end && (*data == ' ' || *data == ':' || *data == '\t')) ++data;
        if (data == end || *data++ != '"') return false;
        audio_frame = true;
    }
    if (!audio_frame) return false;
    if (audio_ended || s_cancel_generation) return true;
    for (; data < end; ++data) {
        if (*data == '"') audio_ended = true;
        else encoded[used++] = (unsigned char)*data;
        if (used == sizeof(encoded) || (audio_ended && used)) {
            uint8_t pcm[384];
            size_t bytes = 0;
            int result = mbedtls_base64_decode(pcm, sizeof(pcm), &bytes, encoded, used);
            used = 0;
            if (result || bytes % sizeof(float)) {
                audio_ended = true;
                break;
            }
            if (s_first_audio_us == 0) {
                s_first_audio_us = esp_timer_get_time();
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_first_text_us > 0) s_status.text_to_first_audio_ms =
                    (uint32_t)((s_first_audio_us - s_first_text_us) / 1000);
                if (s_turn_end_us > 0) s_status.total_first_audio_ms =
                    (uint32_t)((s_first_audio_us - s_turn_end_us) / 1000);
                xSemaphoreGive(s_lock);
                set_state(JAFR_CLOUD_SPEAKING);
            }
            dispatch_tts_pcm(pcm, bytes);
        }
        if (audio_ended || s_cancel_generation) break;
    }
    return true;
}

static void parse_stt_event(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "session.created") == 0) {
        configure_stt();
    } else if (strcmp(type->valuestring, "session.updated") == 0) {
        xEventGroupSetBits(s_events, CLOUD_READY_STT);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.stt_connected = true;
        xSemaphoreGive(s_lock);
        mark_direct_ready();
    } else if (strcmp(type->valuestring, "transcription.text.delta") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            size_t len = strlen(text->valuestring);
            if (s_transcript_len + len < sizeof(s_transcript)) {
                memcpy(s_transcript + s_transcript_len, text->valuestring, len + 1);
                s_transcript_len += len;
            }
        }
    } else if (strcmp(type->valuestring, "turn.start") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        jafr_cloud_state_t state = s_status.state;
        xSemaphoreGive(s_lock);
        if (state == JAFR_CLOUD_SPEAKING || state == JAFR_CLOUD_THINKING) {
            xEventGroupSetBits(s_events, CLOUD_BARGE_IN);
        }
        if (s_transcript_len == 0) s_transcript[0] = '\0';
        set_state(JAFR_CLOUD_LISTENING);
    } else if (strcmp(type->valuestring, "turn.resume") == 0) {
        s_turn_finalize_at_us = 0;
    } else if (strcmp(type->valuestring, "turn.end") == 0) {
        s_turn_end_us = esp_timer_get_time();
        s_turn_finalize_at_us = s_turn_end_us + 400000;
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *message = error ? cJSON_GetObjectItemCaseSensitive(error, "message") : NULL;
        record_transport_failure(cJSON_IsString(message) ? message->valuestring : "STT service error", 0);
    }
    cJSON_Delete(root);
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    if (s_stopping_websockets) return;
    ws_message_t *message = arg;
    esp_websocket_event_data_t *event = event_data;
    const bool is_stt = message == &s_stt_message;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "%s websocket connected", is_stt ? "STT" : "TTS");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (is_stt) s_status.stt_connected = false;
        else s_status.tts_connected = false;
        s_status.direct_active = false;
        s_status.fallback_active = true;
        s_status.reconnects++;
        xSemaphoreGive(s_lock);
        xEventGroupClearBits(s_events, is_stt ? CLOUD_READY_STT : (CLOUD_READY_TTS | CLOUD_TTS_IDLE));
        xEventGroupSetBits(s_events, CLOUD_TRANSPORT_FAILED);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        int http_status = event ? event->error_handle.esp_ws_handshake_status_code : 0;
        record_transport_failure(is_stt ? "STT websocket transport error" :
                                           "TTS websocket transport error", http_status);
    } else if (event_id == WEBSOCKET_EVENT_DATA && event && event->op_code == 0x1) {
        if (!is_stt && event->data_len > 0 && stream_tts_delta(event)) return;
        if (ws_message_append(message, event)) {
            if (is_stt) parse_stt_event(message->data);
            else parse_tts_event(message->data);
            ws_message_reset(message);
        }
    }
}

static void destroy_websockets(void)
{
    // A microphone write may still be using the STT client after route loss.
    xSemaphoreTake(s_mic_transport_lock, portMAX_DELAY);
    s_stopping_websockets = true;
    if (s_stt) {
        esp_websocket_client_stop(s_stt);
        esp_websocket_client_destroy(s_stt);
        s_stt = NULL;
    }
    if (s_tts) {
        esp_websocket_client_stop(s_tts);
        esp_websocket_client_destroy(s_tts);
        s_tts = NULL;
    }
    xEventGroupClearBits(s_events, CLOUD_READY_STT | CLOUD_READY_TTS | CLOUD_TTS_IDLE);
    ws_message_reset(&s_stt_message);
    ws_message_reset(&s_tts_message);
    s_turn_finalize_at_us = 0;
    s_transcript_len = 0;
    s_transcript[0] = '\0';
    s_mic_count = 0;
    xEventGroupClearBits(s_events, CLOUD_TURN_READY | CLOUD_BARGE_IN | CLOUD_TRANSPORT_FAILED);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.stt_connected = false;
    s_status.tts_connected = false;
    s_status.direct_active = false;
    s_status.fallback_active = s_status.mode == JAFR_VOICE_MODE_DIRECT;
    xSemaphoreGive(s_lock);
    s_stopping_websockets = false;
    xSemaphoreGive(s_mic_transport_lock);
}

static bool start_websockets(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.direct_active = false;
    s_status.fallback_active = true;
    s_status.state = JAFR_CLOUD_CONNECTING;
    xSemaphoreGive(s_lock);
    char headers[240];
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", s_api_key);
    esp_websocket_client_config_t stt_config = {
        .uri = STT_URI,
        .headers = headers,
        .user_context = &s_stt_message,
        .task_name = "mistral_stt_ws",
        .task_stack = 8192,
        .task_prio = 4,
        .buffer_size = 6144,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
    };
    esp_websocket_client_config_t tts_config = stt_config;
    tts_config.uri = TTS_URI;
    tts_config.user_context = &s_tts_message;
    tts_config.task_name = "mistral_tts_ws";

    s_stt = esp_websocket_client_init(&stt_config);
    s_tts = esp_websocket_client_init(&tts_config);
    if (!s_stt || !s_tts) {
        destroy_websockets();
        return false;
    }
    esp_websocket_register_events(s_stt, WEBSOCKET_EVENT_ANY, websocket_event, &s_stt_message);
    esp_websocket_register_events(s_tts, WEBSOCKET_EVENT_ANY, websocket_event, &s_tts_message);
    if (esp_websocket_client_start(s_stt) != ESP_OK ||
        esp_websocket_client_start(s_tts) != ESP_OK) {
        destroy_websockets();
        return false;
    }
    return true;
}

static bool resolver_has_ipv4(esp_netif_t *netif, esp_netif_dns_type_t slot,
                              esp_netif_dns_info_t *dns)
{
    return esp_netif_get_dns_info(netif, slot, dns) == ESP_OK &&
           dns->ip.type == ESP_IPADDR_TYPE_V4 && dns->ip.u_addr.ip4.addr != 0;
}

static void start_cloud_transport(void)
{
    esp_netif_t *wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t lease = {0};
    if (!wifi || !esp_netif_is_netif_up(wifi) ||
        esp_netif_get_ip_info(wifi, &lease) != ESP_OK || !lease.ip.addr || !lease.gw.addr) {
        set_error("Waiting for Wi-Fi IPv4 and router");
        s_retry_at_us = esp_timer_get_time() + 2000000;
        return;
    }

    esp_netif_dns_info_t dns = {0};
    if (!resolver_has_ipv4(wifi, ESP_NETIF_DNS_MAIN, &dns)) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4 = lease.gw;
        esp_err_t err = esp_netif_set_dns_info(wifi, ESP_NETIF_DNS_MAIN, &dns);
        if (err != ESP_OK) ESP_LOGW(TAG, "Router DNS setup: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Cloud DNS primary " IPSTR, IP2STR(&dns.ip.u_addr.ip4));

    // Retain DHCP resolvers. Fill the next empty slot so lwIP can advance to
    // it even when the lease supplies only a single DNS server.
    const uint32_t public_dns = esp_ip4addr_aton("1.1.1.1");
    bool has_backup = resolver_has_ipv4(wifi, ESP_NETIF_DNS_BACKUP, &dns);
    bool has_public_backup = has_backup && dns.ip.u_addr.ip4.addr == public_dns;
    esp_netif_dns_type_t spare = has_backup
        ? ESP_NETIF_DNS_FALLBACK : ESP_NETIF_DNS_BACKUP;
    if (!has_public_backup && !resolver_has_ipv4(wifi, spare, &dns)) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = public_dns;
        esp_err_t err = esp_netif_set_dns_info(wifi, spare, &dns);
        if (err != ESP_OK) ESP_LOGW(TAG, "Backup DNS setup: %s", esp_err_to_name(err));
    }

    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *resolved = NULL;
    int result = getaddrinfo(CLOUD_HOST, NULL, &hints, &resolved);
    if (result != 0 || !resolved) {
        ESP_LOGW(TAG, "Cloud DNS lookup failed: %d", result);
        record_transport_failure("DNS lookup failed for api.mistral.ai", 0);
        return;
    }
    freeaddrinfo(resolved);
    ESP_LOGI(TAG, "Cloud DNS lookup succeeded");
    if (!start_websockets()) {
        record_transport_failure("Cloud websocket startup failed", 0);
    }
}

static void tts_append(const char *text)
{
    if (!text || !*text || !s_tts || !esp_websocket_client_is_connected(s_tts)) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "input_text.append");
    cJSON_AddStringToObject(root, "text", text);
    websocket_send_json(s_tts, root);
    cJSON_Delete(root);
}

static void tts_flush(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "input_text.flush");
    websocket_send_json(s_tts, root);
    cJSON_Delete(root);
}

static void handle_chat_sse_line(sse_context_t *ctx, const char *line)
{
    if (strncmp(line, "data: ", 6) != 0) return;
    line += 6;
    if (strcmp(line, "[DONE]") == 0) return;
    cJSON *root = cJSON_Parse(line);
    if (!root) return;
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItemCaseSensitive(choice, "delta") : NULL;
    cJSON *content = delta ? cJSON_GetObjectItemCaseSensitive(delta, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring[0]) {
        if (s_first_text_us == 0) {
            s_first_text_us = esp_timer_get_time();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.chat_to_first_text_ms = s_chat_start_us > 0
                ? (uint32_t)((s_first_text_us - s_chat_start_us) / 1000) : 0;
            xSemaphoreGive(s_lock);
        }
        tts_append(content->valuestring);
        size_t len = strlen(content->valuestring);
        size_t room = sizeof(ctx->response) - ctx->response_len - 1;
        if (len > room) len = room;
        if (len) {
            memcpy(ctx->response + ctx->response_len, content->valuestring, len);
            ctx->response_len += len;
            ctx->response[ctx->response_len] = '\0';
        }
    }
    cJSON_Delete(root);
}

static esp_err_t chat_http_event(esp_http_client_event_t *event)
{
    sse_context_t *ctx = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !ctx) return ESP_OK;
    if (s_cancel_generation) return ESP_FAIL;
    const char *data = event->data;
    for (int i = 0; i < event->data_len; ++i) {
        char c = data[i];
        if (c == '\n') {
            ctx->line[ctx->len] = '\0';
            if (ctx->len && ctx->line[ctx->len - 1] == '\r') ctx->line[--ctx->len] = '\0';
            handle_chat_sse_line(ctx, ctx->line);
            ctx->len = 0;
        } else if (ctx->len + 1 < sizeof(ctx->line)) {
            ctx->line[ctx->len++] = c;
        }
    }
    return ESP_OK;
}

static char *build_chat_request(const char *transcript)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CHAT_MODEL);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    cJSON_AddNumberToObject(root, "max_tokens", 180);
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", s_persona[0] ? s_persona : DEFAULT_PERSONA);
    cJSON_AddItemToArray(messages, system);
    system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", VOICE_RULES);
    cJSON_AddItemToArray(messages, system);
    if (s_facts[0]) {
        system = cJSON_CreateObject();
        cJSON_AddStringToObject(system, "role", "system");
        cJSON_AddStringToObject(system, "content", s_facts);
        cJSON_AddItemToArray(messages, system);
    }
    for (size_t i = 0; i < s_history_count; ++i) {
        const history_turn_t *turn = &s_history[(s_history_start + i) % HISTORY_TURNS];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", "user");
        cJSON_AddStringToObject(item, "content", turn->user);
        cJSON_AddItemToArray(messages, item);
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "role", "assistant");
        cJSON_AddStringToObject(item, "content", turn->assistant);
        cJSON_AddItemToArray(messages, item);
    }
    xSemaphoreGive(s_lock);
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", transcript);
    cJSON_AddItemToArray(messages, user);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static bool run_chat_turn(const char *transcript)
{
    if (!transcript || !*transcript) return false;
    EventBits_t ready = xEventGroupWaitBits(s_events, CLOUD_READY_TTS | CLOUD_TTS_IDLE,
                                            pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(10000));
    if ((ready & (CLOUD_READY_TTS | CLOUD_TTS_IDLE)) !=
        (CLOUD_READY_TTS | CLOUD_TTS_IDLE)) {
        record_transport_failure("TTS websocket did not become idle", 0);
        return false;
    }
    for (int i = 0; i < 100 && xStreamBufferBytesAvailable(s_playback_stream) > 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    char *body = build_chat_request(transcript);
    if (!body) return false;
    char auth[224];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    sse_context_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        cJSON_free(body);
        return false;
    }
    esp_http_client_config_t config = {
        .url = CHAT_URI,
        .event_handler = chat_http_event,
        .user_data = ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(ctx);
        cJSON_free(body);
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_post_field(client, body, strlen(body));
    s_chat_start_us = esp_timer_get_time();
    s_first_text_us = 0;
    s_first_audio_us = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.turn_to_chat_ms = s_turn_end_us > 0
        ? (uint32_t)((s_chat_start_us - s_turn_end_us) / 1000) : 0;
    s_status.chat_to_first_text_ms = 0;
    s_status.text_to_first_audio_ms = 0;
    s_status.total_first_audio_ms = 0;
    xSemaphoreGive(s_lock);
    xEventGroupClearBits(s_events, CLOUD_TTS_IDLE);
    set_state(JAFR_CLOUD_THINKING);
    s_cancel_generation = false;
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (ctx->len) {
        ctx->line[ctx->len] = '\0';
        handle_chat_sse_line(ctx, ctx->line);
    }
    const bool completed = result == ESP_OK && status == 200 && !s_cancel_generation;
    if (completed) {
        tts_flush();
        history_add(transcript, ctx->response);
    } else if (s_cancel_generation) {
        /* Drain the abandoned generation; audio deltas remain muted until done. */
        tts_flush();
    }
    if (result != ESP_OK && status == 200 && !s_cancel_generation) {
        record_transport_failure("Mistral chat stream failed", status);
    } else if (status != 200 && !s_cancel_generation) {
        char error[64];
        snprintf(error, sizeof(error), "Mistral chat HTTP %d", status);
        record_transport_failure(error, status);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.last_http_status = status;
    xSemaphoreGive(s_lock);
    esp_http_client_cleanup(client);
    free(ctx);
    cJSON_free(body);
    return completed;
}

static bool direct_selected(void)
{
    bool selected;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    selected = s_status.mode == JAFR_VOICE_MODE_DIRECT;
    xSemaphoreGive(s_lock);
    return selected;
}

static bool key_is_configured(void)
{
    bool configured;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    configured = s_status.key_configured;
    xSemaphoreGive(s_lock);
    return configured;
}

static void cloud_manager_task(void *arg)
{
    (void)arg;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_events,
            CLOUD_RECONFIGURE | CLOUD_TURN_READY | CLOUD_BARGE_IN | CLOUD_TRANSPORT_FAILED,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(50));

        if (bits & CLOUD_BARGE_IN) {
            s_cancel_generation = true;
            s_resample_tail_count = 0;
            set_state(JAFR_CLOUD_LISTENING);
        }
        if (bits & CLOUD_TRANSPORT_FAILED) {
            destroy_websockets();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint32_t failures = s_status.consecutive_failures;
            const bool auth_failure = s_pending_auth_failure;
            s_pending_auth_failure = false;
            s_status.direct_active = false;
            s_status.fallback_active = true;
            xSemaphoreGive(s_lock);
            if (auth_failure) {
                s_retry_at_us = 0;
            } else {
                uint32_t shift = failures > 6 ? 6 : failures;
                s_retry_at_us = esp_timer_get_time() + ((int64_t)1 << shift) * 1000000LL;
            }
        }
        if (bits & CLOUD_RECONFIGURE) {
            destroy_websockets();
            s_retry_at_us = 0;
            if (direct_selected() && key_is_configured()) {
                set_state(JAFR_CLOUD_CONNECTING);
                start_cloud_transport();
            } else if (direct_selected()) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.provisioning_required = true;
                s_status.fallback_active = true;
                s_status.direct_active = false;
                xSemaphoreGive(s_lock);
                set_error("Mistral key required - provisioning needed");
            } else {
                set_state(JAFR_CLOUD_DISABLED);
            }
        }
        if (s_retry_at_us && esp_timer_get_time() >= s_retry_at_us &&
            direct_selected() && key_is_configured()) {
            s_retry_at_us = 0;
            set_state(JAFR_CLOUD_CONNECTING);
            start_cloud_transport();
        }
        if (s_turn_finalize_at_us && esp_timer_get_time() >= s_turn_finalize_at_us) {
            s_turn_finalize_at_us = 0;
            if (s_transcript_len) xEventGroupSetBits(s_events, CLOUD_TURN_READY);
        }
        if ((bits & CLOUD_TURN_READY) && jafr_cloud_voice_is_direct()) {
            char transcript[TRANSCRIPT_CAP];
            memcpy(transcript, s_transcript, s_transcript_len + 1);
            s_transcript_len = 0;
            s_transcript[0] = '\0';
            run_chat_turn(transcript);
        }
    }
}

static void load_config(void)
{
    nvs_handle_t handle;
    uint8_t mode = JAFR_VOICE_MODE_PC;
    if (nvs_open("jafr_cloud", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "mode", &mode);
        size_t length = sizeof(s_api_key);
        if (nvs_get_str(handle, "api_key", s_api_key, &length) != ESP_OK) s_api_key[0] = '\0';
        length = sizeof(s_persona);
        if (nvs_get_str(handle, "persona", s_persona, &length) != ESP_OK) s_persona[0] = '\0';
        length = sizeof(s_facts);
        if (nvs_get_str(handle, "facts", s_facts, &length) != ESP_OK) s_facts[0] = '\0';
        nvs_close(handle);
    }
    if (!s_persona[0]) snprintf(s_persona, sizeof(s_persona), "%s", DEFAULT_PERSONA);
    if (mode > JAFR_VOICE_MODE_DIRECT) mode = JAFR_VOICE_MODE_PC;
    s_status.mode = (jafr_voice_mode_t)mode;
    s_status.key_configured = s_api_key[0] != '\0';
    s_status.direct_active = false;
    s_status.fallback_active = mode == JAFR_VOICE_MODE_DIRECT;
    s_status.provisioning_required = mode == JAFR_VOICE_MODE_DIRECT && !s_status.key_configured;
}

esp_err_t jafr_cloud_voice_init(jafr_cloud_audio_cb_t audio_cb,
                                jafr_cloud_state_cb_t state_cb)
{
    s_lock = xSemaphoreCreateMutex();
    s_mic_transport_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_mic_transport_lock || !s_events) return ESP_ERR_NO_MEM;
    s_playback_stream_storage = heap_caps_malloc(PLAYBACK_STREAM_BYTES + 1,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_playback_stream_storage) {
        s_playback_stream_storage = heap_caps_malloc(PLAYBACK_STREAM_BYTES + 1,
                                                     MALLOC_CAP_8BIT);
    }
    if (!s_playback_stream_storage) return ESP_ERR_NO_MEM;
    s_playback_stream = xStreamBufferCreateStatic(PLAYBACK_STREAM_BYTES, sizeof(int16_t),
                                                   s_playback_stream_storage,
                                                   &s_playback_stream_control);
    if (!s_playback_stream) return ESP_ERR_NO_MEM;
    s_audio_cb = audio_cb;
    s_state_cb = state_cb;
    load_config();
    if (xTaskCreatePinnedToCore(cloud_manager_task, "cloud_voice", 12288, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(playback_task, "cloud_playback", 4096, NULL, 5,
                                &s_playback_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_events, CLOUD_RECONFIGURE);
    return ESP_OK;
}

bool jafr_cloud_voice_is_direct(void)
{
    bool direct;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    direct = s_status.direct_active;
    xSemaphoreGive(s_lock);
    return direct;
}

static bool submit_pcm_locked(const int16_t *samples, size_t sample_count)
{
    if (!samples || !sample_count || !jafr_cloud_voice_is_direct()) return false;
    EventBits_t ready = xEventGroupGetBits(s_events);
    if (!(ready & CLOUD_READY_STT)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.audio_chunks_dropped++;
        xSemaphoreGive(s_lock);
        return false;
    }
    while (sample_count) {
        size_t take = MIC_BATCH_SAMPLES - s_mic_count;
        if (take > sample_count) take = sample_count;
        memcpy(s_mic_batch + s_mic_count, samples, take * sizeof(int16_t));
        s_mic_count += take;
        samples += take;
        sample_count -= take;
        if (s_mic_count == MIC_BATCH_SAMPLES) {
            const size_t prefix_len = sizeof(MIC_JSON_PREFIX) - 1;
            unsigned char *encoded = (unsigned char *)s_mic_json + prefix_len;
            size_t encoded_len = 0;
            bool sent_ok = false;
            if (mbedtls_base64_encode(encoded, MIC_BASE64_CAP, &encoded_len,
                    (const unsigned char *)s_mic_batch, sizeof(s_mic_batch)) == 0) {
                encoded[encoded_len] = '"';
                encoded[encoded_len + 1] = '}';
                const size_t message_len = prefix_len + encoded_len + 2;
                sent_ok = esp_websocket_client_send_text(s_stt, s_mic_json, message_len,
                                                         pdMS_TO_TICKS(3000)) == (int)message_len;
            }
            s_mic_count = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (sent_ok) s_status.audio_chunks_sent++;
            else s_status.audio_chunks_dropped++;
            xSemaphoreGive(s_lock);
            if (!sent_ok) return false;
        }
    }
    return true;
}

bool jafr_cloud_voice_submit_pcm(const int16_t *samples, size_t sample_count)
{
    if (!s_mic_transport_lock || xSemaphoreTake(s_mic_transport_lock, 0) != pdTRUE) return false;
    bool sent = submit_pcm_locked(samples, sample_count);
    xSemaphoreGive(s_mic_transport_lock);
    return sent;
}

void jafr_cloud_voice_barge_in(void)
{
    if (s_events) xEventGroupSetBits(s_events, CLOUD_BARGE_IN);
}

esp_err_t jafr_cloud_voice_configure(jafr_voice_mode_t mode,
                                     const char *api_key,
                                     bool clear_key)
{
    if (mode > JAFR_VOICE_MODE_DIRECT || !s_lock) return ESP_ERR_INVALID_ARG;
    if (api_key && strlen(api_key) >= sizeof(s_api_key)) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("jafr_cloud", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (clear_key) {
        err = nvs_erase_key(handle, "api_key");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else if (api_key && api_key[0]) {
        err = nvs_set_str(handle, "api_key", api_key);
    }
    if (err == ESP_OK) err = nvs_set_u8(handle, "mode", (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (clear_key) s_api_key[0] = '\0';
    else if (api_key && api_key[0]) snprintf(s_api_key, sizeof(s_api_key), "%s", api_key);
    s_status.key_configured = s_api_key[0] != '\0';
    s_status.mode = mode;
    s_status.direct_active = false;
    s_status.fallback_active = mode == JAFR_VOICE_MODE_DIRECT;
    s_status.provisioning_required = mode == JAFR_VOICE_MODE_DIRECT && !s_status.key_configured;
    s_status.consecutive_failures = 0;
    s_status.last_http_status = 0;
    s_status.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    xEventGroupSetBits(s_events, CLOUD_RECONFIGURE);
    return ESP_OK;
}

esp_err_t jafr_cloud_voice_configure_persona(const char *persona, const char *facts)
{
    if (!s_lock || !persona || !facts) return ESP_ERR_INVALID_ARG;
    if (strlen(persona) >= sizeof(s_persona) || strlen(facts) >= sizeof(s_facts)) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("jafr_cloud", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "persona", persona[0] ? persona : DEFAULT_PERSONA);
    if (err == ESP_OK) err = nvs_set_str(handle, "facts", facts);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_persona, sizeof(s_persona), "%s", persona[0] ? persona : DEFAULT_PERSONA);
    snprintf(s_facts, sizeof(s_facts), "%s", facts);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void jafr_cloud_voice_get_persona(char *persona, size_t persona_size,
                                  char *facts, size_t facts_size)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (persona && persona_size) snprintf(persona, persona_size, "%s", s_persona);
    if (facts && facts_size) snprintf(facts, facts_size, "%s", s_facts);
    xSemaphoreGive(s_lock);
}

void jafr_cloud_voice_get_status(jafr_cloud_status_t *status)
{
    if (!status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}
