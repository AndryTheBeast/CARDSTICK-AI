/**
 * @file keyboard_reader_tdeck.h
 * @brief I2C keyboard and trackball reader for the LilyGO T-Deck Plus
 */
#pragma once
#include "keyboard_reader.h"
#include "driver/i2c_master.h"
#include <deque>

#define TDECK_KEYBOARD_I2C_ADDR 0x55
// measured on a T-Deck Plus: up and left are the other way round from LilyGO's utilities.h
#define TDECK_TRACKBALL_RIGHT_PIN 2
#define TDECK_TRACKBALL_UP_PIN 3
#define TDECK_TRACKBALL_LEFT_PIN 1
#define TDECK_TRACKBALL_DOWN_PIN 15
#define TDECK_TRACKBALL_CLICK_PIN 0
// Shift+mic: the only combination the stock keyboard firmware sends that is not a typed
// character (Alt itself is not reported, Sym+mic sends '0')
#define TDECK_KEY_SHIFT_MIC 0xe0

namespace HAL
{
    class Hal;
}

namespace KEYBOARD
{
    /**
     * @brief Maps the T-Deck keyboard and trackball onto Cardputer key positions
     *
     * The keyboard is an ESP32-C3 that answers a 1 byte read with the ASCII code of
     * the last key, or 0. It reports no releases, so every key becomes a short pulse.
     * The trackball pins toggle once per step of a roll and become arrow pulses. Its
     * click (the BOOT button) is ENTER when short and ESC when held, as the keyboard
     * has no ESC; Shift+mic is TAB, which switches between voice and text.
     */
    class TDeckKeyboardReader : public KeyboardReader
    {
    public:
        TDeckKeyboardReader(HAL::Hal* hal) : _hal(hal) {}
        virtual ~TDeckKeyboardReader() = default;

        void init() override;
        void update() override;

    private:
        static constexpr uint32_t POLL_MS = 10;
        static constexpr uint32_t PULSE_MS = 40;
        static constexpr uint32_t GAP_MS = 20;
        static constexpr uint32_t DEBOUNCE_MS = 20;
        static constexpr uint32_t LONG_PRESS_MS = 500;
        // a fast roll must not leave a long tail of moves after the ball stops
        static constexpr size_t MAX_QUEUED = 4;

        struct Pulse_t
        {
            Point2D_t key;
            bool shift;
            // trackball moves come with Fn: the arrows share their positions with ; . , /
            // and the text editor tells them apart by Fn, as on the Cardputer
            bool fn = false;
        };

        void _read_keyboard();
        void _read_trackball();
        void _queue(const Pulse_t& pulse);
        bool _ascii_to_pulse(uint8_t c, Pulse_t& pulse);

        HAL::Hal* _hal;
        i2c_master_dev_handle_t _dev_handle = nullptr;
        uint32_t _next_poll = 0;

        std::deque<Pulse_t> _queue_keys;
        Pulse_t _current = {{-1, -1}, false, false};
        uint32_t _current_until = 0;
        uint32_t _gap_until = 0;

        bool _click_down = false;
        bool _click_long = false;
        uint32_t _click_since = 0;
        uint32_t _click_ignore_until = 0;
    };

} // namespace KEYBOARD
