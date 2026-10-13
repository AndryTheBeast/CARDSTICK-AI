v3.4

- Cardputer: type to Gemini. TAB on the chat screen switches to typing: the microphone is off, ENTER opens a message box, and the reply is shown as text without being spoken
- Cardputer: Web setup, as on the StickS3. Edit WiFi networks, the API key and every other setting from a phone or PC browser
- Cardputer: Sleep entry in Settings. Deep sleep, G0 wakes it up
- Cardputer ADV: the RGB LED no longer flickers when the screen is dimmed. It still dims with the screen brightness
- The WiFi password and API key are no longer written to the serial log
- No more error lines at boot for settings that were never saved

v3.3

- Cardputer: the RGB LED shows the state. Orange without WiFi, green when connected, purple while listening, yellow while Gemini speaks, blinking red on low battery. On/off, brightness, the low battery level and each colour are in the new LED settings
- On the Cardputer ADV the LED is powered from the screen backlight: it dims with the screen brightness and can still flicker slightly

v3.2

- Gemini answers sooner: your turn now ends after half a second of silence, instead of waiting up to several seconds
- Battery percentage now matches M5Launcher and Bruce on both devices (0% at 3.30 V, 100% at 4.10 V)
- M5StickS3: louder speaker, x6 on battery and x10 on USB power
- New default settings: screen brightness 40%, system volume 30%, S2S volume 60%, shorter rules. The StickS3 no longer gets the Cardputer rules after a full flash
- Firmware is about 320 KB smaller

v3.1

- M5StickS3 support: two-button navigation, Web setup page for WiFi and API key, Power off menu entry
- Up to 8 saved WiFi networks, the strongest one in range is used
- Battery indicator on the main screen
- Fixed words missing from Gemini's replies (audio and text) when a reply chunk was larger than the message buffer
- Fixed build failing when the project path contains a space

v2.5

- Speach to text feature added using Deepgram API
- SETTINGS: fine tuning of STT
- SETTINGS: added separate volume level for system sounds and speach

v2.2

- No RAM for SSL connection fixed

v2.0

- Integrated the ElevenLabs TTS API to provide high-quality and natural-sounding voice output for the AI assistant's responses
- History messages less bright to make focus on current message
- More customizable settings
- Export settings pays respect to other settings in file
- Settings list items now displays current value

v1.0

- Initial release
- Chat with Gemini API
