/**
 * @file es7210.cpp
 * @brief ES7210 microphone ADC driver for the LilyGO T-Deck Plus
 *
 * Register values follow Espressif's esp_codec_dev ES7210 driver.
 */
#include "hal_config.h"
#ifdef CONFIG_BOARD_TDECK
#include "es7210.h"
#include "hal.h"
#include "esp_log.h"

static const char* TAG = "ES7210";

// registers
#define REG_RESET 0x00
#define REG_CLOCK_OFF 0x01
#define REG_MAINCLK 0x02
#define REG_LRCK_DIV_H 0x04
#define REG_LRCK_DIV_L 0x05
#define REG_POWER_DOWN 0x06
#define REG_OSR 0x07
#define REG_MODE_CONFIG 0x08
#define REG_TIME_CONTROL0 0x09
#define REG_TIME_CONTROL1 0x0A
#define REG_SDP_INTERFACE1 0x11
#define REG_SDP_INTERFACE2 0x12
#define REG_ADC34_HPF2 0x20
#define REG_ADC34_HPF1 0x21
#define REG_ADC12_HPF1 0x22
#define REG_ADC12_HPF2 0x23
#define REG_ANALOG 0x40
#define REG_MIC12_BIAS 0x41
#define REG_MIC34_BIAS 0x42
#define REG_MIC1_GAIN 0x43
#define REG_MIC1_POWER 0x47
#define REG_MIC12_POWER 0x4B
#define REG_MIC34_POWER 0x4C

// gain in 3 dB steps from 0 dB (0x0A = 30 dB, up to 0x0E = 37.5 dB); bit 4 enables it
static constexpr uint8_t MIC_GAIN = 0x0A;

namespace HAL
{
    ES7210::~ES7210()
    {
        if (_dev_handle && _hal && _hal->i2c())
            _hal->i2c()->remove_device(_dev_handle);
    }

    bool ES7210::_write_reg(uint8_t reg, uint8_t val)
    {
        const uint8_t buf[2] = {reg, val};
        return i2c_master_transmit(_dev_handle, buf, 2, ES7210_I2C_TIMEOUT_MS) == ESP_OK;
    }

    bool ES7210::_read_reg(uint8_t reg, uint8_t* val)
    {
        return i2c_master_transmit_receive(_dev_handle, &reg, 1, val, 1, ES7210_I2C_TIMEOUT_MS) == ESP_OK;
    }

    bool ES7210::_update_reg(uint8_t reg, uint8_t mask, uint8_t val)
    {
        uint8_t old = 0;
        if (!_read_reg(reg, &old))
            return false;
        return _write_reg(reg, (old & ~mask) | (val & mask));
    }

    bool ES7210::init()
    {
        if (!_hal || !_hal->i2c() || !_hal->i2c()->is_initialized())
            return false;
        if (_hal->i2c()->probe(ES7210_I2C_ADDR) != ESP_OK)
        {
            ESP_LOGE(TAG, "not found at 0x%02x", ES7210_I2C_ADDR);
            return false;
        }
        if (_hal->i2c()->add_device(ES7210_I2C_ADDR, 400000, nullptr, &_dev_handle) != ESP_OK)
        {
            _dev_handle = nullptr;
            return false;
        }
        ESP_LOGI(TAG, "found at 0x%02x", ES7210_I2C_ADDR);
        return true;
    }

    bool ES7210::start()
    {
        if (!_dev_handle)
            return false;

        bool ok = true;
        ok &= _write_reg(REG_RESET, 0xFF);
        ok &= _write_reg(REG_RESET, 0x41);
        ok &= _write_reg(REG_CLOCK_OFF, 0x3F);
        ok &= _write_reg(REG_TIME_CONTROL0, 0x30);
        ok &= _write_reg(REG_TIME_CONTROL1, 0x30);
        ok &= _write_reg(REG_ADC12_HPF2, 0x2A);
        ok &= _write_reg(REG_ADC12_HPF1, 0x0A);
        ok &= _write_reg(REG_ADC34_HPF2, 0x0A);
        ok &= _write_reg(REG_ADC34_HPF1, 0x2A);
        // I2S slave: the ESP drives BCLK and LRCK
        ok &= _update_reg(REG_MODE_CONFIG, 0x01, 0x00);
        ok &= _write_reg(REG_ANALOG, 0x43);
        // microphone bias 2.87 V
        ok &= _write_reg(REG_MIC12_BIAS, 0x70);
        ok &= _write_reg(REG_MIC34_BIAS, 0x70);

        // 16 kHz from a 4.096 MHz MCLK: ADC divider 1 with DLL and doubler, OSR 32,
        // LRCK = MCLK / 256
        ok &= _write_reg(REG_OSR, 0x20);
        ok &= _write_reg(REG_MAINCLK, 0xC1);
        ok &= _write_reg(REG_LRCK_DIV_H, 0x01);
        ok &= _write_reg(REG_LRCK_DIV_L, 0x00);

        // 16-bit I2S; two mics per data line (no TDM)
        ok &= _write_reg(REG_SDP_INTERFACE1, 0x60);
        ok &= _write_reg(REG_SDP_INTERFACE2, 0x00);

        // power up MIC1 and MIC2, which share the data line the ESP reads; the T-Deck's
        // microphone is on MIC1. MIC3 and MIC4 stay powered down; all clocks run, as
        // tested on the device.
        ok &= _write_reg(REG_CLOCK_OFF, 0x00);
        ok &= _write_reg(REG_POWER_DOWN, 0x00);
        for (uint8_t i = 0; i < 2; i++)
        {
            ok &= _write_reg(REG_MIC1_POWER + i, 0x08);
            ok &= _write_reg(REG_MIC1_GAIN + i, 0x10 | MIC_GAIN);
        }
        ok &= _write_reg(REG_MIC12_POWER, 0x00);
        ok &= _write_reg(REG_MIC34_POWER, 0xFF);
        ok &= _write_reg(REG_ANALOG, 0x43);

        if (!ok)
            ESP_LOGE(TAG, "set-up failed");
        return ok;
    }

    void ES7210::stop()
    {
        if (!_dev_handle)
            return;
        _write_reg(REG_MIC12_POWER, 0xFF);
        _write_reg(REG_MIC34_POWER, 0xFF);
        _write_reg(REG_ANALOG, 0xC0);
        _write_reg(REG_CLOCK_OFF, 0x7F);
        _write_reg(REG_POWER_DOWN, 0x07);
    }

} // namespace HAL
#endif
