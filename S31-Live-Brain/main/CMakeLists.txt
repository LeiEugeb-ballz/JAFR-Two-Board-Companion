idf_component_register(SRCS "main.c" "s31_led_status.c" "s31_graphics.c" "jafr_webui.c" "jafr_ethernet.c" "jafr_ble_audio.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_eth nvs_flash esp_event esp_netif esp_http_server lwip esp_ringbuf esp_timer esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_uart espressif__esp_codec_dev led_strip heap esp_driver_rmt bt
                       EMBED_FILES "web/index.html")
