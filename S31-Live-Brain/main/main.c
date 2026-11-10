/*
 * Phase 3 — Network Audio Foundation Implementation
 * Target: esp32s31 (ESP32-S31 Function-CoreBoard-1)
 *
 * Audio Hardware Routing (Certified):
 *   I2C SDA  = GPIO 51, SCL = GPIO 50, Addr = 0x18
 *   I2S MCLK = GPIO 52, BCLK = GPIO 53, WS = GPIO 55
 *   I2S DIN  = GPIO 54 (ES8311 ASDOUT -> ESP32 RX)
 *   I2S DOUT = GPIO 56 (ESP32 TX -> ES8311 DSDIN)
 *   PA_CTRL  = GPIO 57 (Disabled / LOW for Phase 3)
 *
 * Network Pipeline:
 *   Onboard Mic -> ES8311 -> 16kHz PCM (20ms frames / 320 samples / 640B)
 *   -> RingBuffer -> LwIP UDP Socket -> discovered PC Voice Gateway (:5000)
 *   <- PC Voice Gateway (port 5001) <- LwIP UDP RX Task
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "s31_led_status.h"
#include "s31_graphics.h"
#include "jafr_webui.h"
#include "jafr_ethernet.h"
#include "jafr_ble_audio.h"
#include "jafr_cloud_voice.h"

static const char *TAG = "PHASE_3_NET_AUDIO";

#define I2C_NUM             I2C_NUM_0
#define I2C_SDA_GPIO        GPIO_NUM_51
#define I2C_SCL_GPIO        GPIO_NUM_50

#define I2S_NUM             I2S_NUM_0
#define I2S_MCLK_GPIO       GPIO_NUM_52
#define I2S_BCLK_GPIO       GPIO_NUM_53
#define I2S_WS_GPIO         GPIO_NUM_55
#define I2S_DIN_GPIO        GPIO_NUM_54
#define I2S_DOUT_GPIO       GPIO_NUM_56

#define PA_CTRL_GPIO        GPIO_NUM_57

#define PTT_BUTTON_GPIO_0   GPIO_NUM_0   // Onboard BOOT button
#define PTT_BUTTON_GPIO_4   GPIO_NUM_4   // Breakout GPIO pin 4

#define SAMPLE_RATE         16000
#define SAMPLES_PER_FRAME   320   // 20ms @ 16kHz
#define FRAME_PAYLOAD_SIZE  (SAMPLES_PER_FRAME * sizeof(int16_t)) // 640 bytes

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define GATEWAY_ACK_BIT     BIT2

#define WIFI_SSID           CONFIG_ESP_WIFI_SSID
#define WIFI_PASSWORD       CONFIG_ESP_WIFI_PASSWORD

// Static Network Topology Configuration
#define ESP32_STATIC_IP     "192.168.88.242"
#define ESP32_NETMASK       "255.255.255.0"
#define ESP32_GATEWAY_IP    "192.168.88.1"

#define S31_LOCAL_VAD_RMS_THRESHOLD   100.0f
#define S31_BARGE_IN_RMS_THRESHOLD    250.0f
#define S31_LOCAL_VAD_ZCR_LOWER       0.01f

#define PC_GATEWAY_IP_ADDR  "192.168.88.252" // Fallback if discovery is unavailable
#define PC_GATEWAY_PORT     5000
#define ESP_LISTEN_PORT     5001

// Protocol Header (16 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];     // 'A', 'U' (0x41, 0x55)
    uint8_t  version;      // 0x01
    uint8_t  flags;        // Bit 0: Direction (0=ESP->PC, 1=PC->ESP)
    uint32_t seq_num;      // Sequence number
    uint32_t timestamp_ms; // ESP32 uptime timestamp in ms
    uint16_t payload_len;  // 640 bytes
    uint16_t crc16;        // Integrity checksum
} audio_frame_header_t;

static i2c_master_bus_handle_t i2c_bus = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static esp_codec_dev_handle_t mic_codec_dev = NULL;
static esp_codec_dev_handle_t spk_codec_dev = NULL;

static RingbufHandle_t tx_ringbuf = NULL;
static EventGroupHandle_t wifi_event_group = NULL;

// Telemetry counters
static uint32_t g_tx_packets = 0;
static uint32_t g_tx_bytes = 0;
static uint32_t g_rx_packets = 0;
static uint32_t g_rx_bytes = 0;
static double g_last_mic_rms = 0.0;
static int32_t g_last_mic_pk_pk = 0;
static bool g_local_speech_active = false;
static volatile bool g_mic_muted = false;
static bool g_gateway_auto = true;
static uint32_t g_manual_gateway_ip = 0;
static volatile jafr_audio_route_t g_audio_route = JAFR_AUDIO_ROUTE_AUTO;
// Network byte order. Zero means automatic discovery has not completed.
// The ACK receiver updates this from the packet source.
static volatile uint32_t g_pc_gateway_ip = 0;
static volatile uint32_t g_allocation_failures = 0;
static volatile uint32_t g_last_failed_allocation = 0;
static volatile uint32_t g_last_failed_caps = 0;
static volatile uint32_t g_status_requests = 0;

static void IRAM_ATTR allocation_failed(size_t size, uint32_t caps, const char *function_name)
{
    (void)function_name;
    g_allocation_failures++;
    g_last_failed_allocation = (uint32_t)size;
    g_last_failed_caps = caps;
}

static void route_response_pcm(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) return;
    bool routed_to_dualeye = false;
    if (g_audio_route != JAFR_AUDIO_ROUTE_S31) {
        routed_to_dualeye = jafr_ble_audio_submit_pcm(samples, sample_count);
    }
    if (!routed_to_dualeye && spk_codec_dev != NULL) {
        esp_codec_dev_write(spk_codec_dev, (void *)samples, sample_count * sizeof(int16_t));
    }
    g_rx_packets++;
    g_rx_bytes += sample_count * sizeof(int16_t);
}

static void cloud_state_changed(jafr_cloud_state_t state)
{
    switch (state) {
    case JAFR_CLOUD_LISTENING:
        s31_led_status_set_state(S31_LED_STATE_LISTENING);
        s31_graphics_expression("listening");
        break;
    case JAFR_CLOUD_THINKING:
        s31_led_status_set_state(S31_LED_STATE_THINKING);
        s31_graphics_expression("thinking");
        break;
    case JAFR_CLOUD_SPEAKING:
        s31_led_status_set_state(S31_LED_STATE_SPEAKING);
        s31_graphics_expression("speaking");
        break;
    case JAFR_CLOUD_ERROR:
        s31_led_status_set_state(S31_LED_STATE_ERROR);
        s31_graphics_expression("error");
        break;
    case JAFR_CLOUD_CONNECTING:
        s31_led_status_set_state(S31_LED_STATE_CONNECTING);
        break;
    case JAFR_CLOUD_DISABLED:
    default:
        s31_led_status_set_state(S31_LED_STATE_IDLE);
        s31_graphics_expression("neutral");
        break;
    }
}

static inline uint32_t current_pc_gateway_ip(void)
{
    if (!g_gateway_auto && g_manual_gateway_ip != 0) {
        return g_manual_gateway_ip;
    }
    return g_pc_gateway_ip;
}

static void load_gateway_config(void)
{
    nvs_handle_t handle;
    if (nvs_open("jafr_net", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t automatic = 1;
    nvs_get_u8(handle, "gw_auto", &automatic);
    g_gateway_auto = automatic != 0;

    char ip[16] = {0};
    size_t ip_len = sizeof(ip);
    if (nvs_get_str(handle, "gw_ip", ip, &ip_len) == ESP_OK) {
        struct in_addr parsed;
        if (inet_pton(AF_INET, ip, &parsed) == 1) {
            g_manual_gateway_ip = parsed.s_addr;
        }
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Gateway mode: %s, manual address: %s",
             g_gateway_auto ? "automatic discovery" : "manual",
             g_manual_gateway_ip ? ip : "not set");
}

static const char *audio_route_name(jafr_audio_route_t route)
{
    switch (route) {
    case JAFR_AUDIO_ROUTE_S31:
        return "s31";
    case JAFR_AUDIO_ROUTE_DUALEYE:
        return "dualeye";
    case JAFR_AUDIO_ROUTE_AUTO:
    default:
        return "auto";
    }
}

static void load_audio_config(void)
{
    nvs_handle_t handle;
    uint8_t route = JAFR_AUDIO_ROUTE_AUTO;
    if (nvs_open("jafr_audio", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "route", &route) != ESP_OK || route > JAFR_AUDIO_ROUTE_DUALEYE) {
            route = JAFR_AUDIO_ROUTE_AUTO;
        }
        nvs_close(handle);
    }
    g_audio_route = (jafr_audio_route_t)route;
    ESP_LOGI(TAG, "Response audio route: %s", audio_route_name(g_audio_route));
}

esp_err_t jafr_network_set_gateway_config(bool automatic, const char *ip)
{
    struct in_addr parsed = {0};
    if (!automatic) {
        if (ip == NULL || inet_pton(AF_INET, ip, &parsed) != 1 ||
            parsed.s_addr == htonl(INADDR_ANY) ||
            parsed.s_addr == htonl(INADDR_BROADCAST)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("jafr_net", NVS_READWRITE, &handle), TAG,
                        "open network settings");
    esp_err_t err = nvs_set_u8(handle, "gw_auto", automatic ? 1 : 0);
    if (err == ESP_OK && !automatic) {
        err = nvs_set_str(handle, "gw_ip", ip);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        g_gateway_auto = automatic;
        g_manual_gateway_ip = automatic ? g_manual_gateway_ip : parsed.s_addr;
        g_pc_gateway_ip = 0;
    }
    return err;
}

void jafr_audio_set_mic_muted(bool muted)
{
    g_mic_muted = muted;
    if (muted) {
        g_local_speech_active = false;
        if (s31_led_status_get_state() == S31_LED_STATE_LISTENING) {
            s31_led_status_set_state(S31_LED_STATE_IDLE);
        }
    }
    ESP_LOGI(TAG, "Microphone transport %s", muted ? "MUTED" : "LIVE");
}

esp_err_t jafr_audio_set_output_route(const char *route)
{
    jafr_audio_route_t selected;
    if (route == NULL || strcmp(route, "auto") == 0) {
        selected = JAFR_AUDIO_ROUTE_AUTO;
    } else if (strcmp(route, "s31") == 0) {
        selected = JAFR_AUDIO_ROUTE_S31;
    } else if (strcmp(route, "dualeye") == 0) {
        selected = JAFR_AUDIO_ROUTE_DUALEYE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("jafr_audio", NVS_READWRITE, &handle), TAG,
                        "open audio settings");
    esp_err_t err = nvs_set_u8(handle, "route", (uint8_t)selected);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        g_audio_route = selected;
        ESP_LOGI(TAG, "Response audio route changed to %s", audio_route_name(selected));
    }
    return err;
}

void jafr_network_get_web_status(jafr_web_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    g_status_requests++;
    snprintf(status->wifi_ip, sizeof(status->wifi_ip), "waiting");
    esp_netif_t *wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (wifi != NULL && esp_netif_get_ip_info(wifi, &info) == ESP_OK && info.ip.addr != 0) {
        inet_ntop(AF_INET, &info.ip.addr, status->wifi_ip, sizeof(status->wifi_ip));
    }
    uint32_t gateway = current_pc_gateway_ip();
    inet_ntop(AF_INET, &gateway, status->gateway_ip, sizeof(status->gateway_ip));
    jafr_ethernet_get_status(&status->ethernet_link, status->ethernet_ip,
                             sizeof(status->ethernet_ip));
    s31_graphics_get_transport_status(&status->espnow_ready,
                                      &status->espnow_accepted,
                                      &status->espnow_failed,
                                      &status->espnow_delivered,
                                      &status->espnow_delivery_failed,
                                      status->espnow_last,
                                      sizeof(status->espnow_last));
    status->gateway_auto = g_gateway_auto;
    status->gateway_discovered = g_gateway_auto && g_pc_gateway_ip != 0;
    status->mic_muted = g_mic_muted;
    status->tx_packets = g_tx_packets;
    status->rx_packets = g_rx_packets;
    status->mic_rms = g_last_mic_rms;
    status->free_heap = esp_get_free_heap_size();
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    status->internal_free = heap_caps_get_free_size(internal_caps);
    status->internal_minimum = heap_caps_get_minimum_free_size(internal_caps);
    status->internal_largest = heap_caps_get_largest_free_block(internal_caps);
    status->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    status->psram_minimum = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    status->allocation_failures = g_allocation_failures;
    status->last_failed_allocation = g_last_failed_allocation;
    status->last_failed_caps = g_last_failed_caps;
    status->status_requests = g_status_requests;
    jafr_ble_audio_status_t ble = {0};
    jafr_ble_audio_get_status(&ble);
    status->ble_audio_initialized = ble.initialized;
    status->ble_audio_scanning = ble.scanning;
    status->ble_audio_connected = ble.connected;
    status->ble_audio_ready = ble.ready;
    status->ble_audio_mtu = ble.mtu;
    status->ble_audio_sent = ble.sent_frames;
    status->ble_audio_dropped = ble.queue_drops;
    status->ble_audio_failures = ble.write_failures;
    snprintf(status->audio_route, sizeof(status->audio_route), "%s",
             audio_route_name(g_audio_route));
    const bool using_dualeye = g_audio_route != JAFR_AUDIO_ROUTE_S31 && ble.ready;
    snprintf(status->audio_output, sizeof(status->audio_output), "%s",
             using_dualeye ? "dualeye" :
             (g_audio_route == JAFR_AUDIO_ROUTE_DUALEYE ? "s31-fallback" : "s31"));
    jafr_cloud_status_t cloud = {0};
    jafr_cloud_voice_get_status(&cloud);
    snprintf(status->voice_mode, sizeof(status->voice_mode), "%s",
             cloud.mode == JAFR_VOICE_MODE_DIRECT ? "direct" : "pc");
    snprintf(status->cloud_state, sizeof(status->cloud_state), "%s",
             jafr_cloud_state_name(cloud.state));
    status->cloud_key_configured = cloud.key_configured;
    status->cloud_direct_active = cloud.direct_active;
    status->cloud_fallback_active = cloud.fallback_active;
    status->cloud_provisioning_required = cloud.provisioning_required;
    status->cloud_stt_connected = cloud.stt_connected;
    status->cloud_tts_connected = cloud.tts_connected;
    status->cloud_chunks_sent = cloud.audio_chunks_sent;
    status->cloud_chunks_dropped = cloud.audio_chunks_dropped;
    status->cloud_playback_drops = cloud.playback_queue_drops;
    status->cloud_turns_completed = cloud.turns_completed;
    status->cloud_history_turns = cloud.history_turns;
    status->cloud_reconnects = cloud.reconnects;
    status->cloud_failures = cloud.consecutive_failures;
    status->cloud_auth_failures = cloud.auth_failures;
    status->cloud_turn_to_chat_ms = cloud.turn_to_chat_ms;
    status->cloud_chat_to_text_ms = cloud.chat_to_first_text_ms;
    status->cloud_text_to_audio_ms = cloud.text_to_first_audio_ms;
    status->cloud_total_audio_ms = cloud.total_first_audio_ms;
    status->cloud_playback_stack = cloud.playback_stack_free;
    status->cloud_last_http_status = cloud.last_http_status;
    snprintf(status->cloud_last_error, sizeof(status->cloud_last_error), "%s", cloud.last_error);
}

static float calculate_zcr(int16_t *samples, int num_samples) {
    int zero_crossings = 0;
    for (int i = 1; i < num_samples; i++) {
        if ((samples[i] >= 0 && samples[i - 1] < 0) || (samples[i] < 0 && samples[i - 1] >= 0)) {
            zero_crossings++;
        }
    }
    return (float)zero_crossings / num_samples;
}

static void enable_speaker_pa(void)
{
    gpio_config_t pa_conf = {
        .pin_bit_mask = (1ULL << PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_conf);
    gpio_set_level(PA_CTRL_GPIO, 1); // Speaker PA ON (GPIO 57 HIGH)
    ESP_LOGI(TAG, "Speaker Amplifier PA_CTRL (GPIO%d) set to HIGH (Enabled)", PA_CTRL_GPIO);
}

static void init_i2s_periph(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S0 Master initialized (MCLK=%d, BCLK=%d, WS=%d, DIN=%d, DOUT=%d)",
             I2S_MCLK_GPIO, I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DIN_GPIO, I2S_DOUT_GPIO);
}

static void init_i2c_periph(void)
{
    gpio_set_pull_mode(I2C_SDA_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(I2C_SCL_GPIO, GPIO_PULLUP_ONLY);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    ESP_LOGI(TAG, "I2C0 Master Bus initialized on SDA=%d, SCL=%d", I2C_SDA_GPIO, I2C_SCL_GPIO);
}

static void init_codec_device(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PA_CTRL_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .no_dac_ref = false,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t dev_in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    mic_codec_dev = esp_codec_dev_new(&dev_in_cfg);

    esp_codec_dev_cfg_t dev_out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    spk_codec_dev = esp_codec_dev_new(&dev_out_cfg);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = SAMPLE_RATE,
    };
    esp_err_t ret = esp_codec_dev_open(mic_codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open mic failed: %s", esp_err_to_name(ret));
    } else {
        esp_codec_dev_set_in_gain(mic_codec_dev, 30.0);
        ESP_LOGI(TAG, "ES8311 Codec ADC opened successfully! MIC Gain set to +30.0 dB");
    }

    ret = esp_codec_dev_open(spk_codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open spk failed: %s", esp_err_to_name(ret));
    } else {
        esp_codec_dev_set_out_vol(spk_codec_dev, 75.0);
        ESP_LOGI(TAG, "ES8311 Codec DAC opened successfully! Speaker volume set to 75.0%");
    }
}

static bool verify_network_reachability(void)
{
    ESP_LOGI(TAG, "Verifying network interface and gateway reachability...");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGE(TAG, "Reachability Check FAILED: No default STA netif found");
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "Reachability Check FAILED: Unable to read netif IP info");
        return false;
    }

    char ip_buf[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_buf, sizeof(ip_buf));
    if (strcmp(ip_buf, ESP32_STATIC_IP) != 0) {
        ESP_LOGE(TAG, "Reachability Check FAILED: Netif IP %s != Expected %s", ip_buf, ESP32_STATIC_IP);
        return false;
    }

    ESP_LOGI(TAG, "Network Reachability Check PASSED: Validated IPv4 %s on netif.", ip_buf);
    return true;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*) event_data;
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "WIFI ASSOCIATED");
        ESP_LOGI(TAG, "  - AP SSID: %s (Channel %d)", event->ssid, event->channel);
        ESP_LOGI(TAG, "==================================================");
        s31_led_status_set_state(S31_LED_STATE_WIFI_ASSOCIATED_WAITING_IP);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected. Retrying connection in 2s...");
        s31_led_status_set_state(S31_LED_STATE_DISCONNECTED);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "WIFI ASSOCIATED");
        ESP_LOGI(TAG, "STATIC IP CONFIGURED: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "NETMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "GATEWAY: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "PC GATEWAY: %s", PC_GATEWAY_IP_ADDR);
        ESP_LOGI(TAG, "UDP TX: %s:%d", PC_GATEWAY_IP_ADDR, PC_GATEWAY_PORT);
        ESP_LOGI(TAG, "UDP RX: local port %d", ESP_LISTEN_PORT);
        ESP_LOGI(TAG, "==================================================");

        if (verify_network_reachability()) {
            esp_err_t graphics_net_err = s31_graphics_network_ready();
            if (graphics_net_err != ESP_OK) {
                ESP_LOGW(TAG, "Graphics ESP-NOW unavailable: %s", esp_err_to_name(graphics_net_err));
            }
            s31_led_status_set_state(S31_LED_STATE_IDLE);
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        } else {
            s31_led_status_set_state(S31_LED_STATE_ERROR);
        }
    }
}

static void perform_wifi_scan(void)
{
    ESP_LOGI(TAG, "Scanning 2.4GHz Wi-Fi channels...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_records && ap_count > 0) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);
            ESP_LOGI(TAG, "Scan complete. Found %d Wi-Fi networks:", ap_count);
            for (int i = 0; i < ap_count; i++) {
                ESP_LOGI(TAG, "  [%d] SSID: '%s' | RSSI: %d dBm | Auth: %d | Ch: %d",
                         i, ap_records[i].ssid, ap_records[i].rssi, ap_records[i].authmode, ap_records[i].primary);
            }
            free(ap_records);
        }
    } else {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    }
}

static void init_wifi(void)
{
    s31_led_status_set_state(S31_LED_STATE_CONNECTING);
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    // Stop DHCP Client to apply Static IPv4 Network Topology
    esp_err_t dhcp_err = esp_netif_dhcpc_stop(netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_stop info: %s", esp_err_to_name(dhcp_err));
    }

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    ip_info.ip.addr = esp_ip4addr_aton(ESP32_STATIC_IP);
    ip_info.gw.addr = esp_ip4addr_aton(ESP32_GATEWAY_IP);
    ip_info.netmask.addr = esp_ip4addr_aton(ESP32_NETMASK);

    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    ESP_LOGI(TAG, "Configured Static IPv4: ESP32=%s | Netmask=%s | Gateway=%s",
             ESP32_STATIC_IP, ESP32_NETMASK, ESP32_GATEWAY_IP);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    perform_wifi_scan();

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID '%s'...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void mic_capture_task(void *pvParameters)
{
    int16_t frame_buf[SAMPLES_PER_FRAME];
    ESP_LOGI(TAG, "Microphone capture task active. 20ms frames (640B) -> RingBuffer");

    while (1) {
        esp_err_t ret = esp_codec_dev_read(mic_codec_dev, frame_buf, sizeof(frame_buf));
        if (ret == ESP_OK) {
            // Signal stats
            int16_t min_val = 32767, max_val = -32768;
            double sum_sq = 0.0;
            for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
                int16_t s = frame_buf[i];
                if (s < min_val) min_val = s;
                if (s > max_val) max_val = s;
                sum_sq += (double)s * (double)s;
            }
            g_last_mic_rms = sqrt(sum_sq / SAMPLES_PER_FRAME);
            g_last_mic_pk_pk = (int32_t)max_val - (int32_t)min_val;

            // Local VAD (RMS + ZCR)
            float zcr = calculate_zcr(frame_buf, SAMPLES_PER_FRAME);
            bool is_speech = (g_last_mic_rms >= 100.0) && (zcr >= 0.01f && zcr <= 0.50f);
            bool is_bargein = (g_last_mic_rms >= 800.0) && (zcr >= 0.01f && zcr <= 0.50f);

            static uint8_t s_consec_speech = 0;
            static uint8_t s_consec_silence = 0;
            static uint8_t s_consec_bargein = 0;

            if (g_mic_muted) {
                s_consec_speech = 0;
                s_consec_silence = 0;
                s_consec_bargein = 0;
                g_local_speech_active = false;
                continue;
            }

            if (s31_led_status_get_state() == S31_LED_STATE_SPEAKING) {
                if (is_bargein) {
                    s_consec_bargein++;
                    if (s_consec_bargein >= 12) { // 240ms of loud speech
                        ESP_LOGI(TAG, "LOCAL BARGE-IN DETECTED! Halting playback...");
                        s31_led_status_set_state(S31_LED_STATE_BARGE_IN);
                        
                        if (jafr_cloud_voice_is_direct()) {
                            jafr_cloud_voice_barge_in();
                        } else {
                            // Send BARGE_IN Control Packet
                            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                            if (sock >= 0) {
                                struct sockaddr_in dest_addr;
                                dest_addr.sin_addr.s_addr = current_pc_gateway_ip();
                                dest_addr.sin_family = AF_INET;
                                dest_addr.sin_port = htons(PC_GATEWAY_PORT);
                                audio_frame_header_t hdr;
                                memset(&hdr, 0, sizeof(hdr));
                                hdr.magic[0] = 'C';
                                hdr.magic[1] = 'O';
                                hdr.version = 1;
                                hdr.flags = 0x04; // BARGE_IN
                                hdr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                                if (dest_addr.sin_addr.s_addr != 0) {
                                    sendto(sock, &hdr, sizeof(hdr), 0,
                                           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                                }
                                close(sock);
                            }
                        }
                        
                        s_consec_bargein = 0;
                        g_local_speech_active = true;
                        s_consec_speech = 12;
                        s_consec_silence = 0;
                        s31_led_status_set_state(S31_LED_STATE_LISTENING);
                    }
                } else {
                    s_consec_bargein = 0;
                }
            } else {
                if (is_speech) {
                    s_consec_speech++;
                    s_consec_silence = 0;
                    if (s_consec_speech >= 6) { // 120ms
                        if (!g_local_speech_active) {
                            g_local_speech_active = true;
                            if (s31_led_status_get_state() == S31_LED_STATE_IDLE || s31_led_status_get_state() == S31_LED_STATE_READY) {
                                s31_led_status_set_state(S31_LED_STATE_LISTENING);
                            }
                        }
                    }
                } else {
                    s_consec_speech = 0;
                    s_consec_silence++;
                    if (s_consec_silence >= 25 && g_local_speech_active) { // 500ms local hangover
                        g_local_speech_active = false;
                        if (s31_led_status_get_state() == S31_LED_STATE_LISTENING) {
                            s31_led_status_set_state(S31_LED_STATE_PROCESSING);
                        }
                    }
                }
            }

            // Push 640B frame into RingBuffer
            UBaseType_t res = xRingbufferSend(tx_ringbuf, frame_buf, sizeof(frame_buf), pdMS_TO_TICKS(10));
            if (res != pdTRUE) {
                // RingBuffer full (network backpressure) - frame dropped to preserve low latency
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void init_ptt_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PTT_BUTTON_GPIO_0) | (1ULL << PTT_BUTTON_GPIO_4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "PTT Button initialized on GPIO 0 (BOOT) and GPIO 4 (Breakout). Active LOW.");
}

static bool g_logical_ptt_state = false;

static inline bool read_raw_ptt_state(void)
{
    // Active LOW (pressed = 0, released = 1)
    return (gpio_get_level(PTT_BUTTON_GPIO_0) == 0) || (gpio_get_level(PTT_BUTTON_GPIO_4) == 0);
}

static bool ptt_debounce_update(void)
{
    bool raw_pressed = read_raw_ptt_state();
    static bool candidate_state = false;
    static uint8_t stable_count = 0;

    if (raw_pressed == candidate_state) {
        if (stable_count < 2) {
            stable_count++;
        }
        if (stable_count >= 2 && g_logical_ptt_state != candidate_state) {
            g_logical_ptt_state = candidate_state;
            if (g_logical_ptt_state) {
                s31_led_status_set_state(S31_LED_STATE_LISTENING);
                ESP_LOGI(TAG, "[PTT] Debounced State Transition -> PRESSED (Listening active)");
            } else {
                if (s31_led_status_get_state() == S31_LED_STATE_LISTENING) {
                    s31_led_status_set_state(S31_LED_STATE_GATEWAY_READY);
                }
                ESP_LOGI(TAG, "[PTT] Debounced State Transition -> RELEASED (Gateway ready)");
            }
        }
    } else {
        candidate_state = raw_pressed;
        stable_count = 1;
    }
    return g_logical_ptt_state;
}

static void udp_tx_task(void *pvParameters)
{
    // Wait for Wi-Fi IP
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "UDP TX task starting. Target PC Gateway: %s:%d", PC_GATEWAY_IP_ADDR, PC_GATEWAY_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP TX socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = current_pc_gateway_ip();
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PC_GATEWAY_PORT);

    uint8_t send_packet_buf[sizeof(audio_frame_header_t) + FRAME_PAYLOAD_SIZE];
    audio_frame_header_t *hdr = (audio_frame_header_t *)send_packet_buf;
    uint8_t *payload_ptr = send_packet_buf + sizeof(audio_frame_header_t);

    hdr->magic[0] = 'A';
    hdr->magic[1] = 'U';
    hdr->version = 1;
    hdr->payload_len = FRAME_PAYLOAD_SIZE;
    hdr->crc16 = 0;

    uint32_t seq_num = 0;

    while (1) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(tx_ringbuf, &item_size, portMAX_DELAY);
        if (item != NULL && item_size == FRAME_PAYLOAD_SIZE) {
            if (jafr_cloud_voice_is_direct()) {
                jafr_cloud_voice_submit_pcm((const int16_t *)item, SAMPLES_PER_FRAME);
                vRingbufferReturnItem(tx_ringbuf, item);
                g_tx_packets++;
                g_tx_bytes += item_size;
                continue;
            }
            bool ptt_active = ptt_debounce_update();
            if (ptt_active) {
                hdr->flags = 0x02; // Bit 1: PTT Active
            } else if (g_local_speech_active) {
                hdr->flags = 0x04; // Bit 2: Local VAD Active
            } else {
                hdr->flags = 0x00; // Released/Silence State
            }
            
            // ALWAYS SEND AUDIO (Additive local VAD constraint)
            memcpy(payload_ptr, item, FRAME_PAYLOAD_SIZE);

            hdr->seq_num = seq_num++;
            hdr->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            vRingbufferReturnItem(tx_ringbuf, item);

            // Discovery may update the destination while this task is running.
            dest_addr.sin_addr.s_addr = current_pc_gateway_ip();
            if (dest_addr.sin_addr.s_addr == 0) {
                continue;
            }

            int err = sendto(sock, send_packet_buf, sizeof(send_packet_buf), 0,
                             (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (err > 0) {
                g_tx_packets++;
                g_tx_bytes += err;
            } else {
                ESP_LOGW(TAG, "UDP sendto error: errno %d", errno);
            }
        }
    }
}

static void udp_rx_task(void *pvParameters)
{
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "UDP RX task starting. Listening on port %d", ESP_LISTEN_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP RX socket: errno %d", errno);
        s31_led_status_set_state(S31_LED_STATE_ERROR);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(ESP_LISTEN_PORT);

    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "UDP RX bind failed: errno %d", errno);
        s31_led_status_set_state(S31_LED_STATE_ERROR);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP RX bind successful on 0.0.0.0:%d! Network UDP Sockets Ready.", ESP_LISTEN_PORT);

    uint8_t rx_buf[1024];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    int64_t last_rx_time = 0;

    while (1) {
        // Set 200ms socket timeout to check for silence / end of speaking
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len >= (int)sizeof(audio_frame_header_t)) {
            audio_frame_header_t *hdr = (audio_frame_header_t *)rx_buf;
            if (hdr->magic[0] == 'L' && hdr->magic[1] == 'E') {
                uint8_t *payload = rx_buf + sizeof(audio_frame_header_t);
                if (hdr->payload_len == 3) {
                    s31_led_status_set_custom_rgb(payload[0], payload[1], payload[2]);
                    ESP_LOGI(TAG, "[LED COMMAND] Temporary Custom RGB: R:%d, G:%d, B:%d", payload[0], payload[1], payload[2]);
                } else if (hdr->payload_len >= 4) {
                    uint8_t state_id = payload[0];
                    uint8_t r = payload[1], g = payload[2], b = payload[3];
                    if (state_id == 0xFF) {
                        s31_led_status_factory_reset_theme();
                    } else {
                        s31_led_state_t st = S31_LED_STATE_IDLE;
                        if (state_id == 1) st = S31_LED_STATE_LISTENING;
                        else if (state_id == 2) st = S31_LED_STATE_THINKING;
                        else if (state_id == 3) st = S31_LED_STATE_SPEAKING;
                        s31_led_status_set_state_color(st, r, g, b);
                        ESP_LOGI(TAG, "[LED THEME COMMAND] Saved Persistent Theme for State %d -> R:%d, G:%d, B:%d", state_id, r, g, b);
                    }
                }
            } else if (hdr->magic[0] == 'C' && hdr->magic[1] == 'O') {
                if (hdr->flags == 0x02) {
                    if (g_gateway_auto) {
                        g_pc_gateway_ip = source_addr.sin_addr.s_addr;
                    }
                    ESP_LOGI(TAG, "PC Gateway discovered at %s:%d",
                             inet_ntoa(source_addr.sin_addr), PC_GATEWAY_PORT);
                    xEventGroupSetBits(wifi_event_group, GATEWAY_ACK_BIT);
                    if (s31_led_status_get_state() == S31_LED_STATE_ERROR) {
                        s31_led_status_set_state(S31_LED_STATE_IDLE);
                    }
                } else if (hdr->flags == 0x04 && hdr->payload_len >= 1 &&
                           len >= (int)(sizeof(audio_frame_header_t) + 1)) {
                    uint8_t state_id = rx_buf[sizeof(audio_frame_header_t)];
                    s31_led_state_t state = S31_LED_STATE_IDLE;
                    if (state_id == 1) state = S31_LED_STATE_LISTENING;
                    else if (state_id == 2) state = S31_LED_STATE_THINKING;
                    else if (state_id == 3) state = S31_LED_STATE_SPEAKING;
                    s31_led_status_set_state(state);
                    ESP_LOGI(TAG, "Gateway synchronized conversation state: %u", state_id);
                }
            } else if (hdr->magic[0] == 'A' && hdr->magic[1] == 'U') {
                if (jafr_cloud_voice_is_direct()) {
                    continue;
                }
                g_rx_packets++;
                g_rx_bytes += len;
                last_rx_time = esp_timer_get_time();
                s31_led_status_set_state(S31_LED_STATE_SPEAKING);

                size_t pcm_len = len - sizeof(audio_frame_header_t);
                bool routed_to_dualeye = false;
                if (g_audio_route != JAFR_AUDIO_ROUTE_S31 &&
                    pcm_len >= sizeof(int16_t) && (pcm_len % sizeof(int16_t)) == 0) {
                    routed_to_dualeye = jafr_ble_audio_submit_pcm(
                        (const int16_t *)(rx_buf + sizeof(audio_frame_header_t)),
                        pcm_len / sizeof(int16_t));
                }
                if (!routed_to_dualeye && pcm_len > 0 && spk_codec_dev != NULL) {
                    esp_codec_dev_write(spk_codec_dev, rx_buf + sizeof(audio_frame_header_t), pcm_len);
                }
            }
        } else {
            // Check if TTS stream finished (> 500ms since last packet)
            if (s31_led_status_get_state() == S31_LED_STATE_SPEAKING) {
                if ((esp_timer_get_time() - last_rx_time) > 500000ULL) {
                    s31_led_status_set_state(S31_LED_STATE_IDLE);
                }
            }
        }
    }
}

static void gateway_rediscovery_task(void *pvParameters)
{
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!g_gateway_auto) {
            continue;
        }

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGW(TAG, "Gateway rediscovery socket failed: errno %d", errno);
            continue;
        }

        int broadcast_enabled = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast_enabled, sizeof(broadcast_enabled));

        struct sockaddr_in dest_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(PC_GATEWAY_PORT),
            .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        };
        audio_frame_header_t hdr = {0};
        hdr.magic[0] = 'C';
        hdr.magic[1] = 'O';
        hdr.version = 1;
        hdr.flags = 0x01;
        hdr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        int sent = sendto(sock, &hdr, sizeof(hdr), 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        close(sock);
        if (sent < 0) {
            ESP_LOGW(TAG, "Gateway rediscovery broadcast failed: errno %d", errno);
        }
    }
}

static void boot_self_test_task(void *pvParameters)
{
    s31_led_status_set_state(S31_LED_STATE_SELF_TEST);
    ESP_LOGI(TAG, "Boot Self-Test Task started. Waiting for Wi-Fi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    
    // Wait a bit for UDP tasks to bind
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Broadcasting discovery PING for PC Gateway...");
    xEventGroupClearBits(wifi_event_group, GATEWAY_ACK_BIT);
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock >= 0) {
        int broadcast_enabled = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast_enabled, sizeof(broadcast_enabled));
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PC_GATEWAY_PORT);

        audio_frame_header_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic[0] = 'C';
        hdr.magic[1] = 'O';
        hdr.version = 1;
        hdr.flags = 0x01; // PING
        hdr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        sendto(sock, &hdr, sizeof(hdr), 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        close(sock);
    }

    ESP_LOGI(TAG, "Waiting for discovery ACK from PC Gateway...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, GATEWAY_ACK_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
    if (!(bits & GATEWAY_ACK_BIT)) {
        ESP_LOGW(TAG, "Discovery timed out; trying configured fallback %s...",
                 PC_GATEWAY_IP_ADDR);
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock >= 0) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = inet_addr(PC_GATEWAY_IP_ADDR);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(PC_GATEWAY_PORT);

            audio_frame_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.magic[0] = 'C';
            hdr.magic[1] = 'O';
            hdr.version = 1;
            hdr.flags = 0x01;
            hdr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            sendto(sock, &hdr, sizeof(hdr), 0,
                   (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            close(sock);
        }
        bits = xEventGroupWaitBits(wifi_event_group, GATEWAY_ACK_BIT,
                                   pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    }
    if (bits & GATEWAY_ACK_BIT) {
        ESP_LOGI(TAG, "Gateway ACK received! Transitioning to READY.");
        s31_led_status_set_state(S31_LED_STATE_READY);
        vTaskDelay(pdMS_TO_TICKS(500)); // Brief pause
        
        // Send GREETING_REQUEST
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock >= 0) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = current_pc_gateway_ip();
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(PC_GATEWAY_PORT);

            audio_frame_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.magic[0] = 'C';
            hdr.magic[1] = 'O';
            hdr.version = 1;
            hdr.flags = 0x03; // GREETING_REQUEST
            hdr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

            sendto(sock, &hdr, sizeof(hdr), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            close(sock);
        }
        
        s31_led_status_set_state(S31_LED_STATE_IDLE); 
    } else {
        ESP_LOGE(TAG, "Timeout waiting for Gateway ACK. Self-test failed.");
        s31_led_status_set_state(S31_LED_STATE_ERROR);
    }

    vTaskDelete(NULL);
}

static void telemetry_task(void *pvParameters)
{
    s31_led_state_t observed_state = S31_LED_STATE_MAX;
    int64_t observed_since_us = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        s31_led_state_t current_state = s31_led_status_get_state();
        int64_t now_us = esp_timer_get_time();
        if (current_state != observed_state) {
            observed_state = current_state;
            observed_since_us = now_us;
        } else if ((current_state == S31_LED_STATE_PROCESSING ||
                    current_state == S31_LED_STATE_THINKING) &&
                   now_us - observed_since_us > 12000000LL) {
            ESP_LOGW(TAG, "Conversation-state watchdog returning stale %s to IDLE",
                     s31_led_status_get_state_name(current_state));
            s31_led_status_set_state(S31_LED_STATE_IDLE);
            observed_state = S31_LED_STATE_IDLE;
            observed_since_us = now_us;
        }
        wifi_ap_record_t ap_info;
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        ESP_LOGI(TAG, "[NET AUDIO TELEMETRY] RSSI: %d dBm | LED State: %s | TX Pkts: %lu (%lu B) | RX Pkts: %lu (%lu B) | Mic RMS: %.1f | Pk-Pk: %ld",
                 rssi, s31_led_status_get_state_name(s31_led_status_get_state()),
                 (unsigned long)g_tx_packets, (unsigned long)g_tx_bytes,
                 (unsigned long)g_rx_packets, (unsigned long)g_rx_bytes,
                 g_last_mic_rms, (long)g_last_mic_pk_pk);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "ESP32-S31 PHASE 4B: STANDALONE STATUS LED SYSTEM");
    ESP_LOGI(TAG, "==================================================");

    // Initialize Status LED Subsystem (WS2812 on GPIO 60)
    s31_led_status_init();

    esp_err_t graphics_err = s31_graphics_init();
    if (graphics_err != ESP_OK) {
        ESP_LOGW(TAG, "Graphics output unavailable: %s", esp_err_to_name(graphics_err));
    }

    // Initialize PTT Button GPIO (GPIO 0 BOOT & GPIO 4 Breakout)
    init_ptt_gpio();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_gateway_config();
    load_audio_config();

    // Create RingBuffer: 5 frames * 640B = 3200B capacity
    tx_ringbuf = xRingbufferCreate(3200, RINGBUF_TYPE_NOSPLIT);
    if (!tx_ringbuf) {
        ESP_LOGE(TAG, "Failed to create TX RingBuffer!");
        s31_led_status_set_state(S31_LED_STATE_ERROR);
        return;
    }

    enable_speaker_pa();
    init_i2s_periph();
    init_i2c_periph();
    init_codec_device();
    init_wifi();
    esp_err_t eth_err = jafr_ethernet_start();
    if (eth_err != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet unavailable: %s", esp_err_to_name(eth_err));
    }
    esp_err_t ble_audio_err = jafr_ble_audio_init();
    if (ble_audio_err != ESP_OK) {
        ESP_LOGW(TAG, "DualEye BLE audio unavailable; S31 speaker fallback remains active: %s",
                 esp_err_to_name(ble_audio_err));
    }
    esp_err_t cloud_err = jafr_cloud_voice_init(route_response_pcm, cloud_state_changed);
    if (cloud_err != ESP_OK) {
        ESP_LOGW(TAG, "Direct cloud voice unavailable; PC gateway remains active: %s",
                 esp_err_to_name(cloud_err));
    }
    esp_err_t web_err = jafr_webui_start();
    if (web_err != ESP_OK) {
        ESP_LOGW(TAG, "Web control deck unavailable: %s", esp_err_to_name(web_err));
    }

    if (mic_codec_dev) {
        xTaskCreatePinnedToCore(mic_capture_task, "mic_capture", 4096, NULL, 5, NULL, 0);
        xTaskCreatePinnedToCore(udp_tx_task, "audio_uplink", 8192, NULL, 4, NULL, 1);
        xTaskCreatePinnedToCore(udp_rx_task, "udp_rx", 4096, NULL, 4, NULL, 1);
        xTaskCreatePinnedToCore(gateway_rediscovery_task, "gw_discovery", 3072, NULL, 2, NULL, 0);
        xTaskCreatePinnedToCore(telemetry_task, "telemetry", 3072, NULL, 2, NULL, 0);
        xTaskCreatePinnedToCore(boot_self_test_task, "boot_test", 3072, NULL, 3, NULL, 0);
    }
}
