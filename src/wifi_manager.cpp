#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstring>

static constexpr const char *TAG   = "WiFi";
static constexpr const char *NVS_NS = "wifi_store";

static esp_netif_t *s_netif_ap  = nullptr;
static esp_netif_t *s_netif_sta = nullptr;
static char         s_ip_str[16]    = "0.0.0.0";
static char         s_sta_ssid[33]  = "";
static uint32_t     s_ap_sta_count  = 0;
static volatile bool s_scan_in_progress = false;  // non-blocking scan flag
/* ─── WiFi scan state ─── */
#define WIFI_SCAN_MAX_APS  20
static wifi_ap_record_t s_scan_aps[WIFI_SCAN_MAX_APS];
static uint16_t          s_scan_count = 0;


/* ─── Forward declarations ─── */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static esp_err_t wifi_common_init(void);

/* ════════════════════════════════════════════
 *  Internal helpers
 * ════════════════════════════════════════════ */

static void build_ap_config(wifi_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->ap.ssid, WIFI_AP_SSID, sizeof(WIFI_AP_SSID));
    memcpy(cfg->ap.password, WIFI_AP_PASS, sizeof(WIFI_AP_PASS));
    cfg->ap.ssid_len      = 0;
    cfg->ap.channel        = WIFI_AP_CHANNEL;
    cfg->ap.authmode       = WIFI_AUTH_WPA_WPA2_PSK;
    cfg->ap.max_connection = WIFI_AP_MAX_CONN;
}

static void build_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(reinterpret_cast<char *>(cfg->sta.ssid), ssid, sizeof(cfg->sta.ssid) - 1);
    if (pass && pass[0] != '\0') {
        strncpy(reinterpret_cast<char *>(cfg->sta.password), pass, sizeof(cfg->sta.password) - 1);
    }
}

/* ════════════════════════════════════════════
 *  Event handler
 * ════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED) {
            s_ap_sta_count++;
            ESP_LOGI(TAG, "Station joined AP (total: %u)", s_ap_sta_count);
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (s_ap_sta_count > 0) s_ap_sta_count--;
            ESP_LOGI(TAG, "Station left AP (total: %u)", s_ap_sta_count);
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "STA connected to router");
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            s_scan_in_progress = false;
            ESP_LOGI(TAG, "WiFi scan completed");
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "STA disconnected — will reconnect");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            auto *event = static_cast<ip_event_got_ip_t *>(data);
            esp_ip4addr_ntoa(&event->ip_info.ip, s_ip_str, sizeof(s_ip_str));
            ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGW(TAG, "STA lost IP");
        }
    }
}

/* ════════════════════════════════════════════
 *  Common initialisation (NVS, netif, event loop)
 * ════════════════════════════════════════════ */

static esp_err_t wifi_common_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase required — erasing and re-init");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  WiFi modes
 * ════════════════════════════════════════════ */

esp_err_t wifi_init_ap(void) {
    wifi_common_init();

    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));

    wifi_config_t ap_cfg;
    build_ap_config(&ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    strcpy(s_ip_str, "192.168.4.1");
    ESP_LOGI(TAG, "AP started  SSID:'%s'  IP:192.168.4.1", WIFI_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_init_ap_sta(void) {
    wifi_common_init();

    s_netif_ap  = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,   ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));

    /* ── AP config ── */
    wifi_config_t ap_cfg;
    build_ap_config(&ap_cfg);

    /* ── STA config (try saved credentials) ── */
    char saved_ssid[33] = "";
    char saved_pass[65] = "";
    bool has_creds = (wifi_load_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) == ESP_OK
                      && saved_ssid[0] != '\0');

    wifi_config_t sta_cfg = {};
    if (has_creds) {
        build_sta_config(&sta_cfg, saved_ssid, saved_pass);
        strncpy(s_sta_ssid, saved_ssid, sizeof(s_sta_ssid) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    if (has_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    if (has_creds) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "AP+STA started — connecting to '%s'", saved_ssid);
    }

    strcpy(s_ip_str, "192.168.4.1");
    ESP_LOGI(TAG, "AP active  SSID:'%s'  IP:192.168.4.1", WIFI_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_init_sta(const char *ssid, const char *pass) {
    wifi_common_init();

    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr));

    wifi_config_t sta_cfg;
    build_sta_config(&sta_cfg, ssid, pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    ESP_LOGI(TAG, "STA connecting to '%s' ...", ssid);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  NVS Credential Storage
 * ════════════════════════════════════════════ */

esp_err_t wifi_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "sta_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "sta_pass", pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
        ESP_LOGI(TAG, "Credentials saved to NVS");
    }
    return err;
}

esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = ssid_len;
    err = nvs_get_str(h, "sta_ssid", ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(h, "sta_pass", pass, &len);
    }
    nvs_close(h);
    return err;
}

void wifi_clear_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_sta_ssid[0] = '\0';
    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
}

/* ════════════════════════════════════════════
 *  STA Connection Control
 * ════════════════════════════════════════════ */

esp_err_t wifi_connect_sta(const char *ssid, const char *pass) {
    esp_err_t err = wifi_save_credentials(ssid, pass);
    if (err != ESP_OK) return err;

    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));

    if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
        wifi_config_t sta_cfg;
        build_sta_config(&sta_cfg, ssid, pass);

        if (mode == WIFI_MODE_AP) {
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    ESP_LOGI(TAG, "Connecting STA to '%s'", ssid);
    return ESP_OK;
}

void wifi_disconnect_sta(void) {
    wifi_clear_credentials();
    esp_wifi_disconnect();

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    strcpy(s_ip_str, "192.168.4.1");
    ESP_LOGI(TAG, "STA disconnected, AP remains active");
}



/* ════════════════════════════════════════════
 *  WiFi Scan
 * ════════════════════════════════════════════ */

esp_err_t wifi_scan_start(void) {
    // Clear previous results
    s_scan_count = 0;
    memset(s_scan_aps, 0, sizeof(s_scan_aps));
    s_scan_in_progress = true;

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.ssid = nullptr;
    scan_cfg.bssid = nullptr;
    scan_cfg.channel = 0;
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);  // non-blocking!
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        s_scan_in_progress = false;
    }
    return err;
}

bool wifi_scan_is_done(void) {
    return !s_scan_in_progress;
}

esp_err_t wifi_scan_get_results(void) {
    uint16_t count = WIFI_SCAN_MAX_APS;
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_aps);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan get records failed: %s", esp_err_to_name(err));
        return err;
    }
    s_scan_count = count;
    ESP_LOGI(TAG, "Scan results: %d networks", count);
    return ESP_OK;
}

int wifi_get_scan_count(void) {
    return s_scan_count;
}

const char *wifi_get_scan_ssid(int index) {
    if (index < 0 || index >= s_scan_count) return "";
    return reinterpret_cast<const char *>(s_scan_aps[index].ssid);
}

int16_t wifi_get_scan_rssi(int index) {
    if (index < 0 || index >= s_scan_count) return 0;
    return s_scan_aps[index].rssi;
}

uint8_t wifi_get_scan_auth(int index) {
    if (index < 0 || index >= s_scan_count) return 0;
    return s_scan_aps[index].authmode;
}

/* ════════════════════════════════════════════
 *  Status / Getter Functions
 * ════════════════════════════════════════════ */

bool wifi_is_connected(void) {
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

const char *wifi_get_mode_str(void) {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return "NONE";
    switch (mode) {
        case WIFI_MODE_AP:     return "AP";
        case WIFI_MODE_STA:    return "STA";
        case WIFI_MODE_APSTA:  return "AP+STA";
        default:               return "NONE";
    }
}

const char *wifi_get_ip_str(void)      { return s_ip_str; }
const char *wifi_get_sta_ssid(void)    { return s_sta_ssid; }
uint32_t wifi_get_ap_sta_count(void)  { return s_ap_sta_count; }

void wifi_get_ip(char *buf, size_t len) {
    strncpy(buf, s_ip_str, len - 1);
    buf[len - 1] = '\0';
}

void wifi_stop(void) {
    esp_wifi_stop();
    esp_wifi_deinit();
}
