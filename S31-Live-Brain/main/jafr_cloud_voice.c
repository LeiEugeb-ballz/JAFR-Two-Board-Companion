#include "jafr_cloud_voice.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#define STT_URI "wss://api.mistral.ai/v1/audio/transcriptions/realtime?model=voxtral-mini-transcribe-realtime-3"
#define TTS_URI "wss://api.mistral.ai/v1/audio/speech/realtime?model=voxtral-mini-tts-3-0"
#define CHAT_URI "https://api.mistral.ai/v1/chat/completions"
#define CHAT_MODEL "mistral-small-latest"
#define TTS_VOICE "voice_447"

#define CLOUD_READY_STT BIT0
#define CLOUD_READY_TTS BIT1
#define CLOUD_RECONFIGURE BIT2
#define CLOUD_TURN_READY BIT3
#define CLOUD_BARGE_IN BIT4

#define MIC_BATCH_SAMPLES 1600 /* 100 ms at 16 kHz */
#define TRANSCRIPT_CAP 2048
#define RESPONSE_CAP 3072
#define WS_MESSAGE_CAP 12288

static const char *TAG = "JAFR_CLOUD";
static const char *SYSTEM_PROMPT =
    "You are Jaf-R, pronounced Jafr: Bladder's sharp British desktop companion and robotics co-pilot. "
    "Speak naturally, warmly and confidently in one to three concise sentences. Light teasing is welcome. "
    "Never describe yourself as an AI and never narrate LED colours. Your DualEye displays are your eyes; "
    "their listening, thinking, speaking and emotional reactions happen automatically. Do not claim a tool "
    "or physical action succeeded unless the device reports it.";

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
static EventGroupHandle_t s_events;
static jafr_cloud_status_t s_status;
static char s_api_key[192];
static esp_websocket_client_handle_t s_stt;
static esp_websocket_client_handle_t s_tts;
static ws_message_t s_stt_message;
static ws_message_t s_tts_message;
static int16_t s_mic_batch[MIC_BATCH_SAMPLES];
static size_t s_mic_count;
static char s_transcript[TRANSCRIPT_CAP];
static size_t s_transcript_len;
static volatile int64_t s_turn_finalize_at_us;
static volatile bool s_cancel_generation;
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

static void dispatch_tts_pcm(const uint8_t *pcm, size_t bytes)
{
    if (!s_audio_cb || !pcm || bytes < 2) return;
    size_t incoming = bytes / sizeof(int16_t);
    size_t total = s_resample_tail_count + incoming;
    int16_t *combined = malloc(total * sizeof(int16_t));
    if (!combined) return;
    memcpy(combined, s_resample_tail, s_resample_tail_count * sizeof(int16_t));
    memcpy(combined + s_resample_tail_count, pcm, incoming * sizeof(int16_t));

    size_t groups = total / 3;
    size_t out_count = groups * 2;
    int16_t *out = malloc(out_count * sizeof(int16_t));
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
    if (!s_cancel_generation && out_count) s_audio_cb(out, out_count);
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
        xEventGroupSetBits(s_events, CLOUD_READY_TTS);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.tts_connected = true;
        xSemaphoreGive(s_lock);
    } else if (strcmp(type->valuestring, "speech.audio.delta") == 0) {
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_data");
        if (cJSON_IsString(audio) && !s_cancel_generation) {
            size_t source_len = strlen(audio->valuestring);
            size_t cap = (source_len * 3) / 4 + 4;
            uint8_t *decoded = malloc(cap);
            size_t decoded_len = 0;
            if (decoded && mbedtls_base64_decode(decoded, cap, &decoded_len,
                    (const unsigned char *)audio->valuestring, source_len) == 0) {
                set_state(JAFR_CLOUD_SPEAKING);
                dispatch_tts_pcm(decoded, decoded_len);
            }
            free(decoded);
        }
    } else if (strcmp(type->valuestring, "speech.audio.done") == 0) {
        if (!s_cancel_generation) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.turns_completed++;
            xSemaphoreGive(s_lock);
            set_state(JAFR_CLOUD_LISTENING);
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *message = error ? cJSON_GetObjectItemCaseSensitive(error, "message") : NULL;
        set_error(cJSON_IsString(message) ? message->valuestring : "TTS service error");
    }
    cJSON_Delete(root);
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
        set_state(JAFR_CLOUD_LISTENING);
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
        if (s_status.state == JAFR_CLOUD_SPEAKING || s_status.state == JAFR_CLOUD_THINKING) {
            xEventGroupSetBits(s_events, CLOUD_BARGE_IN);
        }
        if (s_transcript_len == 0) s_transcript[0] = '\0';
        set_state(JAFR_CLOUD_LISTENING);
    } else if (strcmp(type->valuestring, "turn.resume") == 0) {
        s_turn_finalize_at_us = 0;
    } else if (strcmp(type->valuestring, "turn.end") == 0) {
        s_turn_finalize_at_us = esp_timer_get_time() + 400000;
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *message = error ? cJSON_GetObjectItemCaseSensitive(error, "message") : NULL;
        set_error(cJSON_IsString(message) ? message->valuestring : "STT service error");
    }
    cJSON_Delete(root);
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    ws_message_t *message = arg;
    esp_websocket_event_data_t *event = event_data;
    const bool is_stt = message == &s_stt_message;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "%s websocket connected", is_stt ? "STT" : "TTS");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (is_stt) s_status.stt_connected = false;
        else s_status.tts_connected = false;
        s_status.reconnects++;
        xSemaphoreGive(s_lock);
        xEventGroupClearBits(s_events, is_stt ? CLOUD_READY_STT : CLOUD_READY_TTS);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        set_error(is_stt ? "STT websocket transport error" : "TTS websocket transport error");
    } else if (event_id == WEBSOCKET_EVENT_DATA && event && event->op_code == 0x1) {
        if (ws_message_append(message, event)) {
            if (is_stt) parse_stt_event(message->data);
            else parse_tts_event(message->data);
            ws_message_reset(message);
        }
    }
}

static void destroy_websockets(void)
{
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
    xEventGroupClearBits(s_events, CLOUD_READY_STT | CLOUD_READY_TTS);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.stt_connected = false;
    s_status.tts_connected = false;
    xSemaphoreGive(s_lock);
}

static bool start_websockets(void)
{
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
        .reconnect_timeout_ms = 3000,
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
    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, system);
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
    EventBits_t ready = xEventGroupWaitBits(s_events, CLOUD_READY_TTS, pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(10000));
    if (!(ready & CLOUD_READY_TTS)) {
        set_error("TTS websocket not ready");
        return false;
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
    set_state(JAFR_CLOUD_THINKING);
    s_cancel_generation = false;
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (ctx->len) {
        ctx->line[ctx->len] = '\0';
        handle_chat_sse_line(ctx, ctx->line);
    }
    if (result == ESP_OK && status == 200 && !s_cancel_generation) tts_flush();
    if (result != ESP_OK && !s_cancel_generation) set_error("Mistral chat stream failed");
    if (status != 200 && !s_cancel_generation) {
        char error[64];
        snprintf(error, sizeof(error), "Mistral chat HTTP %d", status);
        set_error(error);
    }
    esp_http_client_cleanup(client);
    free(ctx);
    cJSON_free(body);
    return result == ESP_OK && status == 200 && !s_cancel_generation;
}

static void cloud_manager_task(void *arg)
{
    (void)arg;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_events,
            CLOUD_RECONFIGURE | CLOUD_TURN_READY | CLOUD_BARGE_IN,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(50));

        if (bits & CLOUD_BARGE_IN) {
            s_cancel_generation = true;
            s_resample_tail_count = 0;
            set_state(JAFR_CLOUD_LISTENING);
        }
        if (bits & CLOUD_RECONFIGURE) {
            destroy_websockets();
            s_mic_count = 0;
            s_transcript_len = 0;
            s_transcript[0] = '\0';
            if (jafr_cloud_voice_is_direct()) {
                set_state(JAFR_CLOUD_CONNECTING);
                if (!start_websockets()) set_error("Cloud websocket startup failed");
            } else {
                set_state(JAFR_CLOUD_DISABLED);
            }
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
        nvs_close(handle);
    }
    if (mode > JAFR_VOICE_MODE_DIRECT) mode = JAFR_VOICE_MODE_PC;
    s_status.mode = (jafr_voice_mode_t)mode;
    s_status.key_configured = s_api_key[0] != '\0';
    if (!s_status.key_configured) s_status.mode = JAFR_VOICE_MODE_PC;
}

esp_err_t jafr_cloud_voice_init(jafr_cloud_audio_cb_t audio_cb,
                                jafr_cloud_state_cb_t state_cb)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_events) return ESP_ERR_NO_MEM;
    s_audio_cb = audio_cb;
    s_state_cb = state_cb;
    load_config();
    if (xTaskCreatePinnedToCore(cloud_manager_task, "cloud_voice", 12288, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_events, CLOUD_RECONFIGURE);
    return ESP_OK;
}

bool jafr_cloud_voice_is_direct(void)
{
    bool direct;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    direct = s_status.mode == JAFR_VOICE_MODE_DIRECT && s_status.key_configured;
    xSemaphoreGive(s_lock);
    return direct;
}

bool jafr_cloud_voice_submit_pcm(const int16_t *samples, size_t sample_count)
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
            size_t encoded_cap = ((sizeof(s_mic_batch) + 2) / 3) * 4 + 1;
            unsigned char *encoded = malloc(encoded_cap);
            size_t encoded_len = 0;
            bool sent_ok = false;
            if (encoded && mbedtls_base64_encode(encoded, encoded_cap, &encoded_len,
                    (const unsigned char *)s_mic_batch, sizeof(s_mic_batch)) == 0) {
                encoded[encoded_len] = '\0';
                cJSON *root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "type", "input_audio.append");
                cJSON_AddStringToObject(root, "audio", (const char *)encoded);
                sent_ok = websocket_send_json(s_stt, root) > 0;
                cJSON_Delete(root);
            }
            free(encoded);
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
    s_status.mode = (mode == JAFR_VOICE_MODE_DIRECT && !s_status.key_configured)
        ? JAFR_VOICE_MODE_PC : mode;
    s_status.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    xEventGroupSetBits(s_events, CLOUD_RECONFIGURE);
    return ESP_OK;
}

void jafr_cloud_voice_get_status(jafr_cloud_status_t *status)
{
    if (!status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}
