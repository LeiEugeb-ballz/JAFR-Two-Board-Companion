#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "jafr_ethernet.h"

static const char *TAG = "JAFR_ETH";
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;
static bool s_link_up;
static char s_ip[16] = "unplugged";

static esp_err_t yt8531_configure_delays(esp_eth_handle_t eth)
{
    bool autoneg = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_S_AUTONEGO, &autoneg), TAG,
                        "enable YT8531 autonegotiation");

    esp_eth_phy_reg_rw_data_t reg = {.reg_value_p = NULL};
    uint32_t value = 0xA001;
    reg.reg_value_p = &value;
    reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &reg), TAG,
                        "select YT8531 chip config");
    reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &reg), TAG,
                        "read YT8531 chip config");
    value |= (1U << 8);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &reg), TAG,
                        "set YT8531 RX delay");

    value = 0xA003;
    reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &reg), TAG,
                        "select YT8531 RGMII config");
    reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &reg), TAG,
                        "read YT8531 RGMII config");
    value = (value & ~0x00FFU) | (13U << 4) | 13U;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &reg), TAG,
                        "set YT8531 TX delays");
    return ESP_OK;
}

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
        snprintf(s_ip, sizeof(s_ip), "acquiring");
        eth_speed_t speed;
        eth_duplex_t duplex;
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGI(TAG, "RJ45 link up: %d Mbps, %s duplex",
                 speed == ETH_SPEED_10M ? 10 : speed == ETH_SPEED_100M ? 100 : 1000,
                 duplex == ETH_DUPLEX_HALF ? "half" : "full");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_link_up = false;
        snprintf(s_ip, sizeof(s_ip), "unplugged");
        ESP_LOGI(TAG, "RJ45 link down; Wi-Fi remains available");
    }
}

static void eth_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "RJ45 address: %s", s_ip);
}

void jafr_ethernet_get_status(bool *link_up, char *ip, size_t ip_size)
{
    if (link_up != NULL) {
        *link_up = s_link_up;
    }
    if (ip != NULL && ip_size > 0) {
        snprintf(ip, ip_size, "%s", s_ip);
    }
}

esp_err_t jafr_ethernet_start(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;
    phy_config.reset_gpio_num = 7;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.interface = EMAC_DATA_INTERFACE_RGMII;
    emac.smi_gpio.mdc_num = 5;
    emac.smi_gpio.mdio_num = 6;
    emac.clock_config.rgmii.clock_rx_gpio = 14;
    emac.clock_config.rgmii.clock_tx_gpio = 13;
    emac.clock_config.rgmii.clock_phy_ref_gpio = -1;
    emac.emac_dataif_gpio.rgmii.tx_ctl_num = 12;
    emac.emac_dataif_gpio.rgmii.txd0_num = 8;
    emac.emac_dataif_gpio.rgmii.txd1_num = 9;
    emac.emac_dataif_gpio.rgmii.txd2_num = 10;
    emac.emac_dataif_gpio.rgmii.txd3_num = 11;
    emac.emac_dataif_gpio.rgmii.rx_ctl_num = 15;
    emac.emac_dataif_gpio.rgmii.rxd0_num = 19;
    emac.emac_dataif_gpio.rgmii.rxd1_num = 18;
    emac.emac_dataif_gpio.rgmii.rxd2_num = 17;
    emac.emac_dataif_gpio.rgmii.rxd3_num = 16;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_FAIL, TAG, "create Ethernet MAC");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        return ESP_FAIL;
    }
    esp_eth_config_t driver_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&driver_config, &s_eth_handle);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        return err;
    }
    ESP_RETURN_ON_ERROR(yt8531_configure_delays(s_eth_handle), TAG,
                        "configure onboard YT8531");

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "JAFR_ETH";
    base.if_desc = "jafr_rj45";
    base.route_prio = 150;
    esp_netif_config_t netif_config = {
        .base = &base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_eth_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_FAIL, TAG, "create Ethernet netif");
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue), TAG, "attach Ethernet netif");

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 77, 2);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    IP4_ADDR(&ip_info.gw, 0, 0, 0, 0);
    esp_err_t dhcp_err = esp_netif_dhcpc_stop(s_eth_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_RETURN_ON_ERROR(dhcp_err, TAG, "stop Ethernet DHCP client");
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_eth_netif, &ip_info), TAG,
                        "set dedicated Ethernet address");
    snprintf(s_ip, sizeof(s_ip), "192.168.77.2");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event, NULL), TAG,
                        "register Ethernet event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   eth_got_ip, NULL), TAG,
                        "register Ethernet IP handler");
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "start Ethernet");
    ESP_LOGI(TAG, "Onboard YT8531 RGMII Ethernet started at 192.168.77.2; cable link is preferred");
    return ESP_OK;
}
