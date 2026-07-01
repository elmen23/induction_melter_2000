#include "mcpwm_control.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

/* ─── Forward declarations ─── */
static void feedback_task(void *pv_param);

/* ════════════════════════════════════════════
 *  app_main
 * ════════════════════════════════════════════ */

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  IH-2000 Induction Heater v1.0");
    ESP_LOGI(TAG, "========================================");

    /* ── 1. Init MCPWM ── */
    ESP_LOGI(TAG, "[1/4] Initialising MCPWM ...");
    esp_err_t err = mcpwm_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MCPWM init FAILED — reboot in 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    /* ── 2. Start WiFi (AP+STA — AP always on, try STA with saved creds) ── */
    ESP_LOGI(TAG, "[2/4] Starting WiFi (AP mode) ...");
    err = wifi_init_ap_sta();  // AP always on + try STA with saved creds
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init FAILED — continuing without network");
    }

    /* ── 3. Start HTTP server ── */
    ESP_LOGI(TAG, "[3/4] Starting HTTP server ...");
    err = web_server_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server FAILED — continuing with MCPWM only");
    }

    /* ── 4. Start feedback task ── */
    ESP_LOGI(TAG, "[4/4] Starting feedback monitor task ...");
    xTaskCreatePinnedToCore(
        feedback_task,
        "feedback",
        4096,
        nullptr,
        tskIDLE_PRIORITY + 2,
        nullptr,
        1  // core 1
    );

    ESP_LOGI(TAG, "────────────────────────────────────");
    ESP_LOGI(TAG, "System ready!");
    ESP_LOGI(TAG, " WiFi:     Mode=%s  AP='%s'  IP=%s", wifi_get_mode_str(), WIFI_AP_SSID, wifi_get_ip_str());
    ESP_LOGI(TAG, " Web UI:   http://%s", wifi_get_ip_str());
    ESP_LOGI(TAG, " PWM Out:  A=GPIO%d  B=GPIO%d", GPIO_PWM_A, GPIO_PWM_B);
    ESP_LOGI(TAG, "────────────────────────────────────");

    // Main loop: just log stats periodically
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick += 5;

        InductionConfig cfg;
        InductionFeedback fb;
        mcpwm_get_config(cfg);
        mcpwm_get_feedback(fb);

        ESP_LOGD(TAG, "[%us] %s | %u kHz  %.1f%%  DT:%u/%u ns  |  %.1f kW  %.0f°C",
                 tick,
                 cfg.enable ? "RUN" : "STOP",
                 cfg.frequency_hz / 1000, cfg.duty_percent,
                 cfg.dead_time_red_ns, cfg.dead_time_fed_ns,
                 fb.power_kw, fb.temperature_c);
    }
}

/* ════════════════════════════════════════════
 *  Feedback task (runs on core 1)
 *  Updates feedback values every 250ms
 * ════════════════════════════════════════════ */

static void feedback_task(void *pv_param) {
    (void)pv_param;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(250);

    while (true) {
        // Read sensors / update feedback
        InductionFeedback fb;
        mcpwm_get_feedback(fb);
        // In a real system, this is where you'd read:
        //   - ADC for DC bus voltage
        //   - Current transformer via ADC
        //   - Thermistor / thermocouple for temperature

        vTaskDelayUntil(&last_wake, interval);
    }
}
