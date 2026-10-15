/**
 * @file led_tdeck.cpp
 * @brief The LED class on the T-Deck: its keyboard backlight as a one-colour status light
 *
 * The T-Deck has no RGB LED. Its keyboard controller (ESP32-C3, I2C 0x55) takes 0x01
 * and a level from 0 to 255 for the white keyboard backlight. A colour becomes the
 * level of its brightest channel, and the effects run on a timer here, so the app
 * drives it exactly as the Cardputer's LED.
 */
#include "hal_config.h"
#ifdef CONFIG_BOARD_TDECK
#include "led.h"
#include "i2c/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <cmath>

static const char* TAG = "LED_TDECK";

#define KEYBOARD_I2C_ADDR 0x55
#define KEYBOARD_CMD_BACKLIGHT 0x01
#define TICK_US 20000

namespace
{
    enum class Mode
    {
        OFF,
        CONSTANT,
        ONCE,
        FLASH,
        BLINK,
        DOUBLE_BLINK,
        FADE,
        BREATHE
    };

    // one LED per device: the state lives here rather than in the class
    struct State
    {
        Mode mode = Mode::OFF;
        uint8_t level = 0;
        uint8_t then_level = 0;
        int64_t start_us = 0;
        uint32_t a_ms = 0; // on / duration / blink / fade in / period
        uint32_t b_ms = 0; // off / gap / fade out
        uint32_t c_ms = 0; // period / hold
    };

    State s_state;
    portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
    i2c_master_dev_handle_t s_dev = nullptr;
    esp_timer_handle_t s_timer = nullptr;
    bool s_suspended = false;
    int s_written = -1;

    uint8_t to_level(const HAL::Color& color) { return std::max(color.r, std::max(color.g, color.b)); }

    void write_level(uint8_t level)
    {
        if (!s_dev || s_suspended || level == s_written)
            return;
        const uint8_t buf[2] = {KEYBOARD_CMD_BACKLIGHT, level};
        if (i2c_master_transmit(s_dev, buf, sizeof(buf), 20) == ESP_OK)
            s_written = level;
    }

    // the level the current effect wants now; may end a one-off effect
    uint8_t level_now(State& st, int64_t now_us)
    {
        const uint32_t t = (uint32_t)((now_us - st.start_us) / 1000);
        switch (st.mode)
        {
        case Mode::OFF:
            return 0;
        case Mode::CONSTANT:
            return st.level;
        case Mode::ONCE:
            if (t < st.a_ms)
                return st.level;
            st.mode = Mode::OFF;
            return 0;
        case Mode::FLASH:
            if (t < st.a_ms)
                return st.level;
            st.mode = Mode::CONSTANT;
            st.level = st.then_level;
            return st.level;
        case Mode::BLINK:
        {
            const uint32_t cycle = std::max<uint32_t>(1, st.a_ms + st.b_ms);
            return (t % cycle) < st.a_ms ? st.level : 0;
        }
        case Mode::DOUBLE_BLINK:
        {
            const uint32_t p = t % std::max<uint32_t>(1, st.c_ms);
            const bool on = p < st.a_ms || (p >= st.a_ms + st.b_ms && p < 2 * st.a_ms + st.b_ms);
            return on ? st.level : 0;
        }
        case Mode::FADE:
            if (t < st.a_ms)
                return (uint8_t)(st.level * t / std::max<uint32_t>(1, st.a_ms));
            if (t < st.a_ms + st.c_ms)
                return st.level;
            if (t < st.a_ms + st.c_ms + st.b_ms)
                return (uint8_t)(st.level * (st.a_ms + st.c_ms + st.b_ms - t) / std::max<uint32_t>(1, st.b_ms));
            st.mode = Mode::OFF;
            return 0;
        case Mode::BREATHE:
        {
            const uint32_t period = std::max<uint32_t>(1, st.a_ms);
            const float phase = (float)(t % period) / period;
            return (uint8_t)(st.level * (1.0f - cosf(2.0f * (float)M_PI * phase)) / 2.0f);
        }
        }
        return 0;
    }

    void tick(void*)
    {
        portENTER_CRITICAL(&s_lock);
        const uint8_t level = level_now(s_state, esp_timer_get_time());
        portEXIT_CRITICAL(&s_lock);
        write_level(level);
    }

    void apply(const State& st)
    {
        portENTER_CRITICAL(&s_lock);
        s_state = st;
        s_state.start_us = esp_timer_get_time();
        const uint8_t level = level_now(s_state, s_state.start_us);
        portEXIT_CRITICAL(&s_lock);
        write_level(level);
    }
} // namespace

namespace HAL
{
    // the RMT members of the class are not used here; its pins mean nothing on the T-Deck
    LED::LED(int gpio_num, int power_pwm_gpio, uint32_t power_pwm_hz)
    {
        _initialized = false;
        init();
    }

    LED::~LED() { deinit(); }

    bool LED::init()
    {
        if (_initialized)
            return true;
        i2c_master_bus_handle_t bus = nullptr;
        if (i2c_master_get_bus_handle((i2c_port_num_t)I2CConfig::BUS_NUM, &bus) != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C bus not up");
            return false;
        }
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = KEYBOARD_I2C_ADDR;
        dev_cfg.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK)
        {
            s_dev = nullptr;
            ESP_LOGE(TAG, "keyboard not added to the I2C bus");
            return false;
        }
        esp_timer_create_args_t args = {};
        args.callback = tick;
        args.name = "kbd_light";
        if (esp_timer_create(&args, &s_timer) != ESP_OK || esp_timer_start_periodic(s_timer, TICK_US) != ESP_OK)
        {
            ESP_LOGE(TAG, "timer not started");
            return false;
        }
        _initialized = true;
        ESP_LOGI(TAG, "keyboard backlight as status light");
        return true;
    }

    bool LED::deinit()
    {
        if (s_timer)
        {
            esp_timer_stop(s_timer);
            esp_timer_delete(s_timer);
            s_timer = nullptr;
        }
        apply(State{});
        if (s_dev)
        {
            i2c_master_bus_rm_device(s_dev);
            s_dev = nullptr;
        }
        _initialized = false;
        return true;
    }

    bool LED::set_color(const Color& color)
    {
        State st;
        st.mode = Mode::CONSTANT;
        st.level = to_level(color);
        apply(st);
        return true;
    }

    bool LED::off()
    {
        apply(State{});
        return true;
    }

    bool LED::blink_once(const Color& color, uint32_t duration_ms)
    {
        State st;
        st.mode = Mode::ONCE;
        st.level = to_level(color);
        st.a_ms = duration_ms;
        apply(st);
        return true;
    }

    bool LED::flash(const Color& color, uint32_t duration_ms, const Color& then)
    {
        State st;
        st.mode = Mode::FLASH;
        st.level = to_level(color);
        st.then_level = to_level(then);
        st.a_ms = duration_ms;
        apply(st);
        return true;
    }

    bool LED::blink_periodic(const Color& color, uint32_t on_ms, uint32_t off_ms)
    {
        State st;
        st.mode = Mode::BLINK;
        st.level = to_level(color);
        st.a_ms = on_ms;
        st.b_ms = off_ms;
        apply(st);
        return true;
    }

    bool LED::blink_periodic_double(const Color& color, uint32_t blink_ms, uint32_t gap_ms, uint32_t period_ms)
    {
        State st;
        st.mode = Mode::DOUBLE_BLINK;
        st.level = to_level(color);
        st.a_ms = blink_ms;
        st.b_ms = gap_ms;
        st.c_ms = period_ms;
        apply(st);
        return true;
    }

    bool LED::fade(const Color& color, uint32_t fade_in_ms, uint32_t fade_out_ms, uint32_t hold_ms)
    {
        State st;
        st.mode = Mode::FADE;
        st.level = to_level(color);
        st.a_ms = fade_in_ms;
        st.b_ms = fade_out_ms;
        st.c_ms = hold_ms;
        apply(st);
        return true;
    }

    bool LED::breathe(const Color& color, uint32_t period_ms)
    {
        State st;
        st.mode = Mode::BREATHE;
        st.level = to_level(color);
        st.a_ms = period_ms;
        apply(st);
        return true;
    }

    bool LED::stop() { return off(); }

    void LED::suspend()
    {
        write_level(0);
        s_suspended = true;
    }

    void LED::resume()
    {
        s_suspended = false;
        s_written = -1;
    }

} // namespace HAL
#endif
