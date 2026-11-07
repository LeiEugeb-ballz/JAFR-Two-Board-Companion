#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "jafr_webui.h"
#include "s31_graphics.h"

static const char *TAG = "JAFR_WEBUI";

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
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"wifi_ip\":\"%s\",\"ethernet_ip\":\"%s\",\"ethernet_link\":%s,\"gateway_ip\":\"%s\","
        "\"gateway_mode\":\"%s\",\"gateway_discovered\":%s,\"mic_muted\":%s,"
        "\"espnow_ready\":%s,\"espnow_accepted\":%lu,\"espnow_failed\":%lu,"
        "\"espnow_delivered\":%lu,\"espnow_delivery_failed\":%lu,"
        "\"espnow_last\":\"%s\","
        "\"tx_packets\":%lu,\"rx_packets\":%lu,\"mic_rms\":%.1f,"
        "\"free_heap\":%lu}",
        s.wifi_ip, s.ethernet_ip, s.ethernet_link ? "true" : "false", s.gateway_ip,
        s.gateway_auto ? "auto" : "manual",
        s.gateway_discovered ? "true" : "false",
        s.mic_muted ? "true" : "false",
        s.espnow_ready ? "true" : "false",
        (unsigned long)s.espnow_accepted, (unsigned long)s.espnow_failed,
        (unsigned long)s.espnow_delivered, (unsigned long)s.espnow_delivery_failed,
        s.espnow_last,
        (unsigned long)s.tx_packets, (unsigned long)s.rx_packets,
        s.mic_rms, (unsigned long)s.free_heap);
    if (len < 0 || len >= (int)sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status overflow");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
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
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/gateway", .method = HTTP_POST, .handler = gateway_post},
        {.uri = "/api/mic", .method = HTTP_POST, .handler = mic_post},
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
