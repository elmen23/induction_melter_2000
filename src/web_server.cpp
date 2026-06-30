#include "web_server.h"
#include "mcpwm_control.h"
#include "wifi_manager.h"
#include "esp_wifi.h"  // WIFI_AUTH_* constants
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <cstring>

static const char *TAG = "HTTP";

static httpd_handle_t s_server = nullptr;

#include "embedded_files.h"

static esp_err_t serve_file(httpd_req_t *req, const char *mime, const uint8_t *data, size_t len) {
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, (const char *)data, len);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  Static file handler
 * ════════════════════════════════════════════ */

struct FileMapping {
    const char *uri;
    const char *mime;
    const uint8_t *data;
    size_t len;
};

static const FileMapping s_files[] = {
    { "/",          "text/html",                index_html,  index_html_len },
    { "/index.html","text/html; charset=utf-8",  index_html,  index_html_len },
    { "/style.css", "text/css; charset=utf-8",   style_css,   style_css_len },
    { "/script.js", "application/javascript",    script_js,   script_js_len },
};

static esp_err_t handler_get_static(httpd_req_t *req) {
    for (auto &f : s_files) {
        if (strcmp(req->uri, f.uri) == 0) {
            return serve_file(req, f.mime, f.data, f.len);
        }
    }
    // Fallback: serve index.html (SPA routing)
    return serve_file(req, s_files[0].mime, s_files[0].data, s_files[0].len);
}

/* ════════════════════════════════════════════
 *  GET /api/config
 * ════════════════════════════════════════════ */

static esp_err_t handler_get_config(httpd_req_t *req) {
    InductionConfig cfg;
    mcpwm_get_config(cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enable",         cfg.enable);
    cJSON_AddNumberToObject(root, "frequency",     cfg.frequency_hz / 1000.0);  // send as kHz
    cJSON_AddNumberToObject(root, "duty",          cfg.duty_percent);
    cJSON_AddNumberToObject(root, "dead_time_red", (double)cfg.dead_time_red_ns);
    cJSON_AddNumberToObject(root, "dead_time_fed", (double)cfg.dead_time_fed_ns);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  POST /api/config
 * ════════════════════════════════════════════ */

static esp_err_t handler_post_config(httpd_req_t *req) {
    // Read body
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    InductionConfig cfg;
    mcpwm_get_config(cfg);

    // Parse each field if present
    cJSON *item;

    item = cJSON_GetObjectItem(root, "enable");
    if (cJSON_IsBool(item)) cfg.enable = item->valueint;

    item = cJSON_GetObjectItem(root, "frequency");
    if (cJSON_IsNumber(item)) {
        float freq_khz = (float)item->valuedouble;
        cfg.frequency_hz = (uint32_t)(freq_khz * 1000);
        if (cfg.frequency_hz < PWM_FREQ_MIN) cfg.frequency_hz = PWM_FREQ_MIN;
        if (cfg.frequency_hz > PWM_FREQ_MAX) cfg.frequency_hz = PWM_FREQ_MAX;
    }

    item = cJSON_GetObjectItem(root, "duty");
    if (cJSON_IsNumber(item)) {
        cfg.duty_percent = (float)item->valuedouble;
        if (cfg.duty_percent < PWM_DUTY_MIN) cfg.duty_percent = PWM_DUTY_MIN;
        if (cfg.duty_percent > PWM_DUTY_MAX) cfg.duty_percent = PWM_DUTY_MAX;
    }

    item = cJSON_GetObjectItem(root, "dead_time_red");
    if (cJSON_IsNumber(item)) {
        cfg.dead_time_red_ns = (uint32_t)item->valuedouble;
        if (cfg.dead_time_red_ns > DEADTIME_MAX) cfg.dead_time_red_ns = DEADTIME_MAX;
    }

    item = cJSON_GetObjectItem(root, "dead_time_fed");
    if (cJSON_IsNumber(item)) {
        cfg.dead_time_fed_ns = (uint32_t)item->valuedouble;
        if (cfg.dead_time_fed_ns > DEADTIME_MAX) cfg.dead_time_fed_ns = DEADTIME_MAX;
    }

    cJSON_Delete(root);

    esp_err_t err = mcpwm_apply_config(cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to apply config");
        return ESP_FAIL;
    }

    // Return the applied config as confirmation
    return handler_get_config(req);
}

/* ════════════════════════════════════════════
 *  GET /api/status
 * ════════════════════════════════════════════ */

static esp_err_t handler_get_status(httpd_req_t *req) {
    InductionConfig cfg;
    mcpwm_get_config(cfg);

    InductionFeedback fb;
    mcpwm_get_feedback(fb);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,    "enable",      cfg.enable);
    cJSON_AddNumberToObject(root,  "power",       fb.power_kw);
    cJSON_AddNumberToObject(root,  "voltage",     fb.voltage);
    cJSON_AddNumberToObject(root,  "current",     fb.current_a);
    cJSON_AddNumberToObject(root,  "temperature", fb.temperature_c);
    cJSON_AddBoolToObject(root,    "connected",   wifi_is_connected());
    cJSON_AddStringToObject(root,  "wifi_mode",   wifi_get_mode_str());
    cJSON_AddStringToObject(root,  "wifi_ip",     wifi_get_ip_str());
    cJSON_AddNumberToObject(root,  "uptime_sec",  (double)(esp_timer_get_time() / 1000000));

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  POST /api/estop
 * ════════════════════════════════════════════ */

static esp_err_t handler_post_estop(httpd_req_t *req) {
    ESP_LOGW(TAG, "*** EMERGENCY STOP via HTTP ***");
    mcpwm_emergency_stop();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ESTOPPED");
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  CORS & common headers
 * ════════════════════════════════════════════ */

static esp_err_t common_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

static esp_err_t handler_options(httpd_req_t *req) {
    common_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  GET /api/wifi  —  WiFi status
 * ════════════════════════════════════════════ */
static esp_err_t wifi_get_handler(httpd_req_t *r) {
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

    char sta_ssid[33] = "";
    char sta_pass[65] = "";
    esp_err_t cred_ret = wifi_load_credentials(sta_ssid, sizeof(sta_ssid),
                                                sta_pass, sizeof(sta_pass));
    bool has_creds = (cred_ret == ESP_OK && strlen(sta_ssid) > 0);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", wifi_get_mode_str());
    cJSON_AddStringToObject(root, "ap_ssid", WIFI_AP_SSID);
    cJSON_AddStringToObject(root, "ap_ip", "192.168.4.1");
    cJSON_AddStringToObject(root, "ip", wifi_get_ip_str());
    cJSON_AddBoolToObject(root, "sta_connected", wifi_is_connected());
    cJSON_AddBoolToObject(root, "has_credentials", has_creds);
    cJSON_AddStringToObject(root, "sta_ssid", has_creds ? sta_ssid : "");
    cJSON_AddNumberToObject(root, "ap_clients", wifi_get_ap_sta_count());

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  POST /api/wifi  —  Set WiFi credentials & connect
 * ════════════════════════════════════════════ */
static esp_err_t wifi_post_handler(httpd_req_t *r) {
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

    char buf[256] = {0};
    int received = httpd_req_recv(r, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "SSID required");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *ssid = ssid_item->valuestring;
    const char *pass = (cJSON_IsString(pass_item)) ? pass_item->valuestring : "";

    esp_err_t err = wifi_connect_sta(ssid, pass);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddStringToObject(resp, "ssid", ssid);

    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    free(json);
    cJSON_Delete(root);
    cJSON_Delete(resp);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  POST /api/wifi/forget  —  Clear credentials & disconnect STA
 * ════════════════════════════════════════════ */
static esp_err_t wifi_forget_handler(httpd_req_t *r) {
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

    wifi_disconnect_sta();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);

    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}


/* ════════════════════════════════════════════
 *  GET /api/wifi/scan  —  Scan WiFi networks
 * ════════════════════════════════════════════ */
/* ─── POST: start scan (non-blocking) ─── */
static esp_err_t wifi_scan_start_handler(httpd_req_t *r) {
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

    esp_err_t err = wifi_scan_start();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(root, "status", "scanning");
    } else {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ─── GET: poll scan results ─── */
static esp_err_t wifi_scan_poll_handler(httpd_req_t *r) {
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

    cJSON *root = cJSON_CreateObject();

    if (!wifi_scan_is_done()) {
        cJSON_AddStringToObject(root, "status", "scanning");
    } else {
        // Results are ready — fetch them
        esp_err_t err = wifi_scan_get_results();
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "status", err == ESP_OK ? "done" : "error");

        if (err == ESP_OK) {
            int count = wifi_get_scan_count();
            cJSON_AddNumberToObject(root, "count", count);

            cJSON *networks = cJSON_AddArrayToObject(root, "networks");
            for (int i = 0; i < count; i++) {
                cJSON *net = cJSON_CreateObject();
                cJSON_AddStringToObject(net, "ssid", wifi_get_scan_ssid(i));
                cJSON_AddNumberToObject(net, "rssi", wifi_get_scan_rssi(i));

                const char *auth_str = "OPEN";
                uint8_t auth = wifi_get_scan_auth(i);
                if (auth == WIFI_AUTH_WEP) auth_str = "WEP";
                else if (auth == WIFI_AUTH_WPA_PSK) auth_str = "WPA";
                else if (auth == WIFI_AUTH_WPA2_PSK) auth_str = "WPA2";
                else if (auth == WIFI_AUTH_WPA_WPA2_PSK) auth_str = "WPA/WPA2";
                else if (auth == WIFI_AUTH_WPA3_PSK) auth_str = "WPA3";
                else if (auth == WIFI_AUTH_WPA2_WPA3_PSK) auth_str = "WPA2/WPA3";
                cJSON_AddStringToObject(net, "auth", auth_str);

                cJSON_AddItemToArray(networks, net);
            }
        } else {
            cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}


static const httpd_uri_t s_routes[] = {
    { .uri = "/",           .method = HTTP_GET,    .handler = handler_get_static,  .user_ctx = nullptr },
    { .uri = "/index.html", .method = HTTP_GET,    .handler = handler_get_static,  .user_ctx = nullptr },
    { .uri = "/style.css",  .method = HTTP_GET,    .handler = handler_get_static,  .user_ctx = nullptr },
    { .uri = "/script.js",  .method = HTTP_GET,    .handler = handler_get_static,  .user_ctx = nullptr },

    { .uri = "/api/config", .method = HTTP_GET,    .handler = handler_get_config,  .user_ctx = nullptr },
    { .uri = "/api/config", .method = HTTP_POST,   .handler = handler_post_config, .user_ctx = nullptr },
    { .uri = "/api/config", .method = HTTP_OPTIONS,.handler = handler_options,     .user_ctx = nullptr },

    { .uri = "/api/status", .method = HTTP_GET,    .handler = handler_get_status,  .user_ctx = nullptr },
    { .uri = "/api/status", .method = HTTP_OPTIONS,.handler = handler_options,     .user_ctx = nullptr },

    { .uri = "/api/estop",  .method = HTTP_POST,   .handler = handler_post_estop,  .user_ctx = nullptr },
    { .uri = "/api/estop",  .method = HTTP_OPTIONS,.handler = handler_options,     .user_ctx = nullptr },

    { .uri = "/api/wifi",       .method = HTTP_GET,    .handler = wifi_get_handler,      .user_ctx = nullptr },
    { .uri = "/api/wifi",       .method = HTTP_POST,   .handler = wifi_post_handler,     .user_ctx = nullptr },
    { .uri = "/api/wifi",       .method = HTTP_OPTIONS,.handler = handler_options,       .user_ctx = nullptr },
    { .uri = "/api/wifi/forget",.method = HTTP_POST,   .handler = wifi_forget_handler,   .user_ctx = nullptr },    { .uri = "/api/wifi/scan",       .method = HTTP_POST,   .handler = wifi_scan_start_handler, .user_ctx = nullptr },    { .uri = "/api/wifi/scan",       .method = HTTP_GET,    .handler = wifi_scan_poll_handler,  .user_ctx = nullptr },
};

/* ════════════════════════════════════════════
 *  Init / Deinit
 * ════════════════════════════════════════════ */

esp_err_t web_server_init(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 4096;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    for (auto &route : s_routes) {
        httpd_register_uri_handler(s_server, &route);
    }

    ESP_LOGI(TAG, "HTTP server started on port %u", cfg.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    return ESP_OK;
}
