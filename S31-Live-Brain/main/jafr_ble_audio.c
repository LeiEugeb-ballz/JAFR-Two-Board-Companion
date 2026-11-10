#include "jafr_ble_audio.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "JAFR_BLE_TX";

#define JAFR_BLE_AUDIO_MAGIC 0x414A
#define JAFR_BLE_AUDIO_VERSION 1
#define JAFR_BLE_AUDIO_PAYLOAD_BYTES (JAFR_BLE_AUDIO_SAMPLES_PER_FRAME / 2)
#define JAFR_BLE_AUDIO_QUEUE_FRAMES 8

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t sequence;
    uint16_t sample_count;
    int16_t predictor;
    uint8_t step_index;
    uint8_t reserved;
    uint16_t payload_bytes;
    uint16_t crc16;
    uint8_t payload[JAFR_BLE_AUDIO_PAYLOAD_BYTES];
} jafr_ble_audio_packet_t;

_Static_assert(sizeof(jafr_ble_audio_packet_t) == 176, "BLE audio packet must fit a 179-byte ATT MTU");

typedef struct {
    int16_t samples[JAFR_BLE_AUDIO_SAMPLES_PER_FRAME];
} pcm_frame_t;

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x5a, 0x4a, 0x46, 0x52, 0x2d, 0x41, 0x55, 0x44,
    0x49, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);
static const ble_uuid128_t s_audio_uuid = BLE_UUID128_INIT(
    0x5a, 0x4a, 0x46, 0x52, 0x2d, 0x41, 0x55, 0x44,
    0x49, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02);

static const int s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static QueueHandle_t s_pcm_queue;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static jafr_ble_audio_status_t s_status;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_audio_handle;
static uint16_t s_service_start;
static uint16_t s_service_end;
static uint8_t s_own_addr_type;
static int16_t s_accumulator[JAFR_BLE_AUDIO_SAMPLES_PER_FRAME];
static size_t s_accumulator_count;
static int16_t s_encoder_predictor;
static int s_encoder_index;
static uint16_t s_sequence;

static uint16_t crc16_ccitt(const uint8_t *data, size_t length, uint16_t crc)
{
    while (length--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint8_t adpcm_encode_sample(int16_t sample, int16_t *predictor, int *index)
{
    int step = s_step_table[*index];
    int diff = (int)sample - *predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= (step >> 2)) { code |= 1; delta += step >> 2; }
    int next = (code & 8) ? (int)*predictor - delta : (int)*predictor + delta;
    if (next > 32767) next = 32767;
    if (next < -32768) next = -32768;
    *predictor = (int16_t)next;
    *index += s_index_table[code & 0x0f];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return code & 0x0f;
}

static size_t encode_frame(const int16_t *samples, jafr_ble_audio_packet_t *packet)
{
    memset(packet, 0, sizeof(*packet));
    packet->magic = JAFR_BLE_AUDIO_MAGIC;
    packet->version = JAFR_BLE_AUDIO_VERSION;
    packet->flags = (s_sequence == 0) ? 1 : 0;
    packet->sequence = s_sequence++;
    packet->sample_count = JAFR_BLE_AUDIO_SAMPLES_PER_FRAME;
    if (packet->flags & 1) {
        s_encoder_predictor = samples[0];
        s_encoder_index = 0;
    }
    packet->predictor = s_encoder_predictor;
    packet->step_index = (uint8_t)s_encoder_index;
    packet->payload_bytes = JAFR_BLE_AUDIO_PAYLOAD_BYTES;
    for (size_t i = 0; i < JAFR_BLE_AUDIO_SAMPLES_PER_FRAME; i += 2) {
        uint8_t lo = adpcm_encode_sample(samples[i], &s_encoder_predictor, &s_encoder_index);
        uint8_t hi = adpcm_encode_sample(samples[i + 1], &s_encoder_predictor, &s_encoder_index);
        packet->payload[i / 2] = (uint8_t)(lo | (hi << 4));
    }
    packet->crc16 = 0;
    uint16_t crc = crc16_ccitt((const uint8_t *)packet, offsetof(jafr_ble_audio_packet_t, crc16), 0xffff);
    crc = crc16_ccitt(packet->payload, packet->payload_bytes, crc);
    packet->crc16 = crc;
    return offsetof(jafr_ble_audio_packet_t, payload) + packet->payload_bytes;
}

static void start_scan(void);

static int characteristic_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &s_audio_uuid.u) == 0) {
            s_audio_handle = chr->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        taskENTER_CRITICAL(&s_status_lock);
        s_status.ready = s_audio_handle != 0 && s_status.mtu >= (sizeof(jafr_ble_audio_packet_t) + 3);
        taskEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "DualEye audio characteristic %s (handle=%u)",
                 s_audio_handle ? "ready" : "missing", s_audio_handle);
        return 0;
    }
    ESP_LOGW(TAG, "Characteristic discovery failed: %d", error->status);
    return error->status;
}

static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0 && service != NULL) {
        s_service_start = service->start_handle;
        s_service_end = service->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE && s_service_start != 0) {
        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_service_start, s_service_end,
                                             &s_audio_uuid.u, characteristic_discovered, NULL);
        if (rc != 0) ESP_LOGW(TAG, "Audio characteristic discovery start failed: %d", rc);
        return rc;
    }
    ESP_LOGW(TAG, "Audio service discovery failed: %d", error->status);
    return error->status;
}

static bool advertisement_has_service(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields = {0};
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) return false;
    for (int i = 0; i < fields.num_uuids128; ++i) {
        if (ble_uuid_cmp(&fields.uuids128[i].u, &s_service_uuid.u) == 0) return true;
    }
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (advertisement_has_service(&event->disc)) {
            ble_addr_t address = event->disc.addr;
            ble_gap_disc_cancel();
            taskENTER_CRITICAL(&s_status_lock);
            s_status.scanning = false;
            taskEXIT_CRITICAL(&s_status_lock);
            int rc = ble_gap_connect(s_own_addr_type, &address, 10000, NULL, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "Connect start failed: %d", rc);
                start_scan();
            }
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_audio_handle = 0;
        s_service_start = 0;
        s_service_end = 0;
        taskENTER_CRITICAL(&s_status_lock);
        s_status.connected = true;
        s_status.ready = false;
        s_status.reconnects++;
        taskEXIT_CRITICAL(&s_status_lock);
        ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_service_uuid.u, service_discovered, NULL);
        ESP_LOGI(TAG, "Connected to DualEye BLE audio receiver");
        return 0;
    case BLE_GAP_EVENT_MTU:
        taskENTER_CRITICAL(&s_status_lock);
        s_status.mtu = event->mtu.value;
        s_status.ready = s_audio_handle != 0 && event->mtu.value >= (sizeof(jafr_ble_audio_packet_t) + 3);
        taskEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "BLE audio MTU=%u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "DualEye BLE audio disconnected: %d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_audio_handle = 0;
        s_accumulator_count = 0;
        taskENTER_CRITICAL(&s_status_lock);
        s_status.connected = false;
        s_status.ready = false;
        taskEXIT_CRITICAL(&s_status_lock);
        xQueueReset(s_pcm_queue);
        start_scan();
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;
    params.passive = 0;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.scanning = rc == 0;
    taskEXIT_CRITICAL(&s_status_lock);
    if (rc != 0) ESP_LOGW(TAG, "BLE scan start failed: %d", rc);
}

static void host_synced(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address inference failed: %d", rc);
        return;
    }
    start_scan();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void sender_task(void *arg)
{
    (void)arg;
    pcm_frame_t frame;
    jafr_ble_audio_packet_t packet;
    int64_t last_send_us = 0;
    while (true) {
        if (xQueueReceive(s_pcm_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        if (!jafr_ble_audio_is_ready()) continue;

        // The PC gateway can arrive slightly faster than real time and Wi-Fi
        // delivery is naturally bursty.  Never forward that burst directly to
        // the GATT receiver: one packet represents exactly 20 ms of audio.
        const int64_t now_us = esp_timer_get_time();
        const int64_t elapsed_us = now_us - last_send_us;
        if (last_send_us != 0 && elapsed_us < 20000) {
            const TickType_t delay = pdMS_TO_TICKS((20000 - elapsed_us + 999) / 1000);
            if (delay > 0) vTaskDelay(delay);
        }
        size_t bytes = encode_frame(frame.samples, &packet);
        int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_audio_handle, &packet, bytes);
        last_send_us = esp_timer_get_time();
        taskENTER_CRITICAL(&s_status_lock);
        if (rc == 0) {
            s_status.sent_frames++;
            s_status.sent_bytes += bytes;
            s_status.last_sequence = packet.sequence;
        } else {
            s_status.write_failures++;
        }
        taskEXIT_CRITICAL(&s_status_lock);
        if (rc != 0) vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t jafr_ble_audio_init(void)
{
    if (s_status.initialized) return ESP_OK;
    s_pcm_queue = xQueueCreate(JAFR_BLE_AUDIO_QUEUE_FRAMES, sizeof(pcm_frame_t));
    if (s_pcm_queue == NULL) return ESP_ERR_NO_MEM;
    int rc = nimble_port_init();
    if (rc != ESP_OK) return rc;
    ble_hs_cfg.sync_cb = host_synced;
    ble_att_set_preferred_mtu(247);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.initialized = true;
    s_status.mtu = 23;
    taskEXIT_CRITICAL(&s_status_lock);
    xTaskCreatePinnedToCore(sender_task, "ble_audio_tx", 4096, NULL, 4, NULL, 1);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE GATT audio client initialized (IMA ADPCM 16 kHz mono)");
    return ESP_OK;
}

bool jafr_ble_audio_submit_pcm(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0 || !jafr_ble_audio_is_ready()) return false;
    bool accepted = false;
    while (sample_count > 0) {
        size_t room = JAFR_BLE_AUDIO_SAMPLES_PER_FRAME - s_accumulator_count;
        size_t take = sample_count < room ? sample_count : room;
        memcpy(&s_accumulator[s_accumulator_count], samples, take * sizeof(int16_t));
        s_accumulator_count += take;
        samples += take;
        sample_count -= take;
        if (s_accumulator_count == JAFR_BLE_AUDIO_SAMPLES_PER_FRAME) {
            pcm_frame_t frame;
            memcpy(frame.samples, s_accumulator, sizeof(frame.samples));
            if (xQueueSend(s_pcm_queue, &frame, 0) == pdTRUE) {
                taskENTER_CRITICAL(&s_status_lock);
                s_status.queued_frames++;
                taskEXIT_CRITICAL(&s_status_lock);
                accepted = true;
            } else {
                taskENTER_CRITICAL(&s_status_lock);
                s_status.queue_drops++;
                taskEXIT_CRITICAL(&s_status_lock);
            }
            s_accumulator_count = 0;
        }
    }
    return accepted;
}

bool jafr_ble_audio_is_ready(void)
{
    bool ready;
    taskENTER_CRITICAL(&s_status_lock);
    ready = s_status.ready;
    taskEXIT_CRITICAL(&s_status_lock);
    return ready;
}

void jafr_ble_audio_get_status(jafr_ble_audio_status_t *status)
{
    if (status == NULL) return;
    taskENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
}

