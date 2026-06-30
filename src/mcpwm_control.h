#pragma once

#include <cstdint>
#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Pin Definitions ─── */
static constexpr gpio_num_t GPIO_PWM_A      = GPIO_NUM_18;   // High-side / PWM-A output
static constexpr gpio_num_t GPIO_PWM_B      = GPIO_NUM_19;   // Low-side / PWM-B output (complementary)
static constexpr gpio_num_t GPIO_ENABLE     = GPIO_NUM_4;    // Master enable (shutdown all outputs)

/* ─── MCPWM Configuration Limits ─── */
static constexpr uint32_t PWM_FREQ_MIN      = 20000;   // 20 kHz
static constexpr uint32_t PWM_FREQ_MAX      = 100000;  // 100 kHz
static constexpr float    PWM_DUTY_MIN      = 0.0f;    // 0%
static constexpr float    PWM_DUTY_MAX      = 100.0f;  // 100%
static constexpr uint32_t DEADTIME_MAX      = 1000;    // 1000 ns max

/* ─── Hardware resolution ─── */
static constexpr uint32_t MCPWM_RESOLUTION_HZ = 40000000;  // 40 MHz → 25 ns/tick
static constexpr uint32_t NS_PER_TICK         = 25;         // nanoseconds per MCPWM tick

/* ─── System State ─── */
struct InductionConfig {
    bool enable;
    uint32_t frequency_hz;      // 20000–100000
    float duty_percent;         // 0.0–100.0
    uint32_t dead_time_red_ns;  // 0–1000  (rising edge delay)
    uint32_t dead_time_fed_ns;  // 0–1000  (falling edge delay)
};

/* ─── Feedback values ─── */
struct InductionFeedback {
    float power_kw;
    float voltage;
    float current_a;
    float temperature_c;
};

/* ─── Public API ─── */
esp_err_t mcpwm_init(void);
esp_err_t mcpwm_apply_config(const InductionConfig &cfg);
esp_err_t mcpwm_get_config(InductionConfig &cfg);
esp_err_t mcpwm_emergency_stop(void);
esp_err_t mcpwm_set_frequency(uint32_t freq_hz);
esp_err_t mcpwm_set_duty(float duty_pct);
esp_err_t mcpwm_set_dead_time(uint32_t red_ns, uint32_t fed_ns);
esp_err_t mcpwm_set_enable(bool en);
void mcpwm_get_feedback(InductionFeedback &fb);

#ifdef __cplusplus
}
#endif
