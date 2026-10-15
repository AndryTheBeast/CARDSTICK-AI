/**
 * @file keyboard_reader_tdeck.cpp
 * @brief I2C keyboard and trackball reader for the LilyGO T-Deck Plus
 */
#include "hal_config.h"
#ifdef CONFIG_BOARD_TDECK
#include "keyboard_reader_tdeck.h"
#include "keyboard.h"
#include "hal.h"
#include <driver/gpio.h>
#include <string.h>
#include "common_define.h"
#include "esp_log.h"

#define TAG "KB_TDECK"

using namespace KEYBOARD;

// Key positions in the Cardputer key map (see _key_value_map)
static constexpr Point2D_t KEY_POS_UP = {11, 2};
static constexpr Point2D_t KEY_POS_DOWN = {11, 3};
static constexpr Point2D_t KEY_POS_LEFT = {10, 3};
static constexpr Point2D_t KEY_POS_RIGHT = {12, 3};
static constexpr Point2D_t KEY_POS_ENTER = {13, 2};
static constexpr Point2D_t KEY_POS_DEL = {13, 0};
static constexpr Point2D_t KEY_POS_SPACE = {13, 3};
static constexpr Point2D_t KEY_POS_SHIFT = {1, 2};
static constexpr Point2D_t KEY_POS_FN = {0, 2};
static constexpr Point2D_t KEY_POS_ESC = {0, 0};
static constexpr Point2D_t KEY_POS_TAB = {0, 1};

// Steps counted by the trackball interrupts, taken by update()
enum
{
    BALL_UP,
    BALL_DOWN,
    BALL_LEFT,
    BALL_RIGHT,
    BALL_COUNT
};
static volatile uint16_t s_ball_steps[BALL_COUNT];

static void IRAM_ATTR trackball_isr(void* arg) { s_ball_steps[(int)arg]++; }

void TDeckKeyboardReader::init()
{
    if (_hal && _hal->i2c() && _hal->i2c()->is_initialized())
    {
        if (_hal->i2c()->add_device(TDECK_KEYBOARD_I2C_ADDR, 100000, nullptr, &_dev_handle) != ESP_OK)
        {
            ESP_LOGE(TAG, "keyboard not added to the I2C bus");
            _dev_handle = nullptr;
        }
    }

    static const struct
    {
        int pin;
        int dir;
    } ball_pins[] = {{TDECK_TRACKBALL_UP_PIN, BALL_UP},
                     {TDECK_TRACKBALL_DOWN_PIN, BALL_DOWN},
                     {TDECK_TRACKBALL_LEFT_PIN, BALL_LEFT},
                     {TDECK_TRACKBALL_RIGHT_PIN, BALL_RIGHT}};

    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_LOGE(TAG, "gpio isr service: %s", esp_err_to_name(ret));

    for (const auto& ball : ball_pins)
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << ball.pin;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.intr_type = GPIO_INTR_NEGEDGE;
        gpio_config(&io_conf);
        gpio_isr_handler_add((gpio_num_t)ball.pin, trackball_isr, (void*)ball.dir);
    }

    gpio_config_t click_conf = {};
    click_conf.pin_bit_mask = 1ULL << TDECK_TRACKBALL_CLICK_PIN;
    click_conf.mode = GPIO_MODE_INPUT;
    click_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&click_conf);

    memset((void*)s_ball_steps, 0, sizeof(s_ball_steps));
    ESP_LOGI(TAG, "keyboard %s, trackball ready", _dev_handle ? "on I2C 0x55" : "missing");
}

bool TDeckKeyboardReader::_ascii_to_pulse(uint8_t c, Pulse_t& pulse)
{
    pulse.shift = false;
    switch (c)
    {
    case '\r':
    case '\n':
        pulse.key = KEY_POS_ENTER;
        return true;
    case 0x08:
    case 0x7f:
        pulse.key = KEY_POS_DEL;
        return true;
    case ' ':
        pulse.key = KEY_POS_SPACE;
        return true;
    case TDECK_KEY_ALT_MIC:
        // the stock keyboard firmware reports no Alt, but Alt+mic has a code of its own
        pulse.key = KEY_POS_TAB;
        return true;
    default:
        break;
    }
    if (c < 0x21 || c > 0x7e)
        return false;

    const char s[2] = {(char)c, 0};
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 14; x++)
        {
            const auto& kv = _key_value_map[y][x];
            if (kv.key_type != KeyType::REGULAR)
                continue;
            if (strcmp(kv.value_first, s) == 0)
            {
                pulse.key = {x, y};
                return true;
            }
            if (strcmp(kv.value_second, s) == 0)
            {
                pulse.key = {x, y};
                pulse.shift = true;
                return true;
            }
        }
    }
    return false;
}

void TDeckKeyboardReader::_queue(const Pulse_t& pulse)
{
    if (_queue_keys.size() < MAX_QUEUED)
        _queue_keys.push_back(pulse);
}

void TDeckKeyboardReader::_read_keyboard()
{
    if (!_dev_handle)
        return;
    uint8_t c = 0;
    if (i2c_master_receive(_dev_handle, &c, 1, 20) != ESP_OK || c == 0)
        return;

    Pulse_t pulse;
    if (_ascii_to_pulse(c, pulse))
        _queue(pulse);
    else
        ESP_LOGI(TAG, "unmapped key 0x%02x", c);
}

void TDeckKeyboardReader::_read_trackball()
{
    static const Point2D_t dir_keys[BALL_COUNT] = {KEY_POS_UP, KEY_POS_DOWN, KEY_POS_LEFT, KEY_POS_RIGHT};
    for (int dir = 0; dir < BALL_COUNT; dir++)
    {
        uint16_t steps = s_ball_steps[dir];
        if (!steps)
            continue;
        s_ball_steps[dir] = 0;
        while (steps--)
            _queue({dir_keys[dir], false, true});
    }
}

void TDeckKeyboardReader::update()
{
    _key_list.clear();
    const uint32_t now = millis();

    if (now >= _next_poll)
    {
        _next_poll = now + POLL_MS;
        _read_keyboard();
        _read_trackball();
    }

    // one pulse at a time, with a gap so a repeated key is seen as a new press
    if (_current.key.x >= 0 && now >= _current_until)
    {
        _current.key = {-1, -1};
        _gap_until = now + GAP_MS;
    }
    if (_current.key.x < 0 && now >= _gap_until && !_queue_keys.empty())
    {
        _current = _queue_keys.front();
        _queue_keys.pop_front();
        _current_until = now + PULSE_MS;
    }
    if (_current.key.x >= 0)
    {
        if (_current.fn)
            _key_list.push_back(KEY_POS_FN);
        if (_current.shift)
            _key_list.push_back(KEY_POS_SHIFT);
        _key_list.push_back(_current.key);
    }

    // short click = ENTER, sent on release; held = ESC for as long as it is held
    const bool click = gpio_get_level((gpio_num_t)TDECK_TRACKBALL_CLICK_PIN) == 0;
    if (click != _click_down && now >= _click_ignore_until)
    {
        _click_down = click;
        _click_ignore_until = now + DEBOUNCE_MS;
        if (click)
        {
            _click_since = now;
            _click_long = false;
        }
        else if (!_click_long)
            _queue({KEY_POS_ENTER, false});
    }
    if (_click_down && (now - _click_since) >= LONG_PRESS_MS)
    {
        _click_long = true;
        _key_list.push_back(KEY_POS_ESC);
    }
}
#endif
