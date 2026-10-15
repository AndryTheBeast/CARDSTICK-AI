/**
 * @file hal_tdeck.cpp
 * @brief HAL for LilyGO T-Deck Plus
 */
#include "hal_config.h"
#ifdef CONFIG_BOARD_TDECK
#include "hal_tdeck.h"
#include "common_define.h"
#if HAL_USE_DISPLAY
#include "display/display_tdeck.hpp"
#endif
#include "esp_log.h"
#include "driver/gpio.h"

static const char* TAG = "HAL";

using namespace HAL;

void HalTDeck::_init_power()
{
    ESP_LOGI(TAG, "init power");
    // deselect the radio and the SD card first: both share the display's SPI bus and
    // must not answer while the panel is set up
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << TDECK_RADIO_CS_GPIO) | (1ULL << TDECK_SDCARD_CS_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(TDECK_RADIO_CS_GPIO, 1);
    gpio_set_level(TDECK_SDCARD_CS_GPIO, 1);

    gpio_reset_pin(TDECK_POWER_ON_GPIO);
    gpio_set_direction(TDECK_POWER_ON_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(TDECK_POWER_ON_GPIO, 1);
    // LilyGO waits this long for the keyboard controller to boot
    delay(500);
}

#if HAL_USE_DISPLAY
void HalTDeck::_init_display()
{
    ESP_LOGI(TAG, "init display");

    _display = new LGFX_TDeck;
    _canvas = new LGFX_Sprite(_display);
    // 150 KB at 320x240: kept out of internal RAM, or the WiFi driver is left so little
    // that its frames land in PSRAM, which the radio cannot send from (the access point
    // then fails every join). The bus copies each line to an internal buffer to send it.
    _canvas->setPsram(true);
    _canvas->createSprite(_display->width(), _display->height());
    ESP_LOGI(TAG, "display %dx%d", (int)_display->width(), (int)_display->height());
}
#endif

void HalTDeck::init()
{
    ESP_LOGI(TAG, "HAL init");

    _init_power();
#if HAL_USE_I2C
    ESP_LOGI(TAG, "init i2c");
    _i2c = new I2CMaster();
#endif
#if HAL_USE_DISPLAY
    _init_display();
#endif
#if HAL_USE_KEYBOARD
    ESP_LOGI(TAG, "init keyboard");
    _keyboard = new KEYBOARD::Keyboard(this);
#endif
#if HAL_USE_SPEAKER
    ESP_LOGI(TAG, "init speaker");
    _speaker = new Speaker(this);
#endif
#if HAL_USE_MIC
    // the ES7210 is not set up yet: keep the mic off until it is
    ESP_LOGI(TAG, "init mic (disabled)");
    _mic = new Mic(this);
    auto mic_cfg = _mic->config();
    mic_cfg.pin_data_in = -1;
    _mic->config(mic_cfg);
#endif
#if HAL_USE_BUTTON
    _homeButton = new Button(TDECK_HOME_BUTTON_GPIO);
#endif
#if HAL_USE_WIFI
    ESP_LOGI(TAG, "init wifi");
    _wifi = new WiFi(_settings);
#endif
}

void HalTDeck::reboot()
{
    ESP_LOGW(TAG, "Rebooting...");
#if HAL_USE_WIFI
    wifi()->set_status_callback(nullptr);
#endif
    delay(100);
    esp_restart();
}
#endif
