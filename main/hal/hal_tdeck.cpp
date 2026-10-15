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
#include "driver/rtc_io.h"
#include "esp_sleep.h"

static const char* TAG = "HAL";

using namespace HAL;

void HalTDeck::_init_power()
{
    ESP_LOGI(TAG, "init power");
    // held low through deep sleep, see powerOff()
    gpio_hold_dis(TDECK_POWER_ON_GPIO);
    gpio_hold_dis(TDECK_BACKLIGHT_GPIO);
    gpio_deep_sleep_hold_dis();
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
    ESP_LOGI(TAG, "init mic");
    _es7210 = new ES7210(this);
    _es7210->init();
    _mic = new Mic(this);
    auto mic_cfg = _mic->config();
    mic_cfg.pin_bck = 47;
    mic_cfg.pin_ws = 21;
    mic_cfg.pin_data_in = _es7210->is_initialized() ? 14 : -1;
    mic_cfg.over_sampling = 1;
    // measured: the microphone is on the ES7210's MIC1, the left slot; the right one
    // (MIC2) carries nothing
    mic_cfg.stereo = false;
    mic_cfg.left_channel = true;
    // x3 on top of the ES7210's 30 dB: speech peaks at about 2000-8000 before it
    mic_cfg.magnification = 6;
    _mic->config(mic_cfg);
#endif
#if HAL_USE_BUTTON
    _homeButton = new Button(TDECK_HOME_BUTTON_GPIO);
#endif
#if HAL_USE_SDCARD
    // mounted on demand; its CS was set high in _init_power
    ESP_LOGI(TAG, "init sdcard");
    _sdcard = new SDCard;
#endif
#if HAL_USE_BAT
    ESP_LOGI(TAG, "init battery");
    _battery = new Battery();
#endif
#if HAL_USE_WIFI
    ESP_LOGI(TAG, "init wifi");
    _wifi = new WiFi(_settings);
#endif
}

#if HAL_USE_BAT
uint8_t HalTDeck::getBatLevel(float voltage) { return Battery::get_level(voltage); }

// Averaged over a burst of samples, then smoothed across calls, as on the Cardputer
float HalTDeck::getBatVoltage()
{
    static constexpr int SAMPLES = 16;
    static constexpr float SMOOTHING = 0.3f;

    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < SAMPLES; i++)
    {
        const float voltage = _battery->get_voltage();
        if (voltage > 1.0f)
        {
            sum += voltage;
            count++;
        }
    }
    if (count == 0)
        return 0.0f;

    static float filtered = 0.0f;
    const float voltage = sum / count;
    // a jump of more than 0.1 V is a real change (charger plugged), not noise
    if (filtered < 1.0f || voltage > filtered + 0.1f || voltage < filtered - 0.1f)
        filtered = voltage;
    else
        filtered += (voltage - filtered) * SMOOTHING;
    return filtered;
}
#endif

// There is no power chip and no software power-off: "off" is deep sleep, left with a
// click of the trackball, which restarts the device.
void HalTDeck::powerOff()
{
    ESP_LOGW(TAG, "Deep sleep...");
#if HAL_USE_WIFI
    wifi()->set_status_callback(nullptr);
    wifi()->disconnect();
#endif
#if HAL_USE_MIC
    _mic->end();
#endif
#if HAL_USE_SPEAKER
    _speaker->end();
#endif
#if HAL_USE_DISPLAY
    _display->setBrightness(0);
    _display->sleep();
#endif
    delay(100);
    // switch the keyboard, display, audio and radio off for the sleep, and keep the
    // backlight dark: both pins float in deep sleep otherwise
    gpio_set_level(TDECK_BACKLIGHT_GPIO, 0);
    gpio_set_level(TDECK_POWER_ON_GPIO, 0);
    gpio_hold_en(TDECK_BACKLIGHT_GPIO);
    gpio_hold_en(TDECK_POWER_ON_GPIO);
    gpio_deep_sleep_hold_en();

    // a click still held would wake the device straight away
    while (gpio_get_level(TDECK_WAKEUP_GPIO) == 0)
        delay(10);
    rtc_gpio_pullup_en(TDECK_WAKEUP_GPIO);
    rtc_gpio_pulldown_dis(TDECK_WAKEUP_GPIO);
    esp_sleep_enable_ext0_wakeup(TDECK_WAKEUP_GPIO, 0);
    esp_deep_sleep_start();
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
