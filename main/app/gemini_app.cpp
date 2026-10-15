#include "gemini_app.h"
#include "audio_buffer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app/utils/ui/dialog.h"
#include "app/utils/ui/key_labels.h"
#include "app/utils/ui/settings_screen.h"
#include "app/utils/ui/settings_submenu.h"
#include "app/utils/ui/key_repeat.h"
#include "app/utils/theme/theme_define.h"
#include "s2s_client.h"
#include <vector>
#include <climits>
#include "assets/g_fonts.hpp"
#include "assets/gemini_icon.h"
#include "assets/qr_gemini.h"
#include "assets/anm_disconnected.h"
#include "assets/anm_wifi.h"
#include "assets/anm_internet.h"
#include "assets/anm_error.h"
#include "assets/anm_playing3.h"
#include "assets/anm_mic.h"

#include "hal.h"
#include "common_define.h"
#include "wifi/wifi.h"
#if S2S_TEXT_INPUT && HAL_USE_LED
#include "hal_cardputer.h"
#include "esp_random.h"
#define LED_KEY_FLASH_MS 120
static uint32_t led_color_by_name(const std::string& name);
#endif

static const char* TAG = "GEMINI_APP";
static const char* GEMINI_NS = "gemini";
#ifdef CONFIG_BOARD_STICKS3
static const char* HINT_MAIN = "[A] START [B hold] SETTINGS";
static const char* API_KEY_HINT = "[A] [B hold]";
static const char* HOME_TITLE = "STICK AI";
#elif defined(CONFIG_BOARD_TDECK)
static const char* HOME_TITLE = "T-DECK AI";
static const char* HINT_MAIN = "[\u2190][\u2192][\u2193] [ENTER] START " KEY_LABEL_ESC " SETTINGS";
static const char* API_KEY_HINT = "[ENTER] " KEY_LABEL_ESC;
#else
static const char* HOME_TITLE = "CARDPUTER AI";
static const char* HINT_MAIN = "[\u2190][\u2192] [ENTER] START " KEY_LABEL_ESC " SETTINGS";
static const char* API_KEY_HINT = "[ENTER] " KEY_LABEL_ESC;
#endif

static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;
static bool is_rendered = false;
#if 0
static constexpr int ICON_SZ = 50;
static uint16_t s_anim_bg[ICON_SZ * ICON_SZ];
static bool s_anim_bg_valid = false;
#endif
#define HINT_HEIGHT 12
// Chat screen bottom row: battery icon on the left, key hints in the middle, status
// icon on the right, all as tall as the hint text. The hints are centred on the
// screen, so they are nudged right to sit midway between the two icons.
#define CHAT_BATTERY_WIDTH 18
#define CHAT_ICON_SIZE 11
#define CHAT_HINT_X 4

extern const uint8_t snap_wav_start[] asm("_binary_snap_wav_start");
extern const uint8_t snap_wav_end[] asm("_binary_snap_wav_end");

// ═══════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════

GeminiApp::GeminiApp(HAL::Hal* hal) : _hal(hal), _currentScreen(SCREEN_START), _scrollPosition(0)
{
    _hal->canvas()->setFont(FONT_16);
    _hintTextContext = new UTILS::HL_TEXT::HLTextContext_t();
    UTILS::HL_TEXT::hl_text_init(_hintTextContext, _hal->canvas(), 20, 1500);

    _descScrollContext = new UTILS::SCROLL_TEXT::ScrollTextContext_t();
    UTILS::SCROLL_TEXT::scroll_text_init(_descScrollContext, _hal->canvas(), _hal->canvas()->width(), 16, 20, 1000);

    _control_event_group = xEventGroupCreate();
    xEventGroupClearBits(_control_event_group, 0xFFFFFF);

    _sprite = new LGFX_Sprite(_hal->canvas());
    _sprite->createSprite(50, 50);

    _s2s_shared.mutex = xSemaphoreCreateMutex();
    _s2s_active = false;
}

GeminiApp::~GeminiApp()
{
    stopS2S();

    _sprite->deleteSprite();
    delete _sprite;

    if (_control_event_group != nullptr)
    {
        vEventGroupDelete(_control_event_group);
        _control_event_group = nullptr;
    }

    if (_s2s_shared.mutex)
    {
        vSemaphoreDelete(_s2s_shared.mutex);
        _s2s_shared.mutex = nullptr;
    }

    if (_hintTextContext)
    {
        UTILS::HL_TEXT::hl_text_free(_hintTextContext);
        delete _hintTextContext;
        _hintTextContext = nullptr;
    }

    if (_descScrollContext)
    {
        UTILS::SCROLL_TEXT::scroll_text_free(_descScrollContext);
        delete _descScrollContext;
        _descScrollContext = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════
// S2S session management
// ═══════════════════════════════════════════════════════════

void GeminiApp::startS2S()
{
    if (_s2s_active)
        stopS2S();

    std::string api_key = _hal->settings()->getString(GEMINI_NS, "api_key");
    std::string model = _hal->settings()->getString(GEMINI_NS, "model");
    std::string voice = _hal->settings()->getString(GEMINI_NS, "voice");
    std::string rules = _hal->settings()->getString(GEMINI_NS, "rules");
    int32_t volume = _hal->settings()->getNumber(GEMINI_NS, "volume");

    if (api_key.empty())
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "API key not set. Please go to settings", "OK");
        return;
    }

    xEventGroupClearBits(_control_event_group, S2S_FATAL_ERROR_BIT | S2S_ERROR_BIT);
    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        _s2s_shared.input_transcript.clear();
        _s2s_shared.output_transcript.clear();
        _s2s_shared.error_message.clear();
        xSemaphoreGive(_s2s_shared.mutex);
    }
#if S2S_CHAT_MEMORY
    // A session that reconnects is told what was said so far, in case its handle is
    // refused. That is read back from the card: the session is stopped at this point,
    // so there is memory to mount it.
    if (!_historyReady && _chatSaving && _chatId != 0 && CHAT_STORE::mount(_hal))
    {
        std::vector<CHAT_STORE::Turn> turns;
        std::string handle;
        CHAT_STORE::load(_chatId, turns, handle);
        CHAT_STORE::unmount(_hal);
        std::string history = CHAT_STORE::history_message(turns);
        if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            _s2s_shared.history.swap(history);
            xSemaphoreGive(_s2s_shared.mutex);
        }
    }
    _historyReady = false;
#endif

    if (s2s_start(_hal, _control_event_group, &_s2s_shared, api_key, model, voice, rules, volume))
    {
        _s2s_active = true;
#if S2S_TEXT_INPUT
        // a session restarted after an error stays in the mode the user chose
        setTextMode(_textMode);
#endif
        setState(APP_STATE_S2S_CONNECTING);
    }
    else
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "Could not start S2S session", "OK");
        setState(APP_STATE_IDLE);
    }
}

void GeminiApp::stopS2S()
{
    if (!_s2s_active)
        return;
    s2s_stop();
    _s2s_active = false;
}

#if S2S_TEXT_INPUT
void GeminiApp::setTextMode(bool enabled)
{
    _textMode = enabled;
    if (enabled)
        xEventGroupSetBits(_control_event_group, S2S_TEXT_MODE_BIT);
    else
        xEventGroupClearBits(_control_event_group, S2S_TEXT_MODE_BIT);
}

// Ask for a message and send it as the user's turn. There is no transcription coming
// back for typed text, so it is put into the chat here.
void GeminiApp::typeMessage()
{
    std::string text;
#if HAL_USE_LED
    // Flash the LED on each key press, if that is switched on. The message box runs its
    // own loop, so this rides on the key click it asks the HAL for.
    auto* settings = _hal->settings();
    const std::string key_color = settings->getString("led", "k_typing");
    auto* hal = static_cast<HAL::HalCardputer*>(_hal);
    if (settings->getBool("led", "enabled") && key_color != "Off")
    {
        const uint32_t brightness = settings->getNumber("led", "brightness");
        auto scaled = [brightness](uint32_t rgb)
        {
            return HAL::Color(((rgb >> 16) & 0xFF) * brightness / 255, ((rgb >> 8) & 0xFF) * brightness / 255, (rgb & 0xFF) * brightness / 255);
        };
        const HAL::Color resting = scaled(led_color_by_name(settings->getString("led", "c_typing")));
        const uint32_t fixed_rgb = led_color_by_name(key_color);
        hal->on_key_sound = [this, scaled, resting, fixed_rgb, random = (key_color == "Random")]()
        {
            uint32_t rgb = fixed_rgb;
            if (random)
            {
                // a point on the colour wheel: one channel full, one off, one in between
                const uint32_t hue = esp_random() % 1536;
                const uint32_t ramp = hue % 256;
                static const uint8_t ORDER[6][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}, {1, 2, 0}, {0, 2, 1}};
                uint8_t channel[3];
                const uint32_t sector = hue / 256;
                channel[ORDER[sector][0]] = 255;
                channel[ORDER[sector][1]] = (sector % 2) ? 255 - ramp : ramp;
                channel[ORDER[sector][2]] = 0;
                rgb = (channel[0] << 16) | (channel[1] << 8) | channel[2];
            }
            _hal->led()->flash(scaled(rgb), LED_KEY_FLASH_MS, resting);
        };
    }
#endif
    const bool entered = UTILS::UI::show_edit_string_dialog(_hal, "Message", text, false, 200);
    is_rendered = false;
#if HAL_USE_LED
    hal->on_key_sound = nullptr;
    // back to showing the state, whatever the flashes left on the LED
    _ledApplied = 0xFFFFFFFF;
    _ledNextCheck = 0;
#endif

    const size_t first = text.find_first_not_of(' ');
    if (!entered || first == std::string::npos)
        return;
    text = text.substr(first, text.find_last_not_of(' ') - first + 1);

    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return;
    _s2s_shared.pending_text = text;
    xSemaphoreGive(_s2s_shared.mutex);
    xEventGroupSetBits(_control_event_group, S2S_SEND_TEXT_BIT);

    _userPrompt = text;
    _lastHistoryIndex = _chat.size();
    for (const auto& line : splitTextIntoLines(text))
        _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
    trimChat();
#if S2S_CHAT_MEMORY
    rememberTurn(true, text);
#endif
    _partialPrompt = "";
    updateScrollPosition();
}
#endif

#if S2S_CHAT_MEMORY
#define CHAT_SAVE_INTERVAL_MS 1000
static const char* CHAT_NS = "chat";

// Called when a session is about to start from the start screen: a new chat, or one
// picked from those on the SD card, which is put back on the screen. False if the user
// backed out of the list.
bool GeminiApp::chooseChat()
{
    std::string handle;
    std::vector<CHAT_STORE::Turn> turns;
    _unsavedTurns.clear();
    _chatId = 0;
    _chatSaving = false;
    _saveFailures = 0;

    if (_hal->settings()->getBool(CHAT_NS, "remember") && CHAT_STORE::mount(_hal))
    {
        const int id = CHAT_STORE::pick(_hal);
        if (id < 0)
        {
            CHAT_STORE::unmount(_hal);
            return false;
        }
        if (id > 0 && CHAT_STORE::load(id, turns, handle))
            _chatId = id;
        // a session needs the memory: the card is mounted again for each write
        CHAT_STORE::unmount(_hal);
        _chatSaving = true;
    }

    // a new chat must not pick up the session of the one before it, nor its text
    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        _s2s_shared.session_handle = handle;
        _s2s_shared.history = CHAT_STORE::history_message(turns);
        xSemaphoreGive(_s2s_shared.mutex);
    }
    _savedHandle = handle;
    _historyReady = true;

    _chat.clear();
    for (const auto& turn : turns)
    {
        for (const auto& line : splitTextIntoLines(turn.text))
            _chat.push_back({turn.user ? HISTORY_ITEM_TYPE_USER : HISTORY_ITEM_TYPE_MODEL, line});
    }
    trimChat();
    _userPrompt = "";
    _partialPrompt = "";
    // everything shown as past, scrolled to its end
    _lastHistoryIndex = _chat.size();
    _scrollPosition = _chat.size();
    return true;
}

void GeminiApp::rememberTurn(bool user, const std::string& text)
{
    if (_chatSaving)
        _unsavedTurns.push_back({user, text});
}

// Writes what the card does not have yet: the new turns, and the handle that resumes
// the session, which changes as the conversation goes on. Looked at once a second from
// the chat screen, and `now` when the session has stopped.
//
// Not while the microphone is streaming. A voice session then holds a TLS record going
// out and one coming in at once and has nothing to spare: the 3 KB of a mounted card
// made it lose the connection. What a voice chat has to write waits until Gemini
// answers, when nothing is being sent.
void GeminiApp::saveChat(bool now)
{
    if (!_chatSaving)
        return;
    if (!now && _appState == APP_STATE_S2S_LISTENING && !_textMode)
        return;
    const uint32_t ms = millis();
    if (!now && ms < _saveNextCheck)
        return;
    _saveNextCheck = ms + CHAT_SAVE_INTERVAL_MS;

    std::string handle = _savedHandle;
    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        handle = _s2s_shared.session_handle;
        xSemaphoreGive(_s2s_shared.mutex);
    }
    if (_unsavedTurns.empty() && (handle == _savedHandle || _chatId == 0))
        return;

    bool saved = CHAT_STORE::mount(_hal);
    if (saved)
    {
        if (_chatId == 0)
            _chatId = CHAT_STORE::create();
        saved = _chatId != 0;
        size_t written = 0;
        while (saved && written < _unsavedTurns.size())
        {
            saved = CHAT_STORE::append(_chatId, _unsavedTurns[written]);
            if (saved)
                written++;
        }
        _unsavedTurns.erase(_unsavedTurns.begin(), _unsavedTurns.begin() + written);
        if (saved && handle != _savedHandle)
        {
            CHAT_STORE::save_handle(_chatId, handle);
            _savedHandle = handle;
        }
        CHAT_STORE::unmount(_hal);
    }

    if (saved)
    {
        _saveFailures = 0;
    }
    else if (++_saveFailures >= 3)
    {
        // card pulled out or full: the chat goes on, unsaved
        ESP_LOGW(TAG, "Chat is no longer saved");
        _chatSaving = false;
        _unsavedTurns.clear();
    }
}

// Back to the start screen: the last of the chat goes to the card
void GeminiApp::closeChat()
{
    saveChat(true);
    _chatSaving = false;
}
#endif

// ═══════════════════════════════════════════════════════════
// Init
// ═══════════════════════════════════════════════════════════

void GeminiApp::init()
{
    ESP_LOGI(TAG, "Initializing");
    setState(APP_STATE_DISCONNECTED);

    _groups = _hal->settings()->getMetadata();
    _groups[2].callback = [this](SETTINGS::SettingGroup_t& group)
    {
        if (_hal->settings()->getString("gemini", "api_key").empty())
        {
            _currentScreen = SCREEN_QR_GEMINI;
            is_rendered = false;
        }
    };

#if HAL_USE_LED
    initLedMenus();
#endif

#if S2S_CHAT_MEMORY
    // the rows of "Chat settings" that open a list of the chats
    for (auto& group : _groups)
    {
        if (group.nvs_namespace != CHAT_NS)
            continue;
        for (auto& item : group.items)
        {
            if (item.type != SETTINGS::TYPE_CALLBACK)
                continue;
            item.callback = [this](SETTINGS::SettingItem_t& row)
            {
                if (!CHAT_STORE::mount(_hal))
                {
                    UTILS::UI::show_error_dialog(_hal, "Error", "Failed to mount SD card", "OK");
                    return;
                }
                if (row.label == "Forget chats")
                    CHAT_STORE::forget(_hal);
                else
                    CHAT_STORE::view(_hal);
                CHAT_STORE::unmount(_hal);
            };
        }
    }
#endif

    if (_hal->wifi()->init())
    {
        _hal->wifi()->set_status_callback(
            [this](HAL::wifi_status_t status)
            {
                _wifiStatus = status;
                switch (status)
                {
                case HAL::WIFI_STATUS_IDLE:
                case HAL::WIFI_STATUS_DISCONNECTED:
                    setState(APP_STATE_DISCONNECTED);
                    break;
                case HAL::WIFI_STATUS_CONNECTING:
                    setState(APP_STATE_CONNECTING_WIFI);
                    break;
                case HAL::WIFI_STATUS_CONNECTED_WEAK:
                case HAL::WIFI_STATUS_CONNECTED_GOOD:
                case HAL::WIFI_STATUS_CONNECTED_STRONG:
                    setState(APP_STATE_IDLE);
                    break;
                }
            });
        if (_hal->settings()->getBool("wifi", "enabled"))
            _hal->wifi()->connect();
    }

    int brightness = _hal->settings()->getNumber("system", "brightness");
    if (brightness > 0)
        _hal->display()->setBrightness(brightness);

    _hal->setSystemVolume(_hal->settings()->getNumber("system", "volume"));

    if (_hal->settings()->getBool("system", "boot_sound"))
        _hal->speaker()->playWav(snap_wav_start, snap_wav_end - snap_wav_start, 1, SYSTEM_CHANNEL);

    _currentScreen = SCREEN_START;
    is_rendered = false;

    // A restart caused by the supply voltage collapsing leaves no other trace when
    // the device runs from the battery, away from a serial log
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", (int)reset_reason);
    if (reset_reason == ESP_RST_BROWNOUT)
        UTILS::UI::show_error_dialog(_hal, "Low power", "Restarted: the battery could not keep up. Charge it or lower the volume", "OK");
}

// ═══════════════════════════════════════════════════════════
// Hints
// ═══════════════════════════════════════════════════════════

const char* GeminiApp::getHintForState() const
{
#ifdef CONFIG_BOARD_STICKS3
    switch (_appState)
    {
    case APP_STATE_S2S_LISTENING:
        return "[A] SEND [B] SCROLL [B hold] STOP";
    case APP_STATE_S2S_SPEAKING:
        return "[A] SKIP [B] SCROLL [B hold] STOP";
    case APP_STATE_S2S_CONNECTING:
    case APP_STATE_S2S_ERROR:
        return "[B hold] CANCEL";
    default:
        return "[B] [A hold] SCROLL [B hold] BACK";
    }
#endif
    switch (_appState)
    {
    case APP_STATE_S2S_LISTENING:
#if S2S_TEXT_INPUT
        return _textMode ? "[ENTER] TYPE " KEY_LABEL_TAB " VOICE " KEY_LABEL_ESC " STOP"
                         : "[ENTER] SEND " KEY_LABEL_TAB " TYPE " KEY_LABEL_ESC " STOP";
#endif
        return "[ENTER] SEND [\u2191][\u2193] SCROLL " KEY_LABEL_ESC " STOP";
    case APP_STATE_S2S_SPEAKING:
        return "[ENTER] SKIP [\u2191][\u2193] SCROLL " KEY_LABEL_ESC " STOP";
    case APP_STATE_S2S_CONNECTING:
    case APP_STATE_S2S_ERROR:
        return KEY_LABEL_ESC " CANCEL";
    default:
        return "[\u2191][\u2193][\u2190][\u2192] SCROLL " KEY_LABEL_ESC " BACK";
    }
}

// ═══════════════════════════════════════════════════════════
// Main update loop
// ═══════════════════════════════════════════════════════════

void GeminiApp::update()
{
    bool need_update = false;
    EventBits_t bits = xEventGroupGetBits(_control_event_group);
#if HAL_USE_LED
    updateLed();
#endif
    checkAutoOff();

    switch (_currentScreen)
    {
    case SCREEN_START:
#if HAL_USE_BAT
        updateBattery();
#endif
        need_update |= drawMainScreen();
        need_update |= drawAnimation(need_update);
        handleMainScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      HINT_MAIN,
                                                      0,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();
        break;

    case SCREEN_QR_GEMINI:
        need_update |= drawGeminiQRScreen();
        handleApiKeyScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      API_KEY_HINT,
                                                      65,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();
        break;

    case SCREEN_SETTINGS:
        handleSettingsMenu();
        break;

    case SCREEN_CHAT:
        // ── S2S state machine ──
        switch (_appState)
        {
        case APP_STATE_DISCONNECTED:
        case APP_STATE_CONNECTING_WIFI:
        case APP_STATE_IDLE:
            break;

        case APP_STATE_S2S_CONNECTING:
            if (bits & S2S_SETUP_COMPLETE_BIT)
            {
                setState(APP_STATE_S2S_LISTENING);
                is_rendered = false;
            }
            else if (bits & S2S_ERROR_BIT)
            {
                setState(APP_STATE_S2S_ERROR);
                is_rendered = false;
            }
            break;

        case APP_STATE_S2S_LISTENING:
        {
            if (bits & S2S_INPUT_TRANSCRIPT_BIT)
            {
                xEventGroupClearBits(_control_event_group, S2S_INPUT_TRANSCRIPT_BIT);
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _partialPrompt = _s2s_shared.input_transcript;
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                updateScrollPosition();
                is_rendered = false;
            }

            if ((bits & S2S_MODEL_SPEAKING_BIT) && !(bits & S2S_LISTENING_BIT))
            {
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _userPrompt = _s2s_shared.input_transcript;
                    _s2s_shared.input_transcript.clear();
                    // output_transcript is left alone: the first words of the reply may
                    // already be in it by the time this screen notices the model is speaking
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                if (!_userPrompt.empty())
                {
                    _lastHistoryIndex = _chat.size();
                    for (const auto& line : splitTextIntoLines(_userPrompt))
                        _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
                    trimChat();
#if S2S_CHAT_MEMORY
                    rememberTurn(true, _userPrompt);
#endif
                }
                _partialPrompt = "";
                updateScrollPosition();
                setState(APP_STATE_S2S_SPEAKING);
                is_rendered = false;

            }

            if (bits & (S2S_ERROR_BIT | S2S_TASK_STOPPED_BIT))
            {
                setState(APP_STATE_S2S_ERROR);
                is_rendered = false;
            }
            break;
        }

        case APP_STATE_S2S_SPEAKING:
        {
            // Late-arriving input transcript (server often sends after model starts speaking)
            if ((bits & S2S_INPUT_TRANSCRIPT_BIT) && _userPrompt.empty())
            {
                xEventGroupClearBits(_control_event_group, S2S_INPUT_TRANSCRIPT_BIT);
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _userPrompt = _s2s_shared.input_transcript;
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                if (!_userPrompt.empty())
                {
                    _lastHistoryIndex = _chat.size();
                    for (const auto& line : splitTextIntoLines(_userPrompt))
                        _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
                    trimChat();
#if S2S_CHAT_MEMORY
                    rememberTurn(true, _userPrompt);
#endif
                    updateScrollPosition();
                    is_rendered = false;
                }
            }

            if (bits & S2S_OUTPUT_TRANSCRIPT_BIT)
            {
                xEventGroupClearBits(_control_event_group, S2S_OUTPUT_TRANSCRIPT_BIT);
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _partialPrompt = _s2s_shared.output_transcript;
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                updateScrollPosition();
                is_rendered = false;
            }

            if ((bits & S2S_LISTENING_BIT) && !(bits & S2S_MODEL_SPEAKING_BIT))
            {
                std::string model_text;
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    model_text = _s2s_shared.output_transcript;
                    _s2s_shared.output_transcript.clear();
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                if (!model_text.empty())
                {
                    for (const auto& line : splitTextIntoLines(model_text))
                        _chat.push_back({HISTORY_ITEM_TYPE_MODEL, line});
                    trimChat();
#if S2S_CHAT_MEMORY
                    rememberTurn(false, model_text);
#endif
                }
                _partialPrompt = "";
                updateScrollPosition();
                setState(APP_STATE_S2S_LISTENING);
                is_rendered = false;
            }

            if (bits & (S2S_ERROR_BIT | S2S_TASK_STOPPED_BIT))
            {
                setState(APP_STATE_S2S_ERROR);
                is_rendered = false;
            }
            break;
        }

        case APP_STATE_S2S_ERROR:
            if (bits & S2S_TASK_STOPPED_BIT)
            {
                if (bits & S2S_FATAL_ERROR_BIT)
                {
                    std::string err_msg;
                    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        err_msg = _s2s_shared.error_message;
                        _s2s_shared.error_message.clear();
                        xSemaphoreGive(_s2s_shared.mutex);
                    }
                    xEventGroupClearBits(_control_event_group, S2S_FATAL_ERROR_BIT | S2S_ERROR_BIT);
                    stopS2S();
#if S2S_CHAT_MEMORY
                    closeChat();
#endif
                    UTILS::UI::show_error_dialog(_hal, "Error", err_msg.empty() ? "Session failed" : err_msg);
                    _currentScreen = SCREEN_START;
                    setState(APP_STATE_IDLE);
                    is_rendered = false;
                }
                else
                {
                    ESP_LOGW(TAG, "S2S session ended, attempting reconnect");
                    stopS2S();
#if S2S_CHAT_MEMORY
                    saveChat(true);
#endif
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    startS2S();
                    is_rendered = false;
                }
            }
            break;
        }

#if S2S_CHAT_MEMORY
        saveChat();
#endif
#if HAL_USE_BAT
        updateBattery();
#endif
        need_update |= drawResponseScreen();
        need_update |= drawAnimation(need_update, true);
        handleResponseScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      getHintForState(),
                                                      CHAT_HINT_X,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();
        break;
    }
}

// ═══════════════════════════════════════════════════════════
// Main screen
// ═══════════════════════════════════════════════════════════

#if S2S_TEXT_INPUT
static uint32_t icon_color(uint32_t rgb, uint8_t level);

// Start screen cards: "Live" talks, "Text" types. Drawn rather than stored as bitmaps,
// so that the unselected one can be dimmed and neither costs flash.
#define CARD_W 102
#ifdef CONFIG_BOARD_TDECK
// 320x240: near-square cards, and the Settings card below them, centred under the title
#define CARD_H 96
#define CARD_Y 68
// icon and label as one block, centred in the card
#define CARD_ICON_Y 20
#define CARD_LABEL_Y 65
#define SETTINGS_CARD_Y (CARD_Y + CARD_H + 14)
#define SETTINGS_CARD_H 38
#else
#define CARD_H 66
#define CARD_Y 52
#define CARD_ICON_Y 7
#define CARD_LABEL_Y (CARD_H - 21)
#endif
#define CARD_GAP 12

// microphone with sound waves on both sides, centred on cx, 32 px tall from y
static void draw_live_icon(LGFX_Sprite* c, int cx, int y, uint32_t color)
{
    c->fillRoundRect(cx - 6, y, 13, 20, 6, color);
    c->fillArc(cx, y + 12, 11, 10, 0, 180, color);
    c->fillRect(cx - 1, y + 22, 3, 6, color);
    c->fillRoundRect(cx - 7, y + 28, 15, 3, 1, color);
    for (int r = 18; r <= 25; r += 7)
    {
        c->fillArc(cx, y + 11, r + 1, r, 325, 360, color);
        c->fillArc(cx, y + 11, r + 1, r, 0, 35, color);
        c->fillArc(cx, y + 11, r + 1, r, 145, 215, color);
    }
}

// keyboard, centred on cx, in the same 32 px of height
static void draw_text_icon(LGFX_Sprite* c, int cx, int y, uint32_t color)
{
    c->drawRoundRect(cx - 24, y + 3, 49, 27, 4, color);
    c->drawRoundRect(cx - 23, y + 4, 47, 25, 3, color);
    for (int i = 0; i < 6; i++)
        c->fillRect(cx - 18 + i * 6 + (i > 2 ? 1 : 0), y + 8, 4, 4, color);
    for (int i = 0; i < 5; i++)
        c->fillRect(cx - 15 + i * 6 + (i > 2 ? 1 : 0), y + 14, 4, 4, color);
    c->fillRect(cx - 18, y + 20, 4, 4, color);
    c->fillRect(cx - 11, y + 20, 23, 4, color);
    c->fillRect(cx + 15, y + 20, 4, 4, color);
}

static void draw_mode_card(LGFX_Sprite* c, int x, const char* label, bool text_icon, bool selected)
{
    const uint32_t border = selected ? THEME_COLOR_TITLE : (uint32_t)0x3A3A3A;
    const uint32_t fill = selected ? (uint32_t)0x10283C : (uint32_t)0x000000;
    const uint32_t icon = icon_color(text_icon ? 0xFFC000 : 0xFF5A5A, selected ? 255 : 105);
    c->fillRoundRect(x, CARD_Y, CARD_W, CARD_H, 8, fill);
    c->drawRoundRect(x, CARD_Y, CARD_W, CARD_H, 8, border);
    if (selected)
        c->drawRoundRect(x + 1, CARD_Y + 1, CARD_W - 2, CARD_H - 2, 7, border);
    if (text_icon)
        draw_text_icon(c, x + CARD_W / 2, CARD_Y + CARD_ICON_Y, icon);
    else
        draw_live_icon(c, x + CARD_W / 2, CARD_Y + CARD_ICON_Y, icon);
    c->setFont(FONT_16);
    c->setTextColor(selected ? TFT_WHITE : TFT_DARKGREY);
    c->drawCenterString(label, x + CARD_W / 2, CARD_Y + CARD_LABEL_Y);
}

#ifdef CONFIG_BOARD_TDECK
// gear, centred on (cx, cy): eight teeth around a ring
static void draw_settings_icon(LGFX_Sprite* c, int cx, int cy, uint32_t color)
{
    c->fillArc(cx, cy, 8, 4, 0, 360, color);
    for (int a = 0; a < 360; a += 45)
        c->fillArc(cx, cy, 11, 8, a - 12, a + 12, color);
}

// as wide as the two cards together, so the three line up
static void draw_settings_card(LGFX_Sprite* c, int x, bool selected)
{
    const int w = CARD_W * 2 + CARD_GAP;
    const uint32_t border = selected ? THEME_COLOR_TITLE : (uint32_t)0x3A3A3A;
    const uint32_t fill = selected ? (uint32_t)0x10283C : (uint32_t)0x000000;
    c->fillRoundRect(x, SETTINGS_CARD_Y, w, SETTINGS_CARD_H, 8, fill);
    c->drawRoundRect(x, SETTINGS_CARD_Y, w, SETTINGS_CARD_H, 8, border);
    if (selected)
        c->drawRoundRect(x + 1, SETTINGS_CARD_Y + 1, w - 2, SETTINGS_CARD_H - 2, 7, border);

    static const char* label = "Settings";
    c->setFont(FONT_16);
    const int icon_w = 22;
    const int gap = 8;
    const int content_w = icon_w + gap + c->textWidth(label);
    const int left = x + (w - content_w) / 2;
    const int cy = SETTINGS_CARD_Y + SETTINGS_CARD_H / 2;
    draw_settings_icon(c, left + icon_w / 2, cy, icon_color(0x8AB4F8, selected ? 255 : 105));
    c->setTextColor(selected ? TFT_WHITE : TFT_DARKGREY);
    c->setTextDatum(middle_left);
    c->drawString(label, left + icon_w + gap, cy);
    c->setTextDatum(top_left);
}
#endif

bool GeminiApp::drawMainScreen()
{
    if (is_rendered)
        return false;

    _chat.clear();
    _partialPrompt = "";

    auto* canvas = _hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_16);
    canvas->setTextColor(THEME_COLOR_TITLE);
    canvas->drawCenterString(HOME_TITLE, canvas->width() / 2, TEXT_PADDING);
    canvas->setFont(FONT_12);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawCenterString("v" BUILD_NUMBER, canvas->width() / 2, TEXT_PADDING + 20);

    const int left = (canvas->width() - CARD_W * 2 - CARD_GAP) / 2;
#ifdef CONFIG_BOARD_TDECK
    draw_mode_card(canvas, left, "Live", false, !_startOnSettings && !_startInText);
    draw_mode_card(canvas, left + CARD_W + CARD_GAP, "Text", true, !_startOnSettings && _startInText);
    draw_settings_card(canvas, left, _startOnSettings);
#else
    draw_mode_card(canvas, left, "Live", false, !_startInText);
    draw_mode_card(canvas, left + CARD_W + CARD_GAP, "Text", true, _startInText);
#endif

#if HAL_USE_BAT
    drawBattery(4, 5, 22, 11, true);
#endif

    is_rendered = true;
    return true;
}
#else
bool GeminiApp::drawMainScreen()
{
    if (is_rendered)
        return false;

    _chat.clear();
    _partialPrompt = "";

    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->fillRect(0, 0, _hal->canvas()->width(), 20, THEME_COLOR_BG);
    _hal->canvas()->setTextColor(THEME_COLOR_TITLE);
    _hal->canvas()->setFont(FONT_16);

    int offestY = TEXT_PADDING;
    int lineHeight = _hal->canvas()->fontHeight(FONT_16);
    _hal->canvas()->drawCenterString(HOME_TITLE, _hal->canvas()->width() / 2, offestY);
    offestY += lineHeight * 2;
    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2, offestY, 64, 64, image_data_gemini_icon, TFT_BLACK);

    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2 - 50 - 8, offestY + 7, 50, 50, image_data_mic, TFT_BLACK);
    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2 + 64 + 8, offestY + 7, 50, 50, image_data_playing3, TFT_BLACK);

    offestY += 64 + 2;
    _hal->canvas()->setTextColor(TFT_WHITE);
    _hal->canvas()->drawCenterString("v" BUILD_NUMBER, _hal->canvas()->width() / 2, offestY);

#if HAL_USE_BAT
    drawBattery(4, 5, 22, 11, true);
#endif

    is_rendered = true;
    return true;
}
#endif

#if HAL_USE_BAT
#define BATTERY_CHECK_INTERVAL_MS 5000

void GeminiApp::updateBattery()
{
    uint32_t now = millis();
    if (_batLevel >= 0 && now < _batNextCheck)
        return;
    _batNextCheck = now + BATTERY_CHECK_INTERVAL_MS;

    float voltage = _hal->getBatVoltage();
    // a failed read comes back as 0 V: keep the last good value
    if (voltage < 1.0f)
        return;
    int level = _hal->getBatLevel(voltage);
    bool charging = _hal->isCharging();
    if (level != _batLevel || charging != _batCharging)
    {
        ESP_LOGI(TAG, "Battery %.2f V, %d%%%s", voltage, level, charging ? ", charging" : "");
        _batLevel = level;
        _batCharging = charging;
        is_rendered = false;
    }
}

void GeminiApp::drawBattery(int x, int y, int w, int h, bool with_percent)
{
    if (_batLevel < 0)
        return;

    auto* canvas = _hal->canvas();

    canvas->drawRect(x, y, w, h, TFT_WHITE);
    canvas->fillRect(x + w, y + 3, 2, h - 6, TFT_WHITE);

    // green when full, through yellow at half, to red when empty (RGB888)
    int level = _batLevel > 100 ? 100 : _batLevel;
    uint32_t red = level >= 50 ? 255 * (100 - level) / 50 : 255;
    uint32_t green = level >= 50 ? 255 : 255 * level / 50;
    uint32_t color = (red << 16) | (green << 8);
    int fill = (w - 4) * _batLevel / 100;
    if (fill < 1)
        fill = 1;
    canvas->fillRect(x + 2, y + 2, fill, h - 4, color);

    if (_batCharging)
    {
        // lightning bolt over the bar
        int cx = x + w / 2;
        int mid = y + h / 2;
        canvas->fillTriangle(cx + 2, y + 1, cx - 3, mid + 1, cx + 1, mid + 1, TFT_WHITE);
        canvas->fillTriangle(cx - 2, y + h - 2, cx + 3, mid - 1, cx - 1, mid - 1, TFT_WHITE);
    }

    if (with_percent)
    {
        canvas->setFont(FONT_10);
        canvas->setTextColor(TFT_LIGHTGREY);
        canvas->drawString((std::to_string(_batLevel) + "%").c_str(), x + w + 6, y + 1);
        canvas->setFont(FONT_16);
    }
}
#endif

#if HAL_USE_LED
#define LED_CHECK_INTERVAL_MS 200
// Blink and breathe timing, for the effect speeds Slow, Normal and Fast
static const uint32_t LED_BLINK_ON_MS[] = {500, 250, 125};
static const uint32_t LED_BLINK_OFF_MS[] = {1500, 750, 375};
static const uint32_t LED_BREATHE_MS[] = {4000, 2500, 1500};

// Colours are picked by name in the LED settings; "Off" or an unknown name gives no
// light. Names and values are the ones Bruce offers for this LED.
static uint32_t led_color_by_name(const std::string& name)
{
    static const struct
    {
        const char* name;
        uint32_t rgb;
    } colors[] = {{"Purple", 0x960064}, {"White", 0xFFFFFF},      {"Red", 0xFF0000},   {"Orange", 0xFF4500},
                  {"Yellow", 0xFFFF00}, {"Lime Green", 0x7CFC00}, {"Green", 0x008000}, {"Cyan", 0x00FFFF},
                  {"Blue", 0x0000FF},   {"Magenta", 0xFF00FF},    {"Pink", 0xFF1493}};
    for (const auto& color : colors)
    {
        if (name == color.name)
            return color.rgb;
    }
    return 0;
}

// What a row of "LED settings" shows for its state, e.g. "Purple, Breathe"
static std::string led_menu_summary(HAL::Hal* hal, const SETTINGS::SettingGroup_t& menu)
{
    std::string summary;
    for (const auto& item : menu.items)
    {
        if (item.key.rfind("c_", 0) != 0 && item.key.rfind("e_", 0) != 0)
            continue;
        if (!summary.empty())
            summary += ", ";
        summary += hal->settings()->getString(menu.nvs_namespace, item.key);
    }
    return summary;
}

// The settings hold one group per LED state next to "LED settings". They are taken
// out of the list of groups and opened from the rows of "LED settings" instead.
void GeminiApp::initLedMenus()
{
    // "LED settings" is the first of them
    size_t led = 3;
    while (led < _groups.size() - 1 && _groups[led].nvs_namespace != "led")
        led++;

    for (size_t i = led + 1; i < _groups.size();)
    {
        if (_groups[i].nvs_namespace == "led")
        {
            _ledMenus.push_back(_groups[i]);
            _groups.erase(_groups.begin() + i);
        }
        else
        {
            i++;
        }
    }

    for (auto& item : _groups[led].items)
    {
        if (item.type != SETTINGS::TYPE_CALLBACK)
            continue;
        for (auto& menu : _ledMenus)
        {
            if (menu.name != item.label)
                continue;
            item.value = led_menu_summary(_hal, menu);
            item.callback = [this, &menu](SETTINGS::SettingItem_t& row)
            {
                UTILS::UI::show_settings_submenu(_hal, menu);
                row.value = led_menu_summary(_hal, menu);
            };
        }
    }
}

// Low battery wins over the session state, which wins over the WiFi state.
// The LED is only touched when what it should show changes.
void GeminiApp::updateLed()
{
    const uint32_t now = millis();
    if (now < _ledNextCheck)
        return;
    _ledNextCheck = now + LED_CHECK_INTERVAL_MS;

    auto* settings = _hal->settings();
    const char* color_key = nullptr;
    if (settings->getBool("led", "enabled"))
    {
#if HAL_USE_BAT
        // the settings screen does not sample the battery itself
        updateBattery();
        const int low_level = settings->getNumber("led", "low_bat");
        if (low_level > 0 && _batLevel >= 0 && _batLevel <= low_level)
        {
            color_key = "c_lowbat";
        }
        else
#endif
#if S2S_TEXT_INPUT
            // the microphone is off while typing
            if (_appState == APP_STATE_S2S_LISTENING && _textMode)
            color_key = "c_typing";
        else
#endif
            if (_appState == APP_STATE_S2S_LISTENING)
            color_key = "c_listen";
        else if (_appState == APP_STATE_S2S_SPEAKING)
            color_key = "c_speak";
        else
            color_key = _hal->wifi()->is_connected() ? "c_online" : "c_offline";
    }

    uint32_t rgb = color_key ? led_color_by_name(settings->getString("led", color_key)) : 0;
    const uint32_t brightness = settings->getNumber("led", "brightness");
    const HAL::Color color(((rgb >> 16) & 0xFF) * brightness / 255, ((rgb >> 8) & 0xFF) * brightness / 255, (rgb & 0xFF) * brightness / 255);
    rgb = (color.r << 16) | (color.g << 8) | color.b;

    // each state has its colour under c_<state>, its effect under e_<state> and the
    // speed of that effect under s_<state>
    enum { EFFECT_STEADY, EFFECT_BLINK, EFFECT_BREATHE };
    uint32_t effect = EFFECT_STEADY;
    uint32_t speed = 1;
    if (rgb)
    {
        const std::string effect_name = settings->getString("led", std::string("e_") + (color_key + 2));
        if (effect_name == "Blink")
            effect = EFFECT_BLINK;
        else if (effect_name == "Breathe")
            effect = EFFECT_BREATHE;
        const std::string speed_name = settings->getString("led", std::string("s_") + (color_key + 2));
        if (speed_name == "Slow")
            speed = 0;
        else if (speed_name == "Fast")
            speed = 2;
    }

    const uint32_t wanted = rgb | (effect << 24) | (speed << 26);
    if (wanted == _ledApplied)
        return;
    _ledApplied = wanted;

    if (!rgb)
        _hal->led()->off();
    else if (effect == EFFECT_BLINK)
        _hal->led()->blink_periodic(color, LED_BLINK_ON_MS[speed], LED_BLINK_OFF_MS[speed]);
    else if (effect == EFFECT_BREATHE)
        _hal->led()->breathe(color, LED_BREATHE_MS[speed]);
    else
        _hal->led()->set_color(color);
}
#endif

void GeminiApp::handleMainScreenInput()
{
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();

#ifdef CONFIG_BOARD_TDECK
    // up and down move between the two mode cards and the Settings card below them
    if (_hal->keyboard()->isKeyPressing(KEY_NUM_UP) || _hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
    {
        const uint8_t key = _hal->keyboard()->isKeyPressing(KEY_NUM_DOWN) ? KEY_NUM_DOWN : KEY_NUM_UP;
        _hal->keyboard()->waitForRelease(key);
        const bool want_settings = key == KEY_NUM_DOWN;
        if (want_settings != _startOnSettings)
        {
            _startOnSettings = want_settings;
            _hal->playNextSound();
            is_rendered = false;
        }
        return;
    }
    if (_startOnSettings && (_hal->keyboard()->isKeyPressing(KEY_NUM_LEFT) || _hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT)))
    {
        // back up to the card on that side
        const uint8_t key = _hal->keyboard()->isKeyPressing(KEY_NUM_LEFT) ? KEY_NUM_LEFT : KEY_NUM_RIGHT;
        _hal->keyboard()->waitForRelease(key);
        _startOnSettings = false;
        _startInText = key == KEY_NUM_RIGHT;
        _hal->playNextSound();
        is_rendered = false;
        return;
    }
    if (_startOnSettings && _hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
        _hal->playNextSound();
        _currentScreen = SCREEN_SETTINGS;
        setState(APP_STATE_IDLE);
        is_rendered = false;
        return;
    }
#endif

    if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
        _hal->playNextSound();
        is_rendered = false;

        if (_hal->settings()->getString(GEMINI_NS, "api_key").empty())
        {
            UTILS::UI::show_error_dialog(_hal, "Failed", "API key not set. Go to settings", "OK");
            return;
        }
        if (!_hal->settings()->getBool("wifi", "enabled"))
        {
            UTILS::UI::show_error_dialog(_hal, "Failed", "WiFi is disabled", "OK");
            return;
        }

#if S2S_CHAT_MEMORY
        if (!chooseChat())
            return;
#endif
        _currentScreen = SCREEN_CHAT;
#if S2S_TEXT_INPUT
        _textMode = _startInText;
#endif
        startS2S();
    }
#if S2S_TEXT_INPUT
    else if (_hal->keyboard()->isKeyPressing(KEY_NUM_LEFT) || _hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT) ||
             _hal->keyboard()->isKeyPressing(KEY_NUM_TAB))
    {
        // two cards: any of these keys moves to the other one
        const uint8_t key = _hal->keyboard()->isKeyPressing(KEY_NUM_LEFT)    ? KEY_NUM_LEFT
                            : _hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT) ? KEY_NUM_RIGHT
                                                                             : KEY_NUM_TAB;
        const bool want_text = key == KEY_NUM_TAB ? !_startInText : key == KEY_NUM_RIGHT;
        _hal->keyboard()->waitForRelease(key);
        if (want_text != _startInText)
        {
            _startInText = want_text;
            _hal->playNextSound();
            is_rendered = false;
        }
    }
#endif
    else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
        _hal->playNextSound();
        _currentScreen = SCREEN_SETTINGS;
        setState(APP_STATE_IDLE);
        is_rendered = false;
    }
}

// ═══════════════════════════════════════════════════════════
// Settings & QR
// ═══════════════════════════════════════════════════════════

void GeminiApp::handleSettingsMenu()
{
    bool need_update = UTILS::UI::SETTINGS_SCREEN::update(_hal,
                                                          _groups,
                                                          _hintTextContext,
                                                          _descScrollContext,
                                                          [this]()
                                                          {
                                                              _currentScreen = SCREEN_START;
                                                              is_rendered = false;
                                                          });
    if (need_update)
        _hal->canvas_update();
}

bool GeminiApp::drawGeminiQRScreen()
{
    if (is_rendered)
        return false;
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->pushImage(0, 0, 135, 135, image_data_qr_gemini);
    _hal->canvas()->setFont(FONT_16);
    _hal->canvas()->setTextColor(THEME_COLOR_TITLE);
    int center_x = _hal->canvas()->height() + (_hal->canvas()->width() - _hal->canvas()->height()) / 2;
    _hal->canvas()->drawCenterString("Gemini", center_x, _hal->canvas()->height() / 2 - _hal->canvas()->fontWidth());
    _hal->canvas()->setTextColor(TFT_LIGHTGREY);
    _hal->canvas()->drawCenterString("Get API key", center_x, _hal->canvas()->height() / 2 + _hal->canvas()->fontWidth());
    is_rendered = true;
    return true;
}

void GeminiApp::handleApiKeyScreenInput()
{
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();
    if (_hal->keyboard()->isPressed())
    {
        if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();
            _currentScreen = SCREEN_SETTINGS;
            is_rendered = false;
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Response / Chat screen
// ═══════════════════════════════════════════════════════════

std::vector<std::string> GeminiApp::splitTextIntoLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string currentLine;

    for (char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(currentLine);
            currentLine.clear();
            continue;
        }
        currentLine += c;
        if (_hal->canvas()->textWidth(currentLine.c_str(), FONT_14) > _hal->canvas()->width() - (TEXT_PADDING * 2))
        {
            size_t lastSpace = currentLine.find_last_of(' ');
            if (lastSpace != std::string::npos)
            {
                lines.push_back(currentLine.substr(0, lastSpace));
                currentLine = currentLine.substr(lastSpace + 1);
            }
            else
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }
        }
    }
    if (!currentLine.empty())
        lines.push_back(currentLine);
    return lines;
}

bool GeminiApp::drawResponseScreen()
{
    if (is_rendered)
        return false;
    int scrollbar_width = 5;
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->setFont(FONT_14);

    int textStartY = TEXT_PADDING;
    int textHeight = _hal->canvas()->height() - textStartY - HINT_HEIGHT;

    if (_chat.empty() && _partialPrompt.empty())
    {
        const char* status_text = "connecting...";
        if (_appState == APP_STATE_S2S_LISTENING)
            status_text = "listening...";
        else if (_appState == APP_STATE_S2S_SPEAKING)
            status_text = "speaking...";
#if S2S_TEXT_INPUT
        if (_textMode && _appState == APP_STATE_S2S_LISTENING)
            status_text = "ENTER to type";
        else if (_textMode && _appState == APP_STATE_S2S_SPEAKING)
            status_text = "answering...";
#endif
        else if (_appState == APP_STATE_S2S_ERROR)
            status_text = "reconnecting...";

        // centred in the area above the hint row, on the middle of the text rather than its top edge
        _hal->canvas()->setTextColor(TFT_DARKGRAY);
        _hal->canvas()->setFont(FONT_16);
        _hal->canvas()->setTextDatum(middle_center);
        _hal->canvas()->drawString(status_text, _hal->canvas()->width() / 2, (_hal->canvas()->height() - HINT_HEIGHT) / 2);
        _hal->canvas()->setTextDatum(top_left);
        _hal->canvas()->setFont(FONT_14);
    }
    else
    {
        int lineHeight = _hal->canvas()->fontHeight(FONT_14);
        int maxVisibleLines = textHeight / lineHeight;
        std::vector<std::string> partial_prompt = splitTextIntoLines(_partialPrompt);
        _totalLines = _chat.size() + partial_prompt.size();

        if (_scrollPosition > _totalLines - maxVisibleLines)
            _scrollPosition = std::max(0, _totalLines - maxVisibleLines);

        // Partial prompt type depends on current state:
        // listening → user input transcript, speaking → model output transcript
        bool partial_is_user = (_appState == APP_STATE_S2S_LISTENING);

        for (int i = 0; i < maxVisibleLines && i + _scrollPosition < _totalLines; i++)
        {
            int y = textStartY + (i * lineHeight);
            int idx = i + _scrollPosition;

            bool is_partial = (idx >= (int)_chat.size());
            bool is_user;
            if (is_partial)
                is_user = partial_is_user;
            else
                is_user = (_chat[idx].first == HISTORY_ITEM_TYPE_USER);

            bool is_last = idx >= _lastHistoryIndex;

            if (is_user)
            {
                _hal->canvas()->fillRect(0, y, TEXT_PADDING - 1, lineHeight, is_last ? TFT_WHITE : TFT_DARKGRAY);
                _hal->canvas()->setTextColor(is_last ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY);
            }
            else
            {
                _hal->canvas()->fillRect(0,
                                         y,
                                         TEXT_PADDING - 1,
                                         lineHeight,
                                         is_last ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY);
                _hal->canvas()->setTextColor(is_last ? TFT_WHITE : TFT_DARKGRAY);
            }

            _hal->canvas()->drawString(is_partial ? partial_prompt[idx - _chat.size()].c_str() : _chat[idx].second.c_str(),
                                       TEXT_PADDING,
                                       y);
        }

        int scrollbar_height = lineHeight * maxVisibleLines;
        if (_totalLines > maxVisibleLines)
        {
            int sx = _hal->canvas()->width() - scrollbar_width - 1;
            _hal->canvas()->drawRect(sx, textStartY, scrollbar_width, scrollbar_height, TFT_DARKGREY);
            int th = scrollbar_height * maxVisibleLines / _totalLines;
            int tp = textStartY + (scrollbar_height - th) * _scrollPosition / (_totalLines - maxVisibleLines);
            _hal->canvas()->fillRect(sx, tp, scrollbar_width, th, TFT_ORANGE);
        }
    }
#if HAL_USE_BAT
    // bottom-left corner, beside the key hints and as tall as their text
    drawBattery(2, _hal->canvas()->height() - HINT_HEIGHT + 1, CHAT_BATTERY_WIDTH, 9, false);
#endif
    is_rendered = true;
    return true;
}

void GeminiApp::handleResponseScreenInput()
{
    int lineHeight = _hal->canvas()->fontHeight(FONT_14);
    int textHeight = _hal->canvas()->height() - 14;
    int maxVisibleLines = textHeight / lineHeight;
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();

    if (_hal->keyboard()->isPressed())
    {
        uint32_t now = millis();

        if (_hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition > 0)
                {
                    _scrollPosition--;
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition < _totalLines - maxVisibleLines)
                {
                    _scrollPosition++;
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition > 0)
                {
                    _scrollPosition = std::max(0, _scrollPosition - maxVisibleLines);
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition < _totalLines - maxVisibleLines)
                {
                    _scrollPosition = std::min(_totalLines - maxVisibleLines, _scrollPosition + maxVisibleLines);
                    is_rendered = false;
                }
            }
        }
#if S2S_TEXT_INPUT
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_TAB) && _appState == APP_STATE_S2S_LISTENING)
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_TAB);
            setTextMode(!_textMode);
            is_rendered = false;
            if (_textMode)
                typeMessage();
        }
#endif
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();
            stopS2S();
#if S2S_CHAT_MEMORY
            closeChat();
#endif
#if S2S_TEXT_INPUT
            _textMode = false;
#endif
            setState(APP_STATE_IDLE);
            _partialPrompt = "";
            _currentScreen = SCREEN_START;
            is_rendered = false;
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            _hal->playNextSound();
            is_rendered = false;

#if S2S_TEXT_INPUT
            if (_textMode && _appState == APP_STATE_S2S_LISTENING)
            {
                typeMessage();
            }
            else
#endif
            if (_appState == APP_STATE_S2S_LISTENING || _appState == APP_STATE_S2S_SPEAKING)
            {
                xEventGroupSetBits(_control_event_group, S2S_USER_INTERRUPT_BIT);
            }
            else if (!_s2s_active)
            {
                startS2S();
            }
        }
    }
    else
    {
        is_repeat = false;
    }
}

// While text is coming in the screen follows its last line: the position is set past
// the end and drawing brings it back to the last full screen. Set to the start of the
// incoming text, a reply longer than the screen stayed on its first lines while the
// rest was written out of sight.
void GeminiApp::updateScrollPosition() { _scrollPosition = _partialPrompt.empty() ? _lastHistoryIndex : INT_MAX / 2; }

void GeminiApp::trimChat()
{
#if S2S_CHAT_MEMORY
    // Every line kept costs a session memory it has little of: free heap fell by about
    // half a KB with each exchange of a voice chat. The whole chat is on the card, to
    // be read in the viewer.
    const int max_lines = 100;
#else
    const int max_lines = MAX_CHAT_LINES;
#endif
    if ((int)_chat.size() > max_lines)
    {
        int excess = _chat.size() - max_lines;
        _chat.erase(_chat.begin(), _chat.begin() + excess);
        _lastHistoryIndex = std::max(0, _lastHistoryIndex - excess);
        _scrollPosition = std::max(0, _scrollPosition - excess);
    }
}

// ═══════════════════════════════════════════════════════════
// Animation & state
// ═══════════════════════════════════════════════════════════

#if 1
float linear(float t) { return t; }
float easeInQuad(float t) { return powf(t, 2.0f); }
float easeOutQuad(float t) { return t * (2.0f - t); }
float easeInOutQuad(float t) { return t < .5f ? 2.0f * powf(t, 2) : -1.0f + (4.0f - 2.0f * t) * t; }
float easeInCubic(float t) { return powf(t, 3.0f); }
float easeOutCubic(float t) { return 1.0f - powf(1.0f - t, 3.0f); }
float easeInOutCubic(float t)
{
    return t < .5f ? 4.0f * powf(t, 3) : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
}
float easeInQuart(float t) { return powf(t, 4.0f); }
float easeOutQuart(float t) { return 1.0f - powf(1.0f - t, 4.0f); }
float easeInOutQuart(float t) { return t < 0.5 ? 8.0f * t * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 4.0f) / 2.0f; }
float easeInQuint(float t) { return powf(t, 5.0f); }
float easeOutQuint(float t) { return 1.0f - powf(1.0f - t, 5.0f); }
float easeInOutQuint(float t) { return t < 0.5 ? 16.0f * t * t * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 5.0f) / 2.0f; }
float easeInSine(float t) { return 1.0f - cosf((t * M_PI) / 2.0f); }
float easeOutSine(float t) { return sinf((t * M_PI) / 2.0f); }
float easeInOutSine(float t) { return -(cosf(M_PI * t) - 1.0f) / 2.0f; }
float easeInExpo(float t) { return t == 0 ? 0 : powf(2, 10.0f * t - 10.0f); }
float easeOutExpo(float t) { return t == 1.0f ? 1.0f : 1.0f - powf(2, -10.0f * t); }
float easeInOutExpo(float t)
{
    return t == 0      ? 0
           : t == 1.0f ? 1.0f
           : t < 0.5f  ? powf(2, 20.0f * t - 10.0f) / 2.0f
                       : (2.0f - powf(2, -20.0f * t + 10.0f)) / 2.0f;
}
float easeInCirc(float t) { return 1.0f - sqrtf(1.0f - powf(t, 2.0f)); }
float easeOutCirc(float t) { return sqrtf(1.0f - powf(t - 1, 2.0f)); }
float easeInOutCirc(float t)
{
    return t < 0.5f ? (1.0f - sqrtf(1.0f - powf(2.0f * t, 2.0f))) / 2.0f
                    : (sqrtf(1 - powf(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
}
#endif

// Status icons for the chat screen, drawn on an 11x11 grid. The 50x50 bitmaps used on
// the main screen turn to mush when scaled this far down, so these are separate drawings.
// `level` (0-255) dims the icon for the pulse.
static uint32_t icon_color(uint32_t rgb, uint8_t level)
{
    return ((((rgb >> 16) & 0xFF) * level / 255) << 16) | ((((rgb >> 8) & 0xFF) * level / 255) << 8) |
           ((rgb & 0xFF) * level / 255);
}

static void draw_wifi_arcs(LGFX_Sprite* c, int x, int y, uint32_t color)
{
    c->drawArc(x + 5, y + 10, 8, 8, 225, 315, color);
    c->drawArc(x + 5, y + 10, 5, 5, 225, 315, color);
    c->fillRect(x + 4, y + 8, 3, 2, color);
}

static void draw_status_icon(LGFX_Sprite* c, AppState state, int x, int y, uint8_t level)
{
    switch (state)
    {
    case APP_STATE_S2S_LISTENING:
    {
        // microphone
        const uint32_t color = icon_color(0xFF5A5A, level);
        c->fillRoundRect(x + 4, y, 3, 7, 1, color);
        c->drawFastVLine(x + 2, y + 4, 3, color);
        c->drawFastVLine(x + 8, y + 4, 3, color);
        c->drawPixel(x + 3, y + 7, color);
        c->drawPixel(x + 7, y + 7, color);
        c->drawFastHLine(x + 4, y + 8, 3, color);
        c->drawPixel(x + 5, y + 9, color);
        c->drawFastHLine(x + 3, y + 10, 5, color);
        break;
    }
    case APP_STATE_S2S_SPEAKING:
    {
        // loudspeaker with two sound waves
        const uint32_t color = icon_color(0x4DB8FF, level);
        c->fillRect(x, y + 4, 3, 3, color);
        c->fillTriangle(x + 1, y + 5, x + 5, y + 1, x + 5, y + 9, color);
        c->drawFastVLine(x + 7, y + 4, 3, color);
        c->drawFastVLine(x + 9, y + 3, 5, color);
        c->drawPixel(x + 8, y + 2, color);
        c->drawPixel(x + 8, y + 8, color);
        break;
    }
    case APP_STATE_CONNECTING_WIFI:
        draw_wifi_arcs(c, x, y, icon_color(0xFFC000, level));
        break;
    case APP_STATE_DISCONNECTED:
    {
        // WiFi, struck through
        draw_wifi_arcs(c, x, y, icon_color(0x909090, level));
        const uint32_t color = icon_color(0xFF4040, level);
        c->drawLine(x + 1, y + 10, x + 9, y + 1, color);
        c->drawLine(x + 2, y + 10, x + 10, y + 1, color);
        break;
    }
    case APP_STATE_S2S_CONNECTING:
    {
        // globe
        const uint32_t color = icon_color(0x40D0FF, level);
        c->drawCircle(x + 5, y + 5, 5, color);
        c->drawEllipse(x + 5, y + 5, 2, 5, color);
        c->drawFastHLine(x, y + 5, 11, color);
        break;
    }
    case APP_STATE_S2S_ERROR:
    {
        // warning triangle
        const uint32_t color = icon_color(0xFF4040, level);
        c->drawTriangle(x + 5, y, x, y + 10, x + 10, y + 10, color);
        c->drawFastVLine(x + 5, y + 4, 3, color);
        c->drawPixel(x + 5, y + 8, color);
        break;
    }
    default:
        break;
    }
}

void GeminiApp::setState(AppState state)
{
    if (_appState == state)
        return;
    ESP_LOGI(TAG, "State -> %d, heap=%lu", state, (unsigned long)esp_get_free_heap_size());
    _appState = state;
    _lastActivity = millis();
    // s_anim_bg_valid = false;
    _anim_context.timer_start = millis();
    // _anim_context.last_alpha = 255;
}

// Sleep (Cardputer) or power off (StickS3) once nothing has happened for the time
// chosen in the settings. A key press and a change of state both count: a chat left
// listening runs out, one that is still being answered does not.
void GeminiApp::checkAutoOff()
{
    const uint32_t now = millis();
    if (now - _autoOffLastCheck < 1000)
        return;
    _autoOffLastCheck = now;

    const int minutes = atoi(_hal->settings()->getString("power", "auto_off").c_str());
    if (minutes <= 0 || _appState == APP_STATE_S2S_CONNECTING || _appState == APP_STATE_S2S_SPEAKING)
    {
        _lastActivity = now;
        return;
    }
    const uint32_t idle = std::min(now - _lastActivity, now - _hal->keyboard()->lastPressedTime());
    if (idle < (uint32_t)minutes * 60000)
        return;

    ESP_LOGW(TAG, "Idle for %d min: turning off", minutes);
    if (_s2s_active)
    {
        stopS2S();
#if S2S_CHAT_MEMORY
        closeChat();
#endif
    }
    _hal->powerOff();
    // not reached unless turning off failed: start counting again
    _lastActivity = millis();
}

// Draw the animation
bool GeminiApp::drawAnimation(bool need_update, bool small)
{
    const uint16_t* icon = nullptr;
    switch (_appState)
    {
    case APP_STATE_IDLE:
        break;
    case APP_STATE_DISCONNECTED:
        icon = image_data_disconnected;
        break;
    case APP_STATE_S2S_ERROR:
        icon = image_data_conn_error;
        break;
    case APP_STATE_CONNECTING_WIFI:
        icon = image_data_connecting_wifi;
        break;
    case APP_STATE_S2S_CONNECTING:
        icon = image_data_connecting_internet;
        break;
    case APP_STATE_S2S_SPEAKING:
        icon = image_data_playing3;
        break;
    case APP_STATE_S2S_LISTENING:
        icon = image_data_mic;
        break;
    }
    if (small && !icon)
        return false;
    if (icon && !small)
        _sprite->pushImage(0, 0, 50, 50, icon);

    uint32_t timer_now = millis();
    // int x_offset = (_hal->canvas()->width() - _sprite->width()) / 2;
    // int y_offset = (_hal->canvas()->height() - _sprite->height()) / 2;
#ifdef CONFIG_BOARD_STICKS3
    // the case covers the outermost pixels of the screen, keep the icon clear of the corner
    int x_offset = _hal->canvas()->width() - _sprite->width() - 2;
    int y_offset = 2;
#else
    int x_offset = _hal->canvas()->width() - _sprite->width() - 1;
    int y_offset = 0;
#endif

    uint32_t full_pos = (timer_now - _anim_context.timer_start) % (_anim_context.duration * 2);
    uint32_t timer_pos = (full_pos % _anim_context.duration) * _anim_context.steps / _anim_context.duration;
    // make reverse move when > duration
    float ifloat;
    if (full_pos >= _anim_context.duration)
    {
        ifloat = easeInOutSine(float(_anim_context.steps - timer_pos) / float(_anim_context.steps));
    }
    else
    {
        ifloat = easeInOutSine(float(timer_pos) / float(_anim_context.steps));
    }
    uint8_t alpha = ifloat * 255;
    if (alpha == _anim_context.last_alpha && need_update == false)
        return false;
    _anim_context.last_alpha = alpha;

    if (small)
    {
        // Chat screen: a small icon in the bottom-right corner, beside the key hints.
        // It pulses, but stays above a floor so that at this size it never disappears.
        // Only its own square is redrawn, the rest of the screen is left alone.
        const uint8_t level = 90 + (uint32_t)alpha * (255 - 90) / 255;
        const int x = _hal->canvas()->width() - 3 - CHAT_ICON_SIZE;
        const int y = _hal->canvas()->height() - HINT_HEIGHT;
        _hal->canvas()->fillRect(x, y, CHAT_ICON_SIZE, CHAT_ICON_SIZE, THEME_COLOR_BG);
#if S2S_TEXT_INPUT
        if (_textMode && _appState == APP_STATE_S2S_LISTENING)
        {
            // keyboard, in place of the microphone
            const uint32_t color = icon_color(0xFFC000, level);
            _hal->canvas()->drawRoundRect(x, y + 2, 11, 8, 1, color);
            for (int kx = 2; kx <= 8; kx += 2)
                _hal->canvas()->drawPixel(x + kx, y + 4, color);
            _hal->canvas()->drawFastHLine(x + 3, y + 7, 5, color);
            return true;
        }
#endif
        draw_status_icon(_hal->canvas(), _appState, x, y, level);
        return true;
    }

    // mixing sprite with background according to alpha
    for (int py = 0; py < _sprite->height(); py++)
    {
        for (int px = 0; px < _sprite->width(); px++)
        {
            uint16_t fg = _sprite->readPixel(px, py);                              // foreground (sprite pixel)
            uint16_t bg = _hal->canvas()->readPixel(px + x_offset, py + y_offset); // background pixel

            uint32_t fg24 = _sprite->color16to24(fg);
            uint8_t fg_r = (fg24 >> 16) & 0xFF;
            uint8_t fg_g = (fg24 >> 8) & 0xFF;
            uint8_t fg_b = fg24 & 0xFF;

            uint32_t bg24 = _hal->canvas()->color16to24(bg);
            uint8_t bg_r = (bg24 >> 16) & 0xFF;
            uint8_t bg_g = (bg24 >> 8) & 0xFF;
            uint8_t bg_b = bg24 & 0xFF;

            uint8_t r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
            uint8_t g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
            uint8_t b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
            // transparent color
            if (fg == THEME_COLOR_BG)
                _sprite->drawPixel(px, py, _sprite->color888(bg_r, bg_g, bg_b));
            else
                _sprite->drawPixel(px, py, _sprite->color888(r, g, b));
        }
    }
    _sprite->pushSprite(x_offset, y_offset);
    is_rendered = false;
    return true;
}
#if 0
bool GeminiApp::drawAnimation(bool need_update)
{
    const uint16_t* icon = nullptr;
    switch (_appState)
    {
    case APP_STATE_IDLE:
        break;
    case APP_STATE_DISCONNECTED:
        icon = image_data_disconnected;
        break;
    case APP_STATE_S2S_ERROR:
        icon = image_data_conn_error;
        break;
    case APP_STATE_CONNECTING_WIFI:
        icon = image_data_connecting_wifi;
        break;
    case APP_STATE_S2S_CONNECTING:
        icon = image_data_connecting_internet;
        break;
    case APP_STATE_S2S_SPEAKING:
        icon = image_data_playing3;
        break;
    case APP_STATE_S2S_LISTENING:
        icon = image_data_mic;
        break;
    }

    auto canvas = _hal->canvas();
    int x_off = canvas->width() - ICON_SZ - 1;

    if (!icon)
    {
        if (s_anim_bg_valid)
        {
            canvas->pushImage(x_off, 0, ICON_SZ, ICON_SZ, s_anim_bg);
            s_anim_bg_valid = false;
            return true;
        }
        return false;
    }

    uint32_t timer_now = millis();
    uint32_t full_pos = (timer_now - _anim_context.timer_start) % (_anim_context.duration * 2);
    float t = float(full_pos) / float(_anim_context.duration * 2);
    float ifloat = easeInOutSine(t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f);

    uint8_t alpha = (uint8_t)(ifloat * 255);
    if (alpha == _anim_context.last_alpha && !need_update)
        return false;
    _anim_context.last_alpha = alpha;

    if (!s_anim_bg_valid || need_update)
        canvas->readRect(x_off, 0, ICON_SZ, ICON_SZ, s_anim_bg);
    else
        canvas->pushImage(x_off, 0, ICON_SZ, ICON_SZ, s_anim_bg);
    s_anim_bg_valid = true;

    _sprite->pushImage(0, 0, ICON_SZ, ICON_SZ, icon);
    uint8_t inv_alpha = 255 - alpha;

    for (int py = 0; py < ICON_SZ; py++)
    {
        for (int px = 0; px < ICON_SZ; px++)
        {
            uint16_t fg = _sprite->readPixel(px, py);
            if (fg == THEME_COLOR_BG)
                continue;

            uint16_t bg = canvas->readPixel(x_off + px, py);

            uint32_t fg24 = canvas->color16to24(fg);
            uint32_t bg24 = canvas->color16to24(bg);

            uint8_t r = (((fg24 >> 16) & 0xFF) * alpha + ((bg24 >> 16) & 0xFF) * inv_alpha) / 255;
            uint8_t g = (((fg24 >> 8) & 0xFF) * alpha + ((bg24 >> 8) & 0xFF) * inv_alpha) / 255;
            uint8_t b = ((fg24 & 0xFF) * alpha + (bg24 & 0xFF) * inv_alpha) / 255;

            canvas->drawPixel(x_off + px, py, canvas->color888(r, g, b));
        }
    }

    return true;
}
#endif