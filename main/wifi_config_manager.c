#include "wifi_config_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "user_fram.h"
#include "user_system.h"
#include "tcp_server_com.h"
#include "user_ouput.h"
#include "user_azure.h"
#include "dns_server.h"
#include "user_external_flash.h"

#define WIFI_CRED_MAGIC 0x57494649u

typedef struct __attribute__((packed))
{
    uint32_t magic;
    char ssid[32];
    char pass[64];
    uint8_t reserved[WIFI_FRAM_SIZE - 4 - 32 - 64];
} wifi_fram_t;

_Static_assert(sizeof(wifi_fram_t) <= WIFI_FRAM_SIZE, "wifi_fram_t exceeds WIFI_FRAM_SIZE");

#define AZURE_CRED_MAGIC 0x415A5552u // 'AZUR'

typedef struct __attribute__((packed))
{
    uint32_t magic;
    char hostName[64];
    char deviceId[64];
    char symmetricKey[64];
    uint8_t reserved[AZURE_FRAM_SIZE - 4 - 64 - 64 - 64];
} azure_fram_t;

_Static_assert(sizeof(azure_fram_t) <= AZURE_FRAM_SIZE, "azure_fram_t exceeds AZURE_FRAM_SIZE");

static const char *WIFI_CFG_TAG = "wifi_cfg";
static int s_retry_num = 0;
static bool s_allow_sta_connect = false;
static bool s_is_associated = false;
static TaskHandle_t s_connect_task = NULL;
static TaskHandle_t s_slow_retry_task = NULL;
static char s_pending_ssid[32] = {0};
static char s_pending_pass[64] = {0};
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY
#define WIFI_SLOW_RETRY_INTERVAL_MS 10000
// Sau 30 lần slow retry (30 × 10s = 5 phút) mà vẫn chưa kết nối
// → restart WiFi driver để thoát khỏi trạng thái stuck
#define WIFI_SLOW_RETRY_MAX_BEFORE_DRIVER_RESTART  30
static uint32_t s_slow_retry_count = 0;
static void prv_wifi_connect(const char *ssid, const char *pass);

static void prv_slow_retry_task(void *pvParameters)
{
    (void)pvParameters;
    s_slow_retry_count = 0;

    while(1)
    {
        // Reset bộ đếm khi WiFi kết nối thành công
        if(Sys_Info.isWifiConnected)
        {
            s_slow_retry_count = 0;
            vTaskDelay(pdMS_TO_TICKS(WIFI_SLOW_RETRY_INTERVAL_MS));
            continue;
        }

        bool is_config_mode = User_Input_Check_Is_Active(IO_CONFIG_INPUT_2);
        if(!is_config_mode && s_allow_sta_connect && !s_is_associated)
        {
             s_slow_retry_count++;
            ESP_LOGI(WIFI_CFG_TAG, "Slow retry connect... (%lu/%d)", (unsigned long)s_slow_retry_count, WIFI_SLOW_RETRY_MAX_BEFORE_DRIVER_RESTART);
            if(s_slow_retry_count >= WIFI_SLOW_RETRY_MAX_BEFORE_DRIVER_RESTART)
            {
                // Sau 5 phút vẫn không kết nối được → restart WiFi driver
                // để thoát khỏi trạng thái stuck trong driver
                ESP_LOGW(WIFI_CFG_TAG, "Too many slow retries (%lu). Restarting WiFi driver...", (unsigned long)s_slow_retry_count);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                s_retry_num = 0;
                s_slow_retry_count = 0;
                prv_wifi_connect(s_pending_ssid, s_pending_pass);
            }
            else
            {
                esp_wifi_connect();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_SLOW_RETRY_INTERVAL_MS));
    }
}

static bool prv_fram_read_wifi(wifi_fram_t *out)
{
    if(out == NULL)
    {
        return false;
    }

    if(!Fram_Init())
    {
        ESP_LOGE(WIFI_CFG_TAG, "FRAM init failed");
        return false;
    }

    memset(out, 0, sizeof(*out));
    return Fram_Read_Data(WIFI_FRAM_ADDR, (uint8_t *)out, sizeof(*out));
}

static bool prv_fram_write_wifi(const wifi_fram_t *in)
{
    if(in == NULL)
    {
        return false;
    }

    if(!Fram_Init())
    {
        ESP_LOGE(WIFI_CFG_TAG, "FRAM init failed");
        return false;
    }

    Fram_Write_Enable();
    Fram_Write_Data(WIFI_FRAM_ADDR, (uint8_t *)in, sizeof(*in));
    return true;
}

static void prv_wifi_connect(const char *ssid, const char *pass)
{
    wifi_config_t wifi_config = {0};

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if(ssid != NULL)
    {
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    }
    if(pass != NULL)
    {
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    }

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(WIFI_CFG_TAG, "Connecting to SSID: %s", (char *)wifi_config.sta.ssid);
    esp_wifi_connect();
}

// static void prv_wifi_restart(wifi_mode_t mode, const wifi_config_t *ap_config)
// {
//     esp_err_t stop_err = esp_wifi_stop();
//     if(stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED)
//     {
//         ESP_ERROR_CHECK(stop_err);
//     }

//     ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
// #if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
//     if(ap_config != NULL)
//     {
//         ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, ap_config));
//     }
// #endif
//     ESP_ERROR_CHECK(esp_wifi_start());

//     if((mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) && s_sta_netif != NULL)
//     {
//         esp_err_t dhcp_err = esp_netif_dhcpc_start(s_sta_netif);
//         if(dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
//         {
//             ESP_LOGW(WIFI_CFG_TAG, "DHCP start failed: %s", esp_err_to_name(dhcp_err));
//         }
//     }
// }

static void prv_build_ap_config(wifi_config_t *ap_config)
{
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    if(ap_config == NULL)
    {
        return;
    }
    memset(ap_config, 0, sizeof(*ap_config));
    
    // Gắn toàn bộ 12 ký tự MAC vào sau chữ MEBICO_CONFIG
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    
    char dynamic_ssid[32];
    snprintf(dynamic_ssid, sizeof(dynamic_ssid), "%s_%02X%02X%02X%02X%02X%02X", WIFI_AP_SSID, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    strncpy((char *)ap_config->ap.ssid, dynamic_ssid, sizeof(ap_config->ap.ssid) - 1);
    strncpy((char *)ap_config->ap.password, WIFI_AP_PASS, sizeof(ap_config->ap.password) - 1);
    ap_config->ap.ssid_len = strlen((char *)ap_config->ap.ssid);
    ap_config->ap.channel = WIFI_AP_CHANNEL;
    ap_config->ap.max_connection = WIFI_AP_MAX_CONN;
    ap_config->ap.authmode = (strlen(WIFI_AP_PASS) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
#else
    (void)ap_config;
#endif
}

static void prv_connect_task(void *pvParameters)
{
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(300));

    s_allow_sta_connect = true;
    s_retry_num = 0;

#if CONFIG_ESP_WIFI_STA_SUPPORT
    if(s_sta_netif != NULL)
    {
        esp_err_t dhcp_err = esp_netif_dhcpc_start(s_sta_netif);
        if(dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        {
            ESP_LOGW(WIFI_CFG_TAG, "DHCP start failed: %s", esp_err_to_name(dhcp_err));
        }
    }
#endif
    prv_wifi_connect(s_pending_ssid, s_pending_pass);
    s_connect_task = NULL;
    vTaskDelete(NULL);
}

// static void prv_set_mode_ap_only(void)
// {
// #if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
// #else
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
// #endif
// }

// static void prv_set_mode_apsta(void)
// {
// #if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
// #else
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
// #endif
// }

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        bool is_config_mode = User_Input_Check_Is_Active(IO_CONFIG_INPUT_2);
        if(s_allow_sta_connect && !is_config_mode)
        {
            esp_wifi_connect();
        }
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        s_is_associated = true;
        ESP_LOGI(WIFI_CFG_TAG, "Associated with AP. Waiting for DHCP IP...");
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *disc_event = (wifi_event_sta_disconnected_t *)event_data;
        char details[128];
        snprintf(details, sizeof(details), "Reason Code: %d", disc_event ? (int)disc_event->reason : 0);
        User_External_Flash_Log_Event("WIFI_DISCONNECTED", details);

        Sys_Info.isWifiConnected = false;
        Sys_Info.isTimeSync = false; // Reset cờ đồng bộ thời gian khi mất WiFi
        s_is_associated = false;
        IoTHubHandle.isAzureInitialized = false; // Đánh dấu Azure rớt kết nối ngay khi ngắt Wi-Fi
        IoTHubHandle.isNeedReinit = true;        // Yêu cầu Azure Reinit lại Socket khi có lại Wi-Fi
        bool is_config_mode = User_Input_Check_Is_Active(IO_CONFIG_INPUT_2);
        if(s_allow_sta_connect && !is_config_mode)
        {
            if(s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
            {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(WIFI_CFG_TAG, "Fast retry %d/%d", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
            }
            else
            {
                if(s_slow_retry_task == NULL)
                {
                    s_retry_num = 0; // Reset để lần disconnect tiếp theo fast retry hoạt động lại
                    xTaskCreatePinnedToCore(prv_slow_retry_task, "wifi_slow_retry", 3072, NULL, 3, &s_slow_retry_task, 0);
                }
                ESP_LOGI(WIFI_CFG_TAG, "Switch to slow retry every %d s", WIFI_SLOW_RETRY_INTERVAL_MS/1000);
            }
        }
        else if (is_config_mode)
        {
            ESP_LOGW(WIFI_CFG_TAG, "Disconnected. Config Mode active (Switch 2 ON) -> Suspending retry connect.");
            if(s_slow_retry_task == NULL)
            {
                xTaskCreatePinnedToCore(prv_slow_retry_task, "wifi_slow_retry", 3072, NULL, 3, &s_slow_retry_task, 0);
            }
        }
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_retry_num = 0;
        Sys_Info.isWifiConnected = true;
        s_is_associated = false;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        wifi_ap_record_t ap_info;
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            rssi = ap_info.rssi;
        }

        char details[128];
        snprintf(details, sizeof(details), "IP: " IPSTR " | RSSI: %ddBm", IP2STR(&event->ip_info.ip), rssi);
        User_External_Flash_Log_Event("WIFI_CONNECTED", details);

        ESP_LOGI(WIFI_CFG_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        char ip_str[16] = {0};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        Fram_Write_Data(FRAM_IP_ADDR_ADDR, (uint8_t *)ip_str, 16);
        ESP_LOGI(WIFI_CFG_TAG, "Saved IP %s to FRAM address 0x%X", ip_str, FRAM_IP_ADDR_ADDR);
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        Sys_Info.isApHasClient = true;
        ESP_LOGI(WIFI_CFG_TAG, "Device connected to AP configuration network");
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        Sys_Info.isApHasClient = false;
        ESP_LOGI(WIFI_CFG_TAG, "Device disconnected from AP configuration network");
    }
}

bool wifi_config_manager_load(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    wifi_fram_t cred;
    if(!prv_fram_read_wifi(&cred))
    {
        return false;
    }

    if(cred.magic != WIFI_CRED_MAGIC)
    {
        return false;
    }

    if(ssid_out != NULL && ssid_len > 0)
    {
        strncpy(ssid_out, cred.ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
    }
    if(pass_out != NULL && pass_len > 0)
    {
        strncpy(pass_out, cred.pass, pass_len - 1);
        pass_out[pass_len - 1] = '\0';
    }

    return true;
}

bool wifi_config_manager_save(const char *ssid, const char *pass)
{
    if(ssid == NULL || ssid[0] == '\0')
    {
        return false;
    }

    wifi_fram_t cred;
    memset(&cred, 0, sizeof(cred));
    cred.magic = WIFI_CRED_MAGIC;
    strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
    if(pass != NULL)
    {
        strncpy(cred.pass, pass, sizeof(cred.pass) - 1);
    }

    if(!prv_fram_write_wifi(&cred))
    {
        return false;
    }

    memset(TCP_Handle.ssid, 0, sizeof(TCP_Handle.ssid));
    memset(TCP_Handle.pass, 0, sizeof(TCP_Handle.pass));
    strncpy(TCP_Handle.ssid, cred.ssid, sizeof(TCP_Handle.ssid) - 1);
    strncpy(TCP_Handle.pass, cred.pass, sizeof(TCP_Handle.pass) - 1);

    strncpy(s_pending_ssid, cred.ssid, sizeof(s_pending_ssid) - 1);
    strncpy(s_pending_pass, cred.pass, sizeof(s_pending_pass) - 1);
    return true;
}

bool wifi_config_manager_clear(void)
{
    wifi_fram_t cred;
    memset(&cred, 0, sizeof(cred));
    return prv_fram_write_wifi(&cred);
}

static bool prv_fram_read_azure(azure_fram_t *out)
{
    if(out == NULL) return false;
    if(!Fram_Init()) return false;
    memset(out, 0, sizeof(*out));
    return Fram_Read_Data(AZURE_FRAM_ADDR, (uint8_t *)out, sizeof(*out));
}

static bool prv_fram_write_azure(const azure_fram_t *in)
{
    if(in == NULL) return false;
    if(!Fram_Init()) return false;
    Fram_Write_Enable();
    Fram_Write_Data(AZURE_FRAM_ADDR, (uint8_t *)in, sizeof(*in));
    return true;
}

bool azure_config_manager_load(char *host, size_t host_len, char *dev, size_t dev_len, char *sym, size_t sym_len)
{
    azure_fram_t cred;
    if(!prv_fram_read_azure(&cred)) return false;
    if(cred.magic != AZURE_CRED_MAGIC) return false;
    
    if(host != NULL && host_len > 0)
    {
        strncpy(host, cred.hostName, host_len - 1);
        host[host_len - 1] = '\0';
    }
    if(dev != NULL && dev_len > 0)
    {
        strncpy(dev, cred.deviceId, dev_len - 1);
        dev[dev_len - 1] = '\0';
    }
    if(sym != NULL && sym_len > 0)
    {
        strncpy(sym, cred.symmetricKey, sym_len - 1);
        sym[sym_len - 1] = '\0';
    }
    return true;
}

bool azure_config_manager_save(const char *host, const char *dev, const char *sym)
{
    if(host == NULL || dev == NULL || sym == NULL) return false;
    // Basic length limits handling handled by strncpy
    
    azure_fram_t cred;
    memset(&cred, 0, sizeof(cred));
    cred.magic = AZURE_CRED_MAGIC;
    strncpy(cred.hostName, host, sizeof(cred.hostName) - 1);
    strncpy(cred.deviceId, dev, sizeof(cred.deviceId) - 1);
    strncpy(cred.symmetricKey, sym, sizeof(cred.symmetricKey) - 1);
    
    return prv_fram_write_azure(&cred);
}

bool azure_config_manager_clear(void)
{
    azure_fram_t cred;
    memset(&cred, 0, sizeof(cred));
    return prv_fram_write_azure(&cred);
}

void wifi_config_manager_schedule_connect(void)
{
    if(s_connect_task != NULL)
    {
        return;
    }

    xTaskCreatePinnedToCore(prv_connect_task, "wifi_connect_task", 4096, NULL, 4, &s_connect_task, 0);
}

void wifi_config_manager_prepare_scan(void)
{
    // No-op: keep AP running to avoid dropping HTTP connection during scan.
}

static void prv_wifi_mode_monitor_task(void *pvParameters)
{
    static bool s_is_ap_active = false;
    // Initial state reflection
    s_is_ap_active = User_Input_Check_Is_Active(IO_CONFIG_INPUT_2);

    while(1)
    {
        bool current_switch_state = User_Input_Check_Is_Active(IO_CONFIG_INPUT_2);
        
        if (current_switch_state && !s_is_ap_active)
        {
            ESP_LOGI(WIFI_CFG_TAG, "Hardware switch 2 toggled ON -> Re-enabling APSTA mode");
            wifi_config_t ap_config;
            prv_build_ap_config(&ap_config);
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            dns_server_start(); // Start DNS server when AP is enabled
            s_is_ap_active = true;
        }
        else if (!current_switch_state && s_is_ap_active)
        {
            ESP_LOGI(WIFI_CFG_TAG, "Hardware switch 2 toggled OFF -> Disabling SoftAP, routing to STA only");
            esp_wifi_set_mode(WIFI_MODE_STA);
            dns_server_stop(); // Stop DNS server when AP is disabled
            s_is_ap_active = false;
            
            // Kích hoạt kết nối lại Wi-Fi ngay lập tức khi thoát chế độ cấu hình
            if(s_allow_sta_connect && !Sys_Info.isWifiConnected)
            {
                ESP_LOGI(WIFI_CFG_TAG, "Switch 2 OFF -> Triggering immediate STA connection");
                s_retry_num = 0;
                esp_wifi_connect();
            }
        }
        
        Sys_Info.isApActive = s_is_ap_active;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool wifi_config_manager_init(void)
{
    esp_err_t err;

    err = esp_netif_init();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(WIFI_CFG_TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(WIFI_CFG_TAG, "event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    s_ap_netif = esp_netif_create_default_wifi_ap();
#else
    ESP_LOGW(WIFI_CFG_TAG, "SoftAP support disabled in sdkconfig");
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t ap_config;
    prv_build_ap_config(&ap_config);

    char ssid[32] = {0};
    char pass[64] = {0};
    bool has_saved = wifi_config_manager_load(ssid, sizeof(ssid), pass, sizeof(pass)) && (ssid[0] != '\0');

    s_allow_sta_connect = has_saved ? true : false;

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    if(User_Input_Check_Is_Active(IO_CONFIG_INPUT_2) == true)
    {
        ESP_LOGI(WIFI_CFG_TAG, "Hardware switch 2 ON -> Emitting Broadcast AP Config");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        dns_server_start(); // Start DNS for Captive Portal
    }
    else
    {
        ESP_LOGI(WIFI_CFG_TAG, "Hardware switch 2 OFF -> AP suppressed, connecting STA exclusively");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());

    if(s_sta_netif != NULL)
    {
        esp_err_t dhcp_err = esp_netif_dhcpc_start(s_sta_netif);
        if(dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        {
            ESP_LOGW(WIFI_CFG_TAG, "DHCP start failed: %s", esp_err_to_name(dhcp_err));
        }
    }

    if(has_saved)
    {
        strncpy(TCP_Handle.ssid, ssid, sizeof(TCP_Handle.ssid) - 1);
        strncpy(TCP_Handle.pass, pass, sizeof(TCP_Handle.pass) - 1);
        prv_wifi_connect(ssid, pass);
        ESP_LOGI(WIFI_CFG_TAG, "Saved WiFi found, AP+STA enabled");
    }
    else
    {
        ESP_LOGI(WIFI_CFG_TAG, "No saved WiFi, AP only");
    }

    ESP_LOGI(WIFI_CFG_TAG, "AP configuration complete");

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // Create daemon to continually monitor switch 2 logic and pivot modes
    xTaskCreatePinnedToCore(prv_wifi_mode_monitor_task, "wifi_mode_mon", 3072, NULL, 3, NULL, 0);
#endif

    return true;
}
