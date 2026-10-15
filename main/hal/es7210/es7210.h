/**
 * @file es7210.h
 * @brief ES7210 microphone ADC driver for the LilyGO T-Deck Plus
 */
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

#define ES7210_I2C_ADDR 0x40
#define ES7210_I2C_TIMEOUT_MS 100

namespace HAL
{
    class Hal;

    /**
     * @brief ES7210 4-channel ADC, as an I2S slave clocked by the ESP's MCLK
     *
     * It runs from an MCLK of 256 x the sample rate, which the mic's I2S port puts out,
     * so start() is called once that clock runs. Set up for 16 kHz, 16-bit I2S.
     */
    class ES7210
    {
    public:
        ES7210(Hal* hal) : _hal(hal) {}
        ~ES7210();

        // finds the chip on the bus; the ADC itself stays powered down
        bool init();
        // full set-up and power-up, with MCLK running
        bool start();
        void stop();

        bool is_initialized() const { return _dev_handle != nullptr; }

    private:
        Hal* _hal;
        i2c_master_dev_handle_t _dev_handle = nullptr;

        bool _write_reg(uint8_t reg, uint8_t val);
        bool _read_reg(uint8_t reg, uint8_t* val);
        bool _update_reg(uint8_t reg, uint8_t mask, uint8_t val);
    };

} // namespace HAL
