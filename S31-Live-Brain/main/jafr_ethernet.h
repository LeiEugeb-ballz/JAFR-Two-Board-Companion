#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t jafr_ethernet_start(void);
void jafr_ethernet_get_status(bool *link_up, char *ip, size_t ip_size);
