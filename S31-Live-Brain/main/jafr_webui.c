#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "jafr_webui.h"
#include "jafr_cloud_voice.h"
#include "s31_graphics.h"

static const char *TAG = "JAFR_WEBUI";

static void json_safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    size_t out = 0;
    for (; src && *src && out + 1 < dst_size; ++src) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (out + 2 >= dst_size) break;
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c >= 0x20) {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

static void *web_malloc(size_t size)
{
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

extern const unsigned char web_index_html_start[] asm("_binary_index_html_start");
extern const unsigned char web_index_html_end[] asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)web_index_html_start,
                           web_index_html_end - web_index_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    jafr_web_status_t s = {0};
    jafr_network_get_web_status(&s);
    char safe_error[196];
    json_safe_copy(safe_error, sizeof(safe_error), s.cloud_last_error);
    char json[3072];
    int len = snprintf(json, sizeof(json),
        "{\"wifi_ip\":\"%s\",\"ethernet_ip\":\"%s\",\"ethernet_link\":%s,\"gateway_ip\":\"%s\","
        "\"gateway_mode\":\"%s\",\"gateway_discovered\":%s,\"mic_muted\":%s,"
        "\"espnow_ready\":%s,\"espnow_accepted\":%lu,\"espnow_failed\":%lu,"
        "\"espnow_delivered\":%lu,\"espnow_delivery_failed\":%lu,"
        "\"espnow_last\":\"%s\","
        "\"tx_packets\":%lu,\"rx_packets\":%lu,\"mic_rms\":%.1f,"
        "\"ble_audio_initialized\":%s,\"ble_audio_scanning\":%s,"
        "\"ble_audio_connected\":%s,\"ble_audio_ready\":%s,"
        "\"ble_audio_mtu\":%u,\"ble_audio_sent\":%lu,"
        "\"ble_audio_dropped\":%lu,\"ble_audio_failures\":%lu,"
        "\"audio_route\":\"%s\",\"audio_output\":\"%s\","
        "\"voice_mode\":\"%s\",\"cloud_state\":\"%s\","
        "\"cloud_key_configured\":%s,\"cloud_direct_active\":%s,"
        "\"cloud_fallback_active\":%s,\"cloud_provisioning_required\":%s,"
        "\"cloud_stt_connected\":%s,"
        "\"cloud_tts_connected\":%s,\"cloud_chunks_sent\":%lu,"
        "\"cloud_chunks_dropped\":%lu,\"cloud_playback_drops\":%lu,"
        "\"cloud_turns_completed\":%lu,\"cloud_history_turns\":%lu,"
        "\"cloud_reconnects\":%lu,\"cloud_failures\":%lu,"
        "\"cloud_auth_failures\":%lu,\"cloud_turn_to_chat_ms\":%lu,"
        "\"cloud_chat_to_text_ms\":%lu,\"cloud_text_to_audio_ms\":%lu,"
        "\"cloud_total_audio_ms\":%lu,\"cloud_playback_stack\":%lu,"
        "\"cloud_last_http_status\":%d,\"cloud_last_error\":\"%s\","
        "\"free_heap\":%lu,\"internal_free\":%lu,\"internal_minimum\":%lu,"
        "\"internal_largest\":%lu,\"psram_free\":%lu,\"psram_minimum\":%lu,"
        "\"allocation_failures\":%lu,\"last_failed_allocation\":%lu,"
        "\"last_failed_caps\":%lu,\"status_requests\":%lu}",
        s.wifi_ip, s.ethernet_ip, s.ethernet_link ? "true" : "false", s.gateway_ip,
        s.gateway_auto ? "auto" : "manual",
        s.gateway_discovered ? "true" : "false",
        s.mic_muted ? "true" : "false",
        s.espnow_ready ? "true" : "false",
        (unsigned long)s.espnow_accepted, (unsigned long)s.espnow_failed,
        (unsigned long)s.espnow_delivered, (unsigned long)s.espnow_delivery_failed,
        s.espnow_last,
        (unsigned long)s.tx_packets, (unsigned long)s.rx_packets, s.mic_rms,
        s.ble_audio_initialized ? "true" : "false",
        s.ble_audio_scanning ? "true" : "false",
        s.ble_audio_connected ? "true" : "false",
        s.ble_audio_ready ? "true" : "false",
        s.ble_audio_mtu,
        (unsigned long)s.ble_audio_sent,
        (unsigned long)s.ble_audio_dropped,
        (unsigned long)s.ble_audio_failures,
        s.audio_route, s.audio_output,
        s.voice_mode, s.cloud_state,
        s.cloud_key_configured ? "true" : "false",
        s.cloud_direct_active ? "true" : "false",
        s.cloud_fallback_active ? "true" : "false",
        s.cloud_provisioning_required ? "true" : "false",
        s.cloud_stt_connected ? "true" : "false",
        s.cloud_tts_connected ? "true" : "false",
        (unsigned long)s.cloud_chunks_sent,
        (unsigned long)s.cloud_chunks_dropped,
        (unsigned long)s.cloud_playback_drops,
        (unsigned long)s.cloud_turns_completed,
        (unsigned long)s.cloud_history_turns,
        (unsigned long)s.cloud_reconnects,
        (unsigned long)s.cloud_failures,
        (unsigned long)s.cloud_auth_failures,
        (unsigned long)s.cloud_turn_to_chat_ms,
        (unsigned long)s.cloud_chat_to_text_ms,
        (unsigned long)s.cloud_text_to_audio_ms,
        (unsigned long)s.cloud_total_audio_ms,
        (unsigned long)s.cloud_playback_stack,
        s.cloud_last_http_status, safe_error,
        (unsigned long)s.free_heap,
        (unsigned long)s.internal_free,
        (unsigned long)s.internal_minimum,
        (unsigned long)s.internal_largest,
        (unsigned long)s.psram_free,
        (unsigned long)s.psram_minimum,
        (unsigned long)s.allocation_failures,
        (unsigned long)s.last_failed_allocation,
        (unsigned long)s.last_failed_caps,
        (unsigned long)s.status_requests);
    if (len < 0 || len >= (int)sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status overflow");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

static esp_err_t persona_get(httpd_req_t *req)
{
    char *persona = web_malloc(1536);
    char *facts = web_malloc(768);
    if (!persona || !facts) {
        free(persona);
        free(facts);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    jafr_cloud_voice_get_persona(persona, 1536, facts, 768);
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "persona", persona);
        cJSON_AddStringToObject(root, "facts", facts);
    }
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    memset(persona, 0, 1536);
    memset(facts, 0, 768);
    free(persona);
    free(facts);
    if (!json) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
}

static esp_err_t persona_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 8192) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char *body = web_malloc(req->content_len + 1);
    char *persona = web_malloc(1536);
    char *facts = web_malloc(768);
    if (!body || !persona || !facts) {
        free(body); free(persona); free(facts);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            memset(body, 0, req->content_len + 1);
            free(body); free(persona); free(facts);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';
    if (httpd_query_key_value(body, "persona", persona, 1536) != ESP_OK ||
        httpd_query_key_value(body, "facts", facts, 768) != ESP_OK) {
        memset(body, 0, req->content_len + 1);
        free(body); free(persona); free(facts);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "persona and facts required");
    }
    esp_err_t err = jafr_cloud_voice_configure_persona(persona, facts);
    memset(body, 0, req->content_len + 1);
    memset(persona, 0, 1536);
    memset(facts, 0, 768);
    free(body); free(persona); free(facts);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "persona configuration rejected");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t gateway_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 128) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[128] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    char mode[12] = {0};
    char ip[20] = {0};
    if (httpd_query_key_value(body, "mode", mode, sizeof(mode)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode required");
    }
    bool automatic = strcmp(mode, "auto") == 0;
    if (!automatic && strcmp(mode, "manual") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
    }
    if (!automatic && httpd_query_key_value(body, "ip", ip, sizeof(ip)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "IPv4 address required");
    }
    esp_err_t err = jafr_network_set_gateway_config(automatic, ip);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid gateway address");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_restart();
}

static esp_err_t mic_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 32) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[32] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
        }
        received += (size_t)chunk;
    }
    char muted[8] = {0};
    if (httpd_query_key_value(body, "muted", muted, sizeof(muted)) != ESP_OK ||
        (strcmp(muted, "true") != 0 && strcmp(muted, "false") != 0)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "muted=true|false required");
    }
    jafr_audio_set_mic_muted(strcmp(muted, "true") == 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t audio_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 32) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[32] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
        }
        received += (size_t)chunk;
    }
    char route[12] = {0};
    if (httpd_query_key_value(body, "route", route, sizeof(route)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "route required");
    }
    esp_err_t err = jafr_audio_set_output_route(route);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "route=auto|s31|dualeye required");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t voice_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 384) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[384] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
        }
        received += (size_t)chunk;
    }
    char mode_text[12] = {0};
    char api_key[192] = {0};
    char clear_text[8] = {0};
    if (httpd_query_key_value(body, "mode", mode_text, sizeof(mode_text)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode required");
    }
    jafr_voice_mode_t mode;
    if (strcmp(mode_text, "pc") == 0) mode = JAFR_VOICE_MODE_PC;
    else if (strcmp(mode_text, "direct") == 0) mode = JAFR_VOICE_MODE_DIRECT;
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode=pc|direct required");

    bool clear = httpd_query_key_value(body, "clear", clear_text, sizeof(clear_text)) == ESP_OK &&
                 strcmp(clear_text, "true") == 0;
    const char *key = NULL;
    if (httpd_query_key_value(body, "api_key", api_key, sizeof(api_key)) == ESP_OK && api_key[0]) {
        key = api_key;
    }
    esp_err_t err = jafr_cloud_voice_configure(mode, key, clear);
    memset(api_key, 0, sizeof(api_key));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "voice configuration rejected");
    }
    jafr_cloud_status_t status = {0};
    jafr_cloud_voice_get_status(&status);
    if (mode == JAFR_VOICE_MODE_DIRECT && !status.key_configured) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Mistral API key required");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t eyes_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 128) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[128] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
        }
        received += (size_t)chunk;
    }

    char action[20] = {0};
    if (httpd_query_key_value(body, "action", action, sizeof(action)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action required");
    }

    bool queued = false;
    if (strcmp(action, "blink") == 0) {
        queued = s31_graphics_blink();
    } else if (strcmp(action, "expression") == 0) {
        char value[24] = {0};
        if (httpd_query_key_value(body, "value", value, sizeof(value)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expression value required");
        }
        queued = s31_graphics_expression(value);
    } else if (strcmp(action, "look") == 0) {
        char x_text[8] = {0};
        char y_text[8] = {0};
        if (httpd_query_key_value(body, "x", x_text, sizeof(x_text)) != ESP_OK ||
            httpd_query_key_value(body, "y", y_text, sizeof(y_text)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "look x and y required");
        }
        char *x_end = NULL;
        char *y_end = NULL;
        long x = strtol(x_text, &x_end, 10);
        long y = strtol(y_text, &y_end, 10);
        if (!x_end || *x_end != '\0' || !y_end || *y_end != '\0' ||
            x < -100 || x > 100 || y < -100 || y > 100) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "look range is -100..100");
        }
        queued = s31_graphics_look((int8_t)x, (int8_t)y);
    } else if (strcmp(action, "sleep") == 0) {
        queued = s31_graphics_sleep();
    } else if (strcmp(action, "wake") == 0) {
        queued = s31_graphics_wake();
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown eye action");
    }

    httpd_resp_set_type(req, "application/json");
    if (!queued) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"queued\":false}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true,\"queued\":true}");
}

static esp_err_t restart_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "web_restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

esp_err_t jafr_webui_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 11;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/gateway", .method = HTTP_POST, .handler = gateway_post},
        {.uri = "/api/mic", .method = HTTP_POST, .handler = mic_post},
        {.uri = "/api/audio", .method = HTTP_POST, .handler = audio_post},
        {.uri = "/api/voice", .method = HTTP_POST, .handler = voice_post},
        {.uri = "/api/persona", .method = HTTP_GET, .handler = persona_get},
        {.uri = "/api/persona", .method = HTTP_POST, .handler = persona_post},
        {.uri = "/api/eyes", .method = HTTP_POST, .handler = eyes_post},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]),
                            TAG, "route registration failed");
    }
    ESP_LOGI(TAG, "Control deck ready on HTTP port 80");
    return ESP_OK;
}
