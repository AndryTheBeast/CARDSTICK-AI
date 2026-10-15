#pragma once

#define LGFX_USE_V1

#include <LovyanGFX.hpp>
#include "Panel_ST7789V2.hpp"
#include "Bus_IDFSPI.hpp"
#include <driver/gpio.h>
#include <rom/ets_sys.h>
#include <freertos/FreeRTOS.h>

namespace lgfx
{
    inline namespace v1
    {
        // T-Deck backlight: an AW9364 LED driver on the backlight pin. It has 16 levels
        // and steps down one level per short low pulse; held low for over 2.5 ms it
        // switches off. PWM would read as random steps and flicker, so pulse it instead.
        struct Light_AW9364 : public ILight
        {
            static constexpr uint8_t STEPS = 16;

            explicit Light_AW9364(int pin) : _pin((gpio_num_t)pin) {}

            bool init(uint8_t brightness) override
            {
                gpio_reset_pin(_pin);
                gpio_set_direction(_pin, GPIO_MODE_OUTPUT);
                gpio_set_level(_pin, 0);
                // the driver needs the off time to reset its level counter
                ets_delay_us(3000);
                _level = 0;
                setBrightness(brightness);
                return true;
            }

            void setBrightness(uint8_t brightness) override
            {
                // 0-255 to 0-16, any non-zero value at least the lowest level
                const uint8_t level = brightness ? (uint8_t)(1 + (brightness * (STEPS - 1)) / 255) : 0;
                if (level == _level)
                    return;
                if (level == 0)
                {
                    gpio_set_level(_pin, 0);
                    ets_delay_us(3000);
                    _level = 0;
                    return;
                }
                if (_level == 0)
                {
                    // switching on starts at the brightest level
                    gpio_set_level(_pin, 1);
                    ets_delay_us(30);
                    _level = STEPS;
                }
                const int pulses = (STEPS + _level - level) % STEPS;
                for (int i = 0; i < pulses; i++)
                {
                    // a pulse stretched past 500 us by a task switch would not count
                    portENTER_CRITICAL(&_mux);
                    gpio_set_level(_pin, 0);
                    ets_delay_us(1);
                    gpio_set_level(_pin, 1);
                    portEXIT_CRITICAL(&_mux);
                    ets_delay_us(1);
                }
                _level = level;
            }

        private:
            gpio_num_t _pin;
            uint8_t _level = 0;
            portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
        };
    } // namespace v1
} // namespace lgfx

// LilyGO T-Deck Plus: ST7789 240x320, used in landscape at 320x240. The SD card and the
// LoRa radio share this SPI bus, so the bus carries MISO and the panel is bus_shared.
// GPIO10 (peripheral power) must be high and the radio's CS held high before this is
// constructed.
class LGFX_TDeck : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789V2 _panel_instance;
    lgfx::Bus_IDFSPI _bus_instance;
    lgfx::Light_AW9364 _light_instance{42};

public:
    LGFX_TDeck(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.freq_write = 40 * 1000 * 1000;
            cfg.freq_read = 4 * 1000 * 1000;
            cfg.pin_dc = 11;
            cfg.pin_cs = 12;
            cfg.pin_sclk = 40;
            cfg.pin_io0 = 41;
            cfg.pin_io1 = 38;
            cfg.spi_3wire = false;
            cfg.pin_io2 = -1;
            cfg.pin_io3 = -1;
            cfg.spi_mode = 0;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();

            cfg.pin_rst = -1;
            cfg.panel_height = 320;
            cfg.panel_width = 240;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.bus_shared = true;

            _panel_instance.config(cfg);
        }
        _panel_instance.setLight(&_light_instance);

        setPanel(&_panel_instance);
        // initialize the display
        init();
        setBrightness(0);
        setRotation(1);
    }
};
