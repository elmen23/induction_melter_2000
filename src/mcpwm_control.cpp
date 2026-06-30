#include "mcpwm_control.h"
#include "esp_log.h"

static constexpr const char *TAG = "MCPWM";

/* ─── Maximum dead-time in hardware ticks (8-bit register) ─── */
static constexpr uint32_t DEADTIME_TICKS_MAX = 255;

/* ─── MCPWM Handles ─── */
static mcpwm_timer_handle_t  s_timer  = nullptr;
static mcpwm_oper_handle_t   s_oper   = nullptr;
static mcpwm_cmpr_handle_t   s_cmpr   = nullptr;
static mcpwm_gen_handle_t    s_gen_a  = nullptr;
static mcpwm_gen_handle_t    s_gen_b  = nullptr;

/* ─── Current active configuration ─── */
static InductionConfig s_cfg = {
    .enable          = false,
    .frequency_hz    = 40000,
    .duty_percent    = 45.0f,
    .dead_time_red_ns = 200,
    .dead_time_fed_ns = 200,
};

/* ─── Feedback state ─── */
static InductionFeedback s_fb = {};

static inline uint32_t ns_to_ticks(uint32_t ns) {
    uint32_t ticks = ns / NS_PER_TICK;
    return (ticks > DEADTIME_TICKS_MAX) ? DEADTIME_TICKS_MAX : ticks;
}

static uint32_t duty_to_compare(float duty_pct) {
    float frac = duty_pct / PWM_DUTY_MAX;
    return static_cast<uint32_t>(frac * MCPWM_RESOLUTION_HZ / s_cfg.frequency_hz);
}

/* ════════════════════════════════════════════
 *  Initialisation
 * ════════════════════════════════════════════ */

esp_err_t mcpwm_init(void) {
    ESP_LOGI(TAG, "Initialising MCPWM — %u Hz base", MCPWM_RESOLUTION_HZ);

    /* ── Timer: 40 MHz, up-counter ── */
    mcpwm_timer_config_t timer_cfg = {};
    timer_cfg.group_id = 0;
    timer_cfg.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    timer_cfg.resolution_hz = MCPWM_RESOLUTION_HZ;
    timer_cfg.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    timer_cfg.period_ticks = 1;
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_cfg, &s_timer));

    /* ── Operator ── */
    mcpwm_operator_config_t oper_cfg = {};
    oper_cfg.group_id = 0;
    ESP_ERROR_CHECK(mcpwm_new_operator(&oper_cfg, &s_oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_oper, s_timer));

    /* ── Comparator ── */
    mcpwm_comparator_config_t cmpr_cfg = {};
    cmpr_cfg.flags.update_cmp_on_tez = true;
    ESP_ERROR_CHECK(mcpwm_new_comparator(s_oper, &cmpr_cfg, &s_cmpr));

    /* ── Generators ── */
    mcpwm_generator_config_t gen_cfg_a = { .gen_gpio_num = GPIO_PWM_A };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_oper, &gen_cfg_a, &s_gen_a));

    mcpwm_generator_config_t gen_cfg_b = { .gen_gpio_num = GPIO_PWM_B };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_oper, &gen_cfg_b, &s_gen_b));

    /* ── Default actions: A high on zero, low on compare; B inverse ── */
    auto set_action = [](mcpwm_gen_handle_t gen, mcpwm_gen_compare_event_action_t cmp_act) {
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(
            gen, MCPWM_GEN_TIMER_EVENT_ACTION(
                MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY,
                (gen == s_gen_a) ? MCPWM_GEN_ACTION_HIGH : MCPWM_GEN_ACTION_LOW)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(gen, cmp_act));
    };
    set_action(s_gen_a, MCPWM_GEN_COMPARE_EVENT_ACTION(
        MCPWM_TIMER_DIRECTION_UP, s_cmpr, MCPWM_GEN_ACTION_LOW));
    set_action(s_gen_b, MCPWM_GEN_COMPARE_EVENT_ACTION(
        MCPWM_TIMER_DIRECTION_UP, s_cmpr, MCPWM_GEN_ACTION_HIGH));

    /* ── Dead time ── */
    auto apply_dt = [](mcpwm_gen_handle_t gen, uint32_t ns) {
        mcpwm_dead_time_config_t dt = { .posedge_delay_ticks = ns_to_ticks(ns) };
        ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen, gen, &dt));
    };
    apply_dt(s_gen_a, s_cfg.dead_time_red_ns);
    apply_dt(s_gen_b, s_cfg.dead_time_fed_ns);

    /* ── Set initial frequency & duty ── */
    mcpwm_set_frequency(s_cfg.frequency_hz);
    mcpwm_set_duty(s_cfg.duty_percent);

    /* ── Start timer once at init (never stop — reduces jitter) ── */
    ESP_ERROR_CHECK(mcpwm_timer_enable(s_timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP));

    /* ── Master enable GPIO ── */
    gpio_config_t en_gpio = {};
    en_gpio.pin_bit_mask = BIT64(GPIO_ENABLE);
    en_gpio.mode = GPIO_MODE_OUTPUT;
    en_gpio.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&en_gpio);
    gpio_set_level(GPIO_ENABLE, 0);

    ESP_LOGI(TAG, "MCPWM ready — A:GPIO%d B:GPIO%d EN:GPIO%d | %u kHz %.1f%% RED:%u FED:%u",
             GPIO_PWM_A, GPIO_PWM_B, GPIO_ENABLE,
             s_cfg.frequency_hz / 1000, s_cfg.duty_percent,
             s_cfg.dead_time_red_ns, s_cfg.dead_time_fed_ns);
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  Config apply / get
 * ════════════════════════════════════════════ */

esp_err_t mcpwm_apply_config(const InductionConfig &cfg) {
    mcpwm_set_frequency(cfg.frequency_hz);
    mcpwm_set_duty(cfg.duty_percent);
    mcpwm_set_dead_time(cfg.dead_time_red_ns, cfg.dead_time_fed_ns);
    mcpwm_set_enable(cfg.enable);

    s_cfg = cfg;
    ESP_LOGI(TAG, "Config: %ukHz %.1f%% RED:%uns FED:%uns %s",
             cfg.frequency_hz / 1000, cfg.duty_percent,
             cfg.dead_time_red_ns, cfg.dead_time_fed_ns,
             cfg.enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t mcpwm_get_config(InductionConfig &cfg) {
    cfg = s_cfg;
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  Parameter setters
 * ════════════════════════════════════════════ */

esp_err_t mcpwm_set_frequency(uint32_t freq_hz) {
    if (freq_hz < PWM_FREQ_MIN || freq_hz > PWM_FREQ_MAX) {
        ESP_LOGW(TAG, "Frequency %u out of range [%u, %u]", freq_hz, PWM_FREQ_MIN, PWM_FREQ_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(mcpwm_timer_set_period(s_timer, MCPWM_RESOLUTION_HZ / freq_hz));
    s_cfg.frequency_hz = freq_hz;
    return ESP_OK;
}

esp_err_t mcpwm_set_duty(float duty_pct) {
    if (duty_pct < PWM_DUTY_MIN || duty_pct > PWM_DUTY_MAX) {
        ESP_LOGW(TAG, "Duty %.1f out of range [%.0f, %.0f]", duty_pct, PWM_DUTY_MIN, PWM_DUTY_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_cmpr, duty_to_compare(duty_pct)));
    s_cfg.duty_percent = duty_pct;
    return ESP_OK;
}

esp_err_t mcpwm_set_dead_time(uint32_t red_ns, uint32_t fed_ns) {
    if (red_ns > DEADTIME_MAX || fed_ns > DEADTIME_MAX) {
        ESP_LOGW(TAG, "Dead time out of range [0, %u]", DEADTIME_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    mcpwm_dead_time_config_t dt_a = {};
    dt_a.posedge_delay_ticks = ns_to_ticks(red_ns);
    ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(s_gen_a, s_gen_a, &dt_a));

    mcpwm_dead_time_config_t dt_b = {};
    dt_b.posedge_delay_ticks = ns_to_ticks(fed_ns);
    ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(s_gen_b, s_gen_b, &dt_b));

    s_cfg.dead_time_red_ns = red_ns;
    s_cfg.dead_time_fed_ns = fed_ns;
    return ESP_OK;
}

esp_err_t mcpwm_set_enable(bool en) {
    gpio_set_level(GPIO_ENABLE, en ? 1 : 0);
    if (!en) {
        gpio_set_level(GPIO_PWM_A, 0);
        gpio_set_level(GPIO_PWM_B, 0);
    }
    s_cfg.enable = en;
    return ESP_OK;
}

esp_err_t mcpwm_emergency_stop(void) {
    ESP_LOGW(TAG, "*** EMERGENCY STOP ***");
    mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_STOP_EMPTY);
    gpio_set_level(GPIO_ENABLE, 0);
    gpio_set_level(GPIO_PWM_A, 0);
    gpio_set_level(GPIO_PWM_B, 0);
    s_cfg.enable = false;
    s_cfg.duty_percent = 0.0f;
    return ESP_OK;
}

/* ════════════════════════════════════════════
 *  Feedback (simulated)
 * ════════════════════════════════════════════ */

void mcpwm_get_feedback(InductionFeedback &fb) {
    if (s_cfg.enable) {
        float freq_khz = s_cfg.frequency_hz / 1000.0f;
        float pct      = s_cfg.duty_percent / PWM_DUTY_MAX;
        s_fb.power_kw      = 15.0f * pct * (freq_khz / 40.0f);
        s_fb.voltage       = 220.0f + (s_fb.power_kw * 1.2f);
        s_fb.current_a     = (s_fb.power_kw * 1000.0f) / (s_fb.voltage + 1.0f);
        s_fb.temperature_c += (s_fb.power_kw * 0.002f) - 0.05f;
        if (s_fb.temperature_c < 25.0f)  s_fb.temperature_c = 25.0f;
        if (s_fb.temperature_c > 110.0f) s_fb.temperature_c = 110.0f;
    } else {
        s_fb.power_kw   = 0.0f;
        s_fb.voltage    = 0.0f;
        s_fb.current_a  = 0.0f;
        if (s_fb.temperature_c > 25.0f) s_fb.temperature_c -= 0.1f;
    }
    fb = s_fb;
}
