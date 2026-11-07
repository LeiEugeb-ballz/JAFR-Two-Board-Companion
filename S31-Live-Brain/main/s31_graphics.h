#ifndef S31_GRAPHICS_H
#define S31_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* All calls enqueue without waiting. UART protocol v1: UTF-8/ASCII lines at
 * 115200 8N1: LOOK x,y, BLINK, EXPRESSION name, SLEEP, WAKE. Each ends in LF.
 * false means duplicate, rate-limited, invalid, unavailable, or queue full.
 * ESP-NOW unicast delivery is reported by the Wi-Fi MAC callback. This proves
 * radio delivery to the configured Eyes MAC, not that a frame was rendered.
 */
esp_err_t s31_graphics_init(void);
esp_err_t s31_graphics_network_ready(void);
bool s31_graphics_look(int8_t x, int8_t y); /* -100..100 */
bool s31_graphics_blink(void);
bool s31_graphics_expression(const char *name);
bool s31_graphics_sleep(void);
bool s31_graphics_wake(void);
void s31_graphics_get_transport_status(bool *ready, uint32_t *accepted,
                                       uint32_t *failed, uint32_t *delivered,
                                       uint32_t *delivery_failed, char *last,
                                       size_t last_size);

#endif
