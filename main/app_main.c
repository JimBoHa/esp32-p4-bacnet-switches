#include <stdint.h>

#include "bacnet_server.h"
#include "clock_service.h"
#include "config_store.h"
#include "diagnostics.h"
#include "discovery_service.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "network_config_store.h"
#include "ota_server.h"
#include "sdkconfig.h"
#include "switch_inputs.h"
#include "lwip/ip4_addr.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "esp_eth_phy_ip101.h"
#endif

#define WAVESHARE_ETH_MDC_GPIO 31
#define WAVESHARE_ETH_MDIO_GPIO 52
#define WAVESHARE_ETH_PHY_RESET_GPIO 51
#define WAVESHARE_ETH_PHY_ADDRESS 1

static const char *TAG = "p4_bacnet";

static void set_ipv4_address(
    esp_ip4_addr_t *address,
    const uint8_t octets[4])
{
    IP4_ADDR(
        address,
        octets[0],
        octets[1],
        octets[2],
        octets[3]);
}

static esp_err_t apply_network_configuration(
    esp_netif_t *netif,
    const network_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        netif != NULL && network_config_is_valid_blob(config),
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid network configuration");
    ESP_RETURN_ON_ERROR(
        esp_netif_set_hostname(netif, config->hostname),
        TAG,
        "could not set hostname");
    if (config->mode == NETWORK_ADDRESS_DHCP) {
        return ESP_OK;
    }

    const esp_err_t stop_result = esp_netif_dhcpc_stop(netif);
    ESP_RETURN_ON_FALSE(
        stop_result == ESP_OK ||
            stop_result == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED,
        stop_result,
        TAG,
        "could not stop DHCP client");
    esp_netif_ip_info_t ip_info = {0};
    set_ipv4_address(&ip_info.ip, config->ipv4);
    set_ipv4_address(&ip_info.netmask, config->netmask);
    set_ipv4_address(&ip_info.gw, config->gateway);
    ESP_RETURN_ON_ERROR(
        esp_netif_set_ip_info(netif, &ip_info),
        TAG,
        "could not set static IPv4 configuration");
    if (config->dns[0] != 0U || config->dns[1] != 0U ||
        config->dns[2] != 0U || config->dns[3] != 0U) {
        esp_netif_dns_info_t dns = {0};
        set_ipv4_address(&dns.ip.u_addr.ip4, config->dns);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_RETURN_ON_ERROR(
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns),
            TAG,
            "could not set static DNS server");
    }
    return ESP_OK;
}

static esp_err_t install_waveshare_ethernet(esp_eth_handle_t *handle)
{
    esp_err_t ret = ESP_OK;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_handle_t installed_handle = NULL;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "null handle");
    phy_config.phy_addr = WAVESHARE_ETH_PHY_ADDRESS;
    phy_config.reset_gpio_num = WAVESHARE_ETH_PHY_RESET_GPIO;
    emac_config.smi_gpio.mdc_num = WAVESHARE_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = WAVESHARE_ETH_MDIO_GPIO;

    mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_GOTO_ON_FALSE(mac != NULL, ESP_ERR_NO_MEM, fail, TAG, "MAC allocation failed");
    phy = esp_eth_phy_new_ip101(&phy_config);
    ESP_GOTO_ON_FALSE(phy != NULL, ESP_ERR_NO_MEM, fail, TAG, "PHY allocation failed");

    esp_eth_config_t driver_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_GOTO_ON_ERROR(
        esp_eth_driver_install(&driver_config, &installed_handle),
        fail,
        TAG,
        "Ethernet driver install failed");
    *handle = installed_handle;
    return ESP_OK;

fail:
    if (installed_handle != NULL) {
        esp_eth_driver_uninstall(installed_handle);
    } else {
        if (mac != NULL) {
            mac->del(mac);
        }
        if (phy != NULL) {
            phy->del(phy);
        }
    }
    return ret;
}

static void ethernet_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)argument;
    (void)event_base;
    esp_eth_handle_t handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        diagnostics_ethernet_link_changed(true);
        uint8_t mac[6] = {0};
        if (esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Ethernet link up, MAC %02X:%02X:%02X:%02X:%02X:%02X",
                mac[0],
                mac[1],
                mac[2],
                mac[3],
                mac[4],
                mac[5]);
        } else {
            ESP_LOGI(TAG, "Ethernet link up");
        }
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        diagnostics_ethernet_link_changed(false);
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started; waiting for DHCP");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)event_base;
    (void)event_id;
    esp_netif_t *netif = argument;
    const ip_event_got_ip_t *event = event_data;
    const esp_netif_ip_info_t *ip = &event->ip_info;

    diagnostics_ip_acquired(netif, ip);
    ESP_ERROR_CHECK_WITHOUT_ABORT(clock_service_start());
    ESP_LOGI(TAG, "IPv4 address: " IPSTR, IP2STR(&ip->ip));
    ESP_LOGI(TAG, "IPv4 netmask: " IPSTR, IP2STR(&ip->netmask));
    ESP_LOGI(TAG, "IPv4 gateway: " IPSTR, IP2STR(&ip->gw));
    const esp_err_t bacnet_result = bacnet_server_start(netif);
    if (bacnet_result != ESP_OK) {
        diagnostics_record_event(
            DIAGNOSTICS_EVENT_BACNET_SOCKET_FAILED, bacnet_result);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(bacnet_result);
}

static void lost_ip_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    diagnostics_ip_lost();
    ESP_LOGW(TAG, "Ethernet IPv4 address lost");
}

void app_main(void)
{
    ESP_ERROR_CHECK(diagnostics_init());
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(network_config_store_init());
    firmware_config_t active_config;
    network_config_t network_config;
    config_store_get_active(&active_config);
    network_config_get_active(&network_config);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK_WITHOUT_ABORT(clock_service_init());
    switch_inputs_init(&active_config);

    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(install_waveshare_ethernet(&eth_handle));
    diagnostics_set_ethernet_handle(eth_handle);

    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    ESP_ERROR_CHECK(netif != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(apply_network_configuration(netif, &network_config));

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(glue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_attach(netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT,
        ESP_EVENT_ANY_ID,
        ethernet_event_handler,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_ETH_GOT_IP,
        got_ip_event_handler,
        netif));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_ETH_LOST_IP,
        lost_ip_event_handler,
        NULL));

    ESP_LOGI(
        TAG,
        "GPIO%d, GPIO%d, and GPIO%d use internal pull-downs; "
        "switch closed = Active",
        CONFIG_TOGGLE_INPUT_1_GPIO,
        CONFIG_TOGGLE_INPUT_2_GPIO,
        CONFIG_TOGGLE_INPUT_3_GPIO);
    ESP_LOGI(
        TAG,
        "starting BACnet Device %u on UDP %u with %s hostname %s",
        (unsigned)active_config.device_instance,
        (unsigned)active_config.bacnet_port,
        network_config.mode == NETWORK_ADDRESS_DHCP ? "DHCP" : "static",
        network_config.hostname);
    uint16_t discovery_https_port = 0U;
#if CONFIG_OTA_HTTPS_ENABLED
    const esp_err_t ota_result = ota_server_start();
    if (ota_result != ESP_OK) {
        diagnostics_record_event(
            DIAGNOSTICS_EVENT_OTA_SERVER_FAILED, ota_result);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ota_result);
    if (ota_result == ESP_OK) {
        discovery_https_port = CONFIG_OTA_HTTPS_PORT;
    }
#endif
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_err_t discovery_result = discovery_service_start(
        network_config.hostname,
        active_config.device_name,
        discovery_https_port,
        active_config.bacnet_port,
        active_config.device_instance,
        active_config.vendor_identifier,
        app != NULL ? app->version : "unknown");
    if (discovery_result != ESP_OK) {
        diagnostics_record_event(
            DIAGNOSTICS_EVENT_MDNS_FAILED, discovery_result);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(discovery_result);
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_ERROR_CHECK(network_config_start_trial_guard());
    ESP_ERROR_CHECK(ota_start_rollback_validation());
}
