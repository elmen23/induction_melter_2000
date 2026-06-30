#pragma once

#include "esp_err.h"
#include <cstddef>   // size_t
#include <cstdint>   // uint32_t

#ifdef __cplusplus
extern "C" {
#endif

/* ─── WiFi AP Configuration ─── */
#define WIFI_AP_SSID        "IH-2000"
#define WIFI_AP_PASS        "induction2000"
#define WIFI_AP_CHANNEL      1
#define WIFI_AP_MAX_CONN     4

/* ─── Initialisation ─── */
esp_err_t wifi_init_ap(void);
esp_err_t wifi_init_ap_sta(void);
esp_err_t wifi_init_sta(const char *ssid, const char *pass);

/* ─── Status ─── */
bool      wifi_is_connected(void);
const char *wifi_get_mode_str(void);
const char *wifi_get_ip_str(void);
void      wifi_get_ip(char *buf, size_t len);
const char *wifi_get_sta_ssid(void);
uint32_t  wifi_get_ap_sta_count(void);

/* ─── Lifecycle ─── */
void      wifi_stop(void);

/* ─── Credential storage (NVS) ─── */
esp_err_t wifi_save_credentials(const char *ssid, const char *pass);
esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
void      wifi_clear_credentials(void);

/* ─── WiFi Scan (non-blocking) ─── */
esp_err_t wifi_scan_start(void);        // non-blocking, returns immediately
bool     wifi_scan_is_done(void);        // check if scan completed
esp_err_t wifi_scan_get_results(void);   // fetch results from driver
int      wifi_get_scan_count(void);
const char *wifi_get_scan_ssid(int index);
int16_t  wifi_get_scan_rssi(int index);
uint8_t  wifi_get_scan_auth(int index);

/* ─── STA connection control ─── */
esp_err_t wifi_connect_sta(const char *ssid, const char *pass);
void      wifi_disconnect_sta(void);

#ifdef __cplusplus
}
#endif
