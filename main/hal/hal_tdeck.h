/**
 * @file hal_tdeck.h
 * @brief HAL for LilyGO T-Deck Plus (ESP32-S3FN16R8, I2C keyboard, trackball, ES7210, MAX98357A)
 */
#pragma once
#include "hal.h"
#include "common_define.h"
#include "es7210/es7210.h"

// ES7210 master clock, from the mic's I2S port
#define TDECK_MIC_MCLK_GPIO 48

// drives the supply of the keyboard, display, audio and radio
#define TDECK_POWER_ON_GPIO GPIO_NUM_10
// chip selects on the display's SPI bus, held high while their device is unused
#define TDECK_RADIO_CS_GPIO GPIO_NUM_9
#define TDECK_SDCARD_CS_GPIO GPIO_NUM_39
// The app reads a home button as "cancel", ahead of the keys. The T-Deck has none (GPIO0
// is the trackball click, which is ENTER), so it is read from the radio's reset line:
// pulled up inside the SX1262 and never driven low here.
#define TDECK_HOME_BUTTON_GPIO 17

#if HAL_USE_SPEAKER
extern const uint8_t error_wav_start[] asm("_binary_error_wav_start");
extern const uint8_t error_wav_end[] asm("_binary_error_wav_end");
#endif

namespace HAL
{
    class HalTDeck : public Hal
    {
    private:
        ES7210* _es7210 = nullptr;

        void _init_power();
#if HAL_USE_DISPLAY
        void _init_display();
#endif

    public:
        HalTDeck(SETTINGS::Settings* settings) : Hal(settings) { _board_type = BoardType::TDECK; }

        std::string type() override { return "T-Deck"; }
        void init() override;

        inline ES7210* es7210() { return _es7210; }

#if HAL_USE_SPEAKER
        void playErrorSound() override { _speaker->playWav(error_wav_start, error_wav_end - error_wav_start); }
        void playKeyboardSound() override { _speaker->tone(5000, 20); }
        void playLastSound() override { _speaker->tone(6000, 20); }
        void playNextSound() override { _speaker->tone(7000, 20); }
        void playDeviceConnectedSound() override
        {
            _speaker->tone(1000, 100);
            delay(50);
            _speaker->tone(1500, 200);
        }
        void playDeviceDisconnectedSound() override
        {
            _speaker->tone(1500, 100);
            delay(50);
            _speaker->tone(1000, 200);
        }
#endif

        void reboot() override;
    };
} // namespace HAL
