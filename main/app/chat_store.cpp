/**
 * @file chat_store.cpp
 * @brief Chats kept on the SD card, so that one can be continued after a restart
 *
 * One text file per chat in /M5Gemini/chats, a line per turn: "U " and what the user
 * said or typed, "M " and what Gemini answered. Beside it, while Google still honours
 * it, the handle that resumes the session itself.
 *
 * The card is driven through FatFs directly, not through the VFS layer the rest of the
 * firmware uses (SDCard::mount). That layer takes 10 KB of heap in one block for as
 * long as the card is mounted. A voice session cannot spare it: with 19 KB left, the
 * TLS buffers for sending the microphone and receiving at once (20 KB) no longer fit,
 * and mounting only for the moment of a write fails for want of a block that size.
 * Here a mounted card costs about 2 KB, so it stays mounted while a chat is open.
 */

// nothing else ahead of the guard: without an SD card this file has to compile to nothing
#include "hal_config.h"
#if HAL_USE_SDCARD
#include "chat_store.h"
#include "app/utils/ui/dialog.h"
#include "app/utils/ui/draw_helper.h"
#include "app/utils/ui/key_repeat.h"
#include "app/utils/theme/theme_define.h"
#include "app/utils/anim/hl_text.h"
#include "common_define.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

static const char* TAG = "CHAT_STORE";
static const char* DRIVE = "0:";
static const char* APP_DIR = "0:/M5Gemini";
static const char* CHATS_DIR = "0:/M5Gemini/chats";

#ifdef CONFIG_BOARD_TDECK
// the T-Deck's SD card shares the display's SPI bus, already set up by then, as in hal/sdcard
#define PIN_NUM_MISO 38
#define PIN_NUM_MOSI 41
#define PIN_NUM_CLK 40
#define PIN_NUM_CS GPIO_NUM_39
#else
// the SD card slot of the Cardputer, as in hal/sdcard
#define PIN_NUM_MISO 39
#define PIN_NUM_MOSI 14
#define PIN_NUM_CLK 40
#define PIN_NUM_CS GPIO_NUM_12
#endif

#define MAX_CHAT_ID 9999
#define MAX_LISTED_CHATS 200
#define MAX_TITLE_BYTES 80

#define HINT_HEIGHT 12
#define ITEMS_Y_OFFSET 20

// What the chat viewer holds of a chat at a time
#define VIEW_MAX_TURNS 60
#define VIEW_MAX_BYTES 6000
// How many turns back it reads when scrolled past the top of that
#define VIEW_STEP_BACK 8
#define VIEW_TEXT_PADDING 5
#define VIEW_SCROLLBAR_WIDTH 6
// The editor is told a length limit: the text as it is plus room to add to it
#define VIEW_EDIT_ROOM 300
#define VIEW_EDIT_MIN 500

static const char* HINT_PICK = "[ENTER] OPEN [TAB] VIEW [DEL] FORGET [ESC]";
static const char* HINT_VIEW = "[\xE2\x86\x91][\xE2\x86\x93] [ENTER] VIEW [DEL] FORGET [ESC]";
static const char* HINT_FORGET = "[\xE2\x86\x91][\xE2\x86\x93] [ENTER] FORGET [ESC] BACK";
static const char* HINT_CHAT = "[\xE2\x86\x91][\xE2\x86\x93] [\xE2\x86\x90][\xE2\x86\x92] TURN [ENTER] EDIT [ESC]";

namespace CHAT_STORE
{
    struct Entry
    {
        int id;
        std::string title;
    };

    // ── The card ──

    static sdmmc_card_t* s_card = nullptr;
    static FATFS* s_fs = nullptr;
    static bool s_bus_owned = false;

    static bool mount_card()
    {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        spi_bus_config_t bus = {};
        bus.mosi_io_num = PIN_NUM_MOSI;
        bus.miso_io_num = PIN_NUM_MISO;
        bus.sclk_io_num = PIN_NUM_CLK;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.data4_io_num = -1;
        bus.data5_io_num = -1;
        bus.data6_io_num = -1;
        bus.data7_io_num = -1;
        bus.max_transfer_sz = 4092;
#ifdef CONFIG_BOARD_TDECK
        // the display has set the bus up already; asking again only logs an error
        esp_err_t err = ESP_ERR_INVALID_STATE;
#else
        esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
#endif
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            return false;
        s_bus_owned = err == ESP_OK;

        sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
        device.gpio_cs = PIN_NUM_CS;
        device.host_id = (spi_host_device_t)host.slot;

        if (host.init() == ESP_OK)
        {
            sdspi_dev_handle_t handle;
            if (sdspi_host_init_device(&device, &handle) == ESP_OK)
            {
                // from here on the host is addressed by the device on it
                host.slot = handle;
                s_card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
                if (s_card && sdmmc_card_init(&host, s_card) == ESP_OK)
                {
                    ff_diskio_register_sdmmc(0, s_card);
                    s_fs = new FATFS;
                    if (f_mount(s_fs, DRIVE, 1) == FR_OK)
                        return true;
                    f_mount(nullptr, DRIVE, 0);
                    ff_diskio_unregister(0);
                    delete s_fs;
                    s_fs = nullptr;
                }
                free(s_card);
                s_card = nullptr;
            }
            if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG)
                host.deinit_p(host.slot);
            else
                host.deinit();
        }
        if (s_bus_owned)
            spi_bus_free(SDSPI_DEFAULT_HOST);
        return false;
    }

    bool mount(HAL::Hal* hal)
    {
        if (s_fs)
            return true;
        // one user of the card at a time
        if (hal->sdcard()->is_mounted())
            hal->sdcard()->eject();

        if (!mount_card())
        {
            // a card just put in sometimes needs a second try
            delay(200);
            if (!mount_card())
            {
                ESP_LOGW(TAG, "No SD card");
                return false;
            }
        }
        f_mkdir(APP_DIR);
        f_mkdir(CHATS_DIR);
        FILINFO info;
        if (f_stat(CHATS_DIR, &info) != FR_OK || !(info.fattrib & AM_DIR))
        {
            unmount(hal);
            return false;
        }
        ESP_LOGD(TAG, "Card mounted, heap=%d", (int)esp_get_free_heap_size());
        return true;
    }

    void unmount(HAL::Hal* hal)
    {
        if (!s_fs)
            return;
        f_mount(nullptr, DRIVE, 0);
        ff_diskio_unregister(0);
        delete s_fs;
        s_fs = nullptr;

        if (s_card->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG)
            s_card->host.deinit_p(s_card->host.slot);
        else
            s_card->host.deinit();
        free(s_card);
        s_card = nullptr;
        if (s_bus_owned)
            spi_bus_free(SDSPI_DEFAULT_HOST);
    }

    // ── Files ──

    static std::string chat_path(int id, const char* extension)
    {
        char path[48];
        snprintf(path, sizeof(path), "%s/%04d.%s", CHATS_DIR, id, extension);
        return path;
    }

    // A file being read line by line. The file object holds a sector buffer, so it is
    // kept off the stack.
    struct Reader
    {
        std::unique_ptr<FIL> file;
        char buffer[64];
        UINT length = 0;
        UINT position = 0;

        bool open(const std::string& path)
        {
            file.reset(new FIL);
            if (f_open(file.get(), path.c_str(), FA_READ) == FR_OK)
                return true;
            file.reset();
            return false;
        }

        bool read_line(std::string& line)
        {
            line.clear();
            bool any = false;
            while (true)
            {
                if (position >= length)
                {
                    position = 0;
                    if (f_read(file.get(), buffer, sizeof(buffer), &length) != FR_OK || length == 0)
                    {
                        length = 0;
                        return any;
                    }
                }
                const char c = buffer[position++];
                any = true;
                if (c == '\n')
                    return true;
                if (c != '\r')
                    line += c;
            }
        }

        ~Reader()
        {
            if (file)
                f_close(file.get());
        }
    };

    static bool write_file(const std::string& path, BYTE mode, const std::string& content)
    {
        std::unique_ptr<FIL> file(new FIL);
        if (f_open(file.get(), path.c_str(), mode) != FR_OK)
            return false;
        UINT written = 0;
        const bool ok = f_write(file.get(), content.data(), content.size(), &written) == FR_OK && written == content.size();
        return (f_close(file.get()) == FR_OK) && ok;
    }

    // a turn is one line of the file
    static std::string escape_line(const std::string& text)
    {
        std::string escaped;
        for (char c : text)
        {
            if (c == '\n')
                escaped += "\\n";
            else if (c == '\\')
                escaped += "\\\\";
            else if (c != '\r')
                escaped += c;
        }
        return escaped;
    }

    static bool parse_line(const std::string& line, Turn& turn)
    {
        if (line.size() < 3 || line[1] != ' ' || (line[0] != 'U' && line[0] != 'M'))
            return false;
        turn.user = line[0] == 'U';
        turn.text.clear();
        for (size_t i = 2; i < line.size(); i++)
        {
            if (line[i] == '\\' && i + 1 < line.size())
            {
                i++;
                turn.text += line[i] == 'n' ? '\n' : line[i];
            }
            else
            {
                turn.text += line[i];
            }
        }
        return true;
    }

    // Length of the UTF-8 character starting at text[pos], 0 if it is not a whole valid one
    static size_t utf8_char_len(const std::string& text, size_t pos)
    {
        const uint8_t lead = (uint8_t)text[pos];
        size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || pos + len > text.size())
            return 0;
        for (size_t i = 1; i < len; i++)
        {
            if (((uint8_t)text[pos + i] & 0xC0) != 0x80)
                return 0;
        }
        return len;
    }

    // ids of the saved chats, newest first
    static std::vector<int> list_ids()
    {
        std::vector<int> ids;
        FF_DIR dir;
        if (f_opendir(&dir, CHATS_DIR) != FR_OK)
            return ids;
        FILINFO info;
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0')
        {
            const char* dot = strrchr(info.fname, '.');
            if (!dot || strcasecmp(dot, ".txt") != 0)
                continue;
            const int id = atoi(info.fname);
            if (id > 0 && id <= MAX_CHAT_ID)
                ids.push_back(id);
        }
        f_closedir(&dir);
        std::sort(ids.begin(), ids.end(), [](int a, int b) { return a > b; });
        return ids;
    }

    // A chat is listed by how it began
    static std::vector<Entry> list()
    {
        std::vector<Entry> entries;
        for (int id : list_ids())
        {
            if (entries.size() >= MAX_LISTED_CHATS)
                break;
            Entry entry = {id, ""};
            Reader reader;
            std::string line;
            Turn turn;
            if (reader.open(chat_path(id, "txt")) && reader.read_line(line) && parse_line(line, turn))
            {
                for (size_t pos = 0; pos < turn.text.size() && entry.title.size() < MAX_TITLE_BYTES;)
                {
                    const size_t len = utf8_char_len(turn.text, pos);
                    if (len == 0 || turn.text[pos] == '\n')
                        entry.title += ' ';
                    else
                        entry.title.append(turn.text, pos, len);
                    pos += len ? len : 1;
                }
            }
            if (entry.title.empty())
                entry.title = "Chat " + std::to_string(id);
            entries.push_back(entry);
        }
        return entries;
    }

    static void remove(int id)
    {
        f_unlink(chat_path(id, "txt").c_str());
        f_unlink(chat_path(id, "ses").c_str());
    }

    void trim(std::vector<Turn>& turns)
    {
        size_t bytes = 0;
        for (const auto& turn : turns)
            bytes += turn.text.size();
        size_t drop = 0;
        while (turns.size() - drop > 1 && (turns.size() - drop > MAX_TURNS || bytes > MAX_TURNS_BYTES))
            bytes -= turns[drop++].text.size();
        turns.erase(turns.begin(), turns.begin() + drop);
    }

    bool load(int id, std::vector<Turn>& turns, std::string& handle)
    {
        turns.clear();
        handle.clear();
        {
            Reader reader;
            if (!reader.open(chat_path(id, "txt")))
                return false;
            std::string line;
            Turn turn;
            while (reader.read_line(line))
            {
                if (!parse_line(line, turn))
                    continue;
                turns.push_back(turn);
                // as it goes, or a long chat would not fit in memory while it is read
                if (turns.size() > MAX_TURNS * 2)
                    trim(turns);
            }
            trim(turns);
        }

        Reader reader;
        if (reader.open(chat_path(id, "ses")))
            reader.read_line(handle);
        ESP_LOGI(TAG, "Chat %d: %d turns%s", id, (int)turns.size(), handle.empty() ? "" : ", with a session handle");
        return true;
    }

    int create()
    {
        const std::vector<int> ids = list_ids();
        const int id = ids.empty() ? 1 : ids.front() + 1;
        return id <= MAX_CHAT_ID ? id : 0;
    }

    bool append(int id, const Turn& turn)
    {
        const std::string line = std::string(turn.user ? "U " : "M ") + escape_line(turn.text) + "\n";
        if (write_file(chat_path(id, "txt"), FA_OPEN_APPEND | FA_WRITE, line))
            return true;
        ESP_LOGE(TAG, "Cannot write chat %d", id);
        return false;
    }

    void save_handle(int id, const std::string& handle)
    {
        const std::string path = chat_path(id, "ses");
        if (handle.empty())
            f_unlink(path.c_str());
        else
            write_file(path, FA_CREATE_ALWAYS | FA_WRITE, handle);
    }

    // Text as the inside of a JSON string. Bytes that are not valid UTF-8 are left out:
    // a transcription cut in the middle of a character would get the message refused.
    static void append_json_text(std::string& json, const std::string& text)
    {
        for (size_t pos = 0; pos < text.size();)
        {
            const size_t len = utf8_char_len(text, pos);
            if (len == 0)
            {
                pos++;
                continue;
            }
            const char c = text[pos];
            if (c == '"' || c == '\\')
            {
                json += '\\';
                json += c;
            }
            else if (c == '\n')
            {
                json += "\\n";
            }
            else if ((uint8_t)c < 0x20)
            {
                json += ' ';
            }
            else
            {
                json.append(text, pos, len);
            }
            pos += len;
        }
    }

    std::string history_message(const std::vector<Turn>& turns)
    {
        // The turns are already cut to what a session is given: they are paid for as
        // input tokens every time one starts. It has to begin with one of the user's.
        size_t first = 0;
        while (first < turns.size() && !turns[first].user)
            first++;
        if (first >= turns.size())
            return "";

        std::string json = "{\"clientContent\":{\"turns\":[";
        for (size_t i = first; i < turns.size(); i++)
        {
            // two turns in a row from the same side, as after an interrupted reply, go as one
            if (i > first && turns[i].user == turns[i - 1].user)
            {
                json += "\\n";
            }
            else
            {
                if (i > first)
                    json += "\"}]},";
                json += turns[i].user ? "{\"role\":\"user\",\"parts\":[{\"text\":\"" : "{\"role\":\"model\",\"parts\":[{\"text\":\"";
            }
            append_json_text(json, turns[i].text);
        }
        json += "\"}]}],\"turnComplete\":true}}";
        return json;
    }

    // ── Reading and editing a chat ──

    // Turns of a chat from turn `first` on, as many as the limits allow: a chat can be
    // longer than what fits in memory. Returns how many turns the file holds.
    static int load_window(int id, int first, std::vector<Turn>& turns)
    {
        turns.clear();
        Reader reader;
        if (!reader.open(chat_path(id, "txt")))
            return 0;
        std::string line;
        Turn turn;
        int total = 0;
        size_t bytes = 0;
        bool full = false;
        while (reader.read_line(line))
        {
            if (!parse_line(line, turn))
                continue;
            if (total >= first && !full)
            {
                if (!turns.empty() && (turns.size() >= VIEW_MAX_TURNS || bytes + turn.text.size() > VIEW_MAX_BYTES))
                {
                    full = true;
                }
                else
                {
                    bytes += turn.text.size();
                    turns.push_back(turn);
                }
            }
            total++;
        }
        return total;
    }

    // Writes the chat again with turn `index` given a new text, left out (no text), or
    // left out with everything after it (`truncate`). The session Google keeps no longer
    // matches the chat then, so its handle goes: the next session starts from the text.
    // Returns how many turns are left, -1 if the card could not be written.
    static int rewrite(int id, int index, const std::string* text, bool truncate)
    {
        const std::string path = chat_path(id, "txt");
        const std::string temp_path = chat_path(id, "tmp");
        int kept = 0;
        bool ok = true;
        {
            Reader reader;
            std::unique_ptr<FIL> out(new FIL);
            if (!reader.open(path) || f_open(out.get(), temp_path.c_str(), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
                return -1;
            std::string line;
            Turn turn;
            int position = 0;
            while (ok && reader.read_line(line))
            {
                if (!parse_line(line, turn))
                    continue;
                const bool chosen = position++ == index;
                if (chosen && truncate)
                    break;
                if (chosen && !text)
                    continue;
                if (chosen)
                    turn.text = *text;
                line = std::string(turn.user ? "U " : "M ") + escape_line(turn.text) + "\n";
                UINT written = 0;
                ok = f_write(out.get(), line.data(), line.size(), &written) == FR_OK && written == line.size();
                kept++;
            }
            ok = (f_close(out.get()) == FR_OK) && ok;
        }

        if (!ok)
        {
            f_unlink(temp_path.c_str());
            return -1;
        }
        f_unlink(path.c_str());
        f_unlink(chat_path(id, "ses").c_str());
        if (kept == 0)
        {
            f_unlink(temp_path.c_str());
            return 0;
        }
        return f_rename(temp_path.c_str(), path.c_str()) == FR_OK ? kept : -1;
    }

    // A line of the screen, and the turn of the file it belongs to
    struct ViewLine
    {
        int turn;
        bool user;
        std::string text;
    };

    static void wrap_turns(LGFX_Sprite* canvas, const std::vector<Turn>& turns, int first, std::vector<ViewLine>& lines)
    {
        const int max_width = canvas->width() - VIEW_TEXT_PADDING * 2 - VIEW_SCROLLBAR_WIDTH;
        lines.clear();
        for (size_t t = 0; t < turns.size(); t++)
        {
            const int turn = first + (int)t;
            const size_t lines_before = lines.size();
            std::string current;
            for (char c : turns[t].text)
            {
                if (c == '\n')
                {
                    lines.push_back({turn, turns[t].user, current});
                    current.clear();
                    continue;
                }
                current += c;
                if (canvas->textWidth(current.c_str(), FONT_14) > max_width)
                {
                    const size_t last_space = current.find_last_of(' ');
                    if (last_space != std::string::npos)
                    {
                        lines.push_back({turn, turns[t].user, current.substr(0, last_space)});
                        current = current.substr(last_space + 1);
                    }
                    else
                    {
                        lines.push_back({turn, turns[t].user, current});
                        current.clear();
                    }
                }
            }
            if (!current.empty() || lines.size() == lines_before)
                lines.push_back({turn, turns[t].user, current});
        }
    }

    static int first_line_of(const std::vector<ViewLine>& lines, int turn)
    {
        for (size_t i = 0; i < lines.size(); i++)
        {
            if (lines[i].turn == turn)
                return (int)i;
        }
        return -1;
    }

    // The chat as on the chat screen. The turn at the top of the screen is the one that
    // is lit, and the one ENTER edits or deletes.
    static void render_chat(HAL::Hal* hal, const std::vector<ViewLine>& lines, int scroll)
    {
        auto* canvas = hal->canvas();
        canvas->fillScreen(THEME_COLOR_BG);
        canvas->setFont(FONT_14);
        const int line_height = canvas->fontHeight(FONT_14);
        const int visible = (canvas->height() - VIEW_TEXT_PADDING - HINT_HEIGHT) / line_height;
        const int selected_turn = lines[scroll].turn;

        for (int i = 0; i < visible && scroll + i < (int)lines.size(); i++)
        {
            const ViewLine& line = lines[scroll + i];
            const int y = VIEW_TEXT_PADDING + i * line_height;
            const bool lit = line.turn == selected_turn;
            if (line.user)
            {
                canvas->fillRect(0, y, VIEW_TEXT_PADDING - 1, line_height, lit ? TFT_WHITE : TFT_DARKGRAY);
                canvas->setTextColor(lit ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY);
            }
            else
            {
                canvas->fillRect(0, y, VIEW_TEXT_PADDING - 1, line_height, lit ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY);
                canvas->setTextColor(lit ? TFT_WHITE : TFT_DARKGRAY);
            }
            canvas->drawString(line.text.c_str(), VIEW_TEXT_PADDING, y);
        }

        UTILS::UI::draw_scrollbar(canvas,
                                  canvas->width() - VIEW_SCROLLBAR_WIDTH,
                                  VIEW_TEXT_PADDING,
                                  VIEW_SCROLLBAR_WIDTH - 1,
                                  line_height * visible,
                                  (int)lines.size() + visible - 1,
                                  visible,
                                  scroll);
    }

    // Returns true if the chat was changed
    static bool view_chat(HAL::Hal* hal, int id)
    {
        std::vector<Turn> turns;
        std::vector<ViewLine> lines;
        int first = 0;
        int total = 0;
        int scroll = 0;
        bool changed = false;

        // Puts the part of the chat starting at turn `from` on the screen, at the first
        // line of turn `show`, or at its last line when that turn did not fit in
        auto open_at = [&](int from, int show)
        {
            first = std::max(0, from);
            total = load_window(id, first, turns);
            wrap_turns(hal->canvas(), turns, first, lines);
            scroll = first_line_of(lines, show);
            if (scroll < 0)
                scroll = show < first ? 0 : (int)lines.size() - 1;
        };
        // the same, making sure that turn is on the screen
        auto show_turn = [&](int from, int show)
        {
            open_at(from, show);
            if (first_line_of(lines, show) < 0)
                open_at(show, show);
        };
        open_at(0, 0);

        bool need_render = true;
        bool is_repeat = false;
        uint32_t next_fire_ts = 0xFFFFFFFF;
        UTILS::HL_TEXT::HLTextContext_t hint_ctx;
        UTILS::HL_TEXT::hl_text_init(&hint_ctx, hal->canvas(), 20, 1500);

        while (!lines.empty())
        {
            bool update_needed = need_render;
            if (need_render)
            {
                render_chat(hal, lines, scroll);
                need_render = false;
            }
            update_needed |= UTILS::HL_TEXT::hl_text_render(&hint_ctx,
                                                            HINT_CHAT,
                                                            0,
                                                            hal->canvas()->height() - HINT_HEIGHT,
                                                            TFT_DARKGREY,
                                                            TFT_WHITE,
                                                            THEME_COLOR_BG);
            if (update_needed)
                hal->canvas_update();

            hal->keyboard()->updateKeyList();
            hal->keyboard()->updateKeysState();
            if (!hal->keyboard()->isPressed())
            {
                is_repeat = false;
                delay(10);
                continue;
            }

            const uint32_t now = millis();
            const int turn = lines[scroll].turn;
            const int last_loaded = first + (int)turns.size() - 1;
            if (hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
            {
                if (key_repeat_check(is_repeat, next_fire_ts, now))
                {
                    if (scroll < (int)lines.size() - 1)
                    {
                        scroll++;
                    }
                    else if (last_loaded < total - 1)
                    {
                        // the next part of the chat
                        open_at(turn + 1, turn + 1);
                    }
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_UP))
            {
                if (key_repeat_check(is_repeat, next_fire_ts, now))
                {
                    if (scroll > 0)
                    {
                        scroll--;
                    }
                    else if (first > 0)
                    {
                        // an earlier part of the chat, shown at the line above this one
                        const int old_first = first;
                        open_at(old_first - VIEW_STEP_BACK, turn);
                        if (first_line_of(lines, turn) < 0)
                            open_at(old_first - 1, turn);
                        else if (scroll > 0)
                            scroll--;
                    }
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
            {
                if (key_repeat_check(is_repeat, next_fire_ts, now) && turn < total - 1)
                {
                    if (turn < last_loaded)
                        scroll = first_line_of(lines, turn + 1);
                    else
                        open_at(turn + 1, turn + 1);
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
            {
                if (key_repeat_check(is_repeat, next_fire_ts, now))
                {
                    // to the start of this turn, then to the one before
                    const int start = first_line_of(lines, turn);
                    if (scroll > start)
                        scroll = start;
                    else if (turn > first)
                        scroll = first_line_of(lines, turn - 1);
                    else if (turn > 0)
                        show_turn(turn - VIEW_STEP_BACK, turn - 1);
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
            {
                hal->playNextSound();
                hal->keyboard()->waitForRelease(hal->keyboard()->isKeyPressing(KEY_NUM_ESC) ? KEY_NUM_ESC : KEY_NUM_BACKSPACE);
                break;
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
            {
                hal->playNextSound();
                hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
                const Turn& current = turns[turn - first];
                const std::vector<std::string> actions = {"Edit", "Delete", "Delete from here on"};
                const int action = UTILS::UI::show_select_dialog(hal, current.user ? "Your message" : "Gemini's reply", actions);

                int left = -2;
                if (action == 0)
                {
                    // the editor has one line
                    std::string text = current.text;
                    std::replace(text.begin(), text.end(), '\n', ' ');
                    const std::string before = text;
                    const int max_length = std::max((int)text.size() + VIEW_EDIT_ROOM, VIEW_EDIT_MIN);
                    if (UTILS::UI::show_edit_string_dialog(hal, current.user ? "You" : "Gemini", text, false, max_length) && text != before &&
                        text.find_first_not_of(' ') != std::string::npos)
                        left = rewrite(id, turn, &text, false);
                }
                else if (action == 1)
                {
                    if (UTILS::UI::show_confirmation_dialog(hal, "Delete", "Delete this message from the chat?", "Delete"))
                        left = rewrite(id, turn, nullptr, false);
                }
                else if (action == 2)
                {
                    if (UTILS::UI::show_confirmation_dialog(hal, "Delete", "Delete this message and all that follow it?", "Delete"))
                        left = rewrite(id, turn, nullptr, true);
                }

                if (left == -1)
                {
                    UTILS::UI::show_error_dialog(hal, "Error", "Could not write to the SD card", "OK");
                }
                else if (left == 0)
                {
                    changed = true;
                    UTILS::UI::show_message_dialog(hal, "Chat", "Nothing left: chat deleted", 0);
                    break;
                }
                else if (left > 0)
                {
                    changed = true;
                    const int show = std::min(turn, left - 1);
                    show_turn(show < first ? show : first, show);
                }
                need_render = true;
            }
            delay(10);
        }

        UTILS::HL_TEXT::hl_text_free(&hint_ctx);
        return changed;
    }

    // ── The chat list ──

    // The same list serves three purposes. Its first row is not a chat: it starts a new
    // one, or forgets them all. The list to read chats from has no such row.
    enum ListMode
    {
        LIST_PICK,
        LIST_VIEW,
        LIST_FORGET,
    };

    static void render(HAL::Hal* hal, const std::vector<Entry>& entries, ListMode mode, int selected, int scroll_offset)
    {
        static const char* const TITLES[] = {"Chats", "View chats", "Forget chats"};
        auto* canvas = hal->canvas();
        canvas->fillScreen(THEME_COLOR_BG);
        canvas->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
        canvas->setFont(FONT_16);
        canvas->drawString(TITLES[mode], 5, 0);

        const int first_rows = mode == LIST_VIEW ? 0 : 1;
        const int count = (int)entries.size() + first_rows;
        const int max_width = canvas->width() - 5 - 6;
        const int line_height = canvas->fontHeight(FONT_16) + 2 + 1;
        const int max_visible_lines = (canvas->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
        int y_offset = ITEMS_Y_OFFSET;

        for (int i = scroll_offset; i < count && i < scroll_offset + max_visible_lines; i++)
        {
            const bool is_chat = i >= first_rows;
            if (i == selected)
            {
                canvas->fillRect(5, y_offset + 1, max_width, line_height - 1, THEME_COLOR_BG_SELECTED);
                canvas->setTextColor(TFT_BLACK, THEME_COLOR_BG_SELECTED);
            }
            else
            {
                canvas->setTextColor(is_chat ? TFT_CYAN : TFT_YELLOW, THEME_COLOR_BG);
            }

            // cut to the width of the row, on a whole character
            std::string label = is_chat ? entries[i - first_rows].title : mode == LIST_FORGET ? "Forget all chats" : "+ New chat";
            while (!label.empty() && canvas->textWidth(label.c_str(), FONT_16) > max_width - 10)
            {
                uint8_t removed;
                do
                {
                    removed = (uint8_t)label.back();
                    label.pop_back();
                } while (!label.empty() && (removed & 0xC0) == 0x80);
            }
            canvas->drawString(label.c_str(), 10, y_offset + 1);
            y_offset += line_height;
        }

        UTILS::UI::draw_scrollbar(canvas,
                                  canvas->width() - 6 - 1,
                                  ITEMS_Y_OFFSET,
                                  6,
                                  line_height * max_visible_lines,
                                  count,
                                  max_visible_lines,
                                  scroll_offset);
    }

    // Runs the list until a chat is opened (its id), a new one is asked for (0), or the
    // user leaves or nothing is left to show (-1). Only LIST_PICK opens a chat.
    static int run_list(HAL::Hal* hal, std::vector<Entry>& entries, ListMode mode)
    {
        static const char* const HINTS[] = {HINT_PICK, HINT_VIEW, HINT_FORGET};
        const int first_rows = mode == LIST_VIEW ? 0 : 1;
        const int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
        const int max_visible_lines = (hal->canvas()->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
        // the most recent chat rather than the row above it
        int selected = first_rows;
        int scroll_offset = 0;
        int result = -1;
        bool need_render = true;
        bool is_repeat = false;
        uint32_t next_fire_ts = 0xFFFFFFFF;

        UTILS::HL_TEXT::HLTextContext_t hint_ctx;
        UTILS::HL_TEXT::hl_text_init(&hint_ctx, hal->canvas(), 20, 1500);

        while (true)
        {
            const int count = (int)entries.size() + first_rows;
            bool update_needed = need_render;
            if (need_render)
            {
                render(hal, entries, mode, selected, scroll_offset);
                need_render = false;
            }
            update_needed |= UTILS::HL_TEXT::hl_text_render(&hint_ctx,
                                                            HINTS[mode],
                                                            0,
                                                            hal->canvas()->height() - HINT_HEIGHT,
                                                            TFT_DARKGREY,
                                                            TFT_WHITE,
                                                            THEME_COLOR_BG);
            if (update_needed)
                hal->canvas_update();

            hal->keyboard()->updateKeyList();
            hal->keyboard()->updateKeysState();
            if (!hal->keyboard()->isPressed())
            {
                is_repeat = false;
                delay(10);
                continue;
            }

            const uint32_t now = millis();
            const bool on_chat = selected >= first_rows;
            if (hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
            {
                if (key_repeat_check(is_repeat, next_fire_ts, now) && selected < count - 1)
                {
                    hal->playNextSound();
                    selected++;
                    if (selected >= scroll_offset + max_visible_lines)
                        scroll_offset = selected - max_visible_lines + 1;
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_UP))
            {
                if (key_repeat_check(is_repeat, next_fire_ts, now) && selected > 0)
                {
                    hal->playNextSound();
                    selected--;
                    if (selected < scroll_offset)
                        scroll_offset = selected;
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
            {
                hal->playNextSound();
                hal->keyboard()->waitForRelease(KEY_NUM_ESC);
                break;
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) || hal->keyboard()->isKeyPressing(KEY_NUM_ENTER) ||
                     hal->keyboard()->isKeyPressing(KEY_NUM_TAB))
            {
                const uint8_t key = hal->keyboard()->isKeyPressing(KEY_NUM_ENTER) ? KEY_NUM_ENTER
                                    : hal->keyboard()->isKeyPressing(KEY_NUM_TAB) ? KEY_NUM_TAB
                                                                                  : KEY_NUM_BACKSPACE;
                hal->playNextSound();
                hal->keyboard()->waitForRelease(key);

                const bool forget_key = key == KEY_NUM_BACKSPACE || (key == KEY_NUM_ENTER && mode == LIST_FORGET);
                const bool view_key = (key == KEY_NUM_TAB && mode != LIST_FORGET) || (key == KEY_NUM_ENTER && mode == LIST_VIEW);
                if (key == KEY_NUM_ENTER && mode == LIST_PICK)
                {
                    result = on_chat ? entries[selected - first_rows].id : 0;
                    break;
                }

                if (view_key && on_chat)
                {
                    // an edit can change how the chat is listed, or delete it
                    if (view_chat(hal, entries[selected - first_rows].id))
                        entries = list();
                }
                else if (forget_key && on_chat)
                {
                    if (UTILS::UI::show_confirmation_dialog(hal, "Forget chat", entries[selected - first_rows].title, "Forget"))
                    {
                        remove(entries[selected - first_rows].id);
                        entries.erase(entries.begin() + selected - first_rows);
                    }
                }
                else if (key == KEY_NUM_ENTER && mode == LIST_FORGET &&
                         UTILS::UI::show_confirmation_dialog(hal,
                                                             "Forget all",
                                                             "Delete all " + std::to_string(entries.size()) + " saved chats?",
                                                             "Forget"))
                {
                    for (const auto& entry : entries)
                        remove(entry.id);
                    ESP_LOGI(TAG, "Forgot %d chats", (int)entries.size());
                    entries.clear();
                }
                if (entries.empty())
                    break;
                selected = std::min(selected, (int)entries.size() + first_rows - 1);
                scroll_offset = std::max(0, std::min(scroll_offset, (int)entries.size() + first_rows - max_visible_lines));
                need_render = true;
            }
            delay(10);
        }

        UTILS::HL_TEXT::hl_text_free(&hint_ctx);
        return result;
    }

    int pick(HAL::Hal* hal)
    {
        std::vector<Entry> entries = list();
        if (entries.empty())
            return 0;
        return run_list(hal, entries, LIST_PICK);
    }

    static void manage(HAL::Hal* hal, ListMode mode, const char* title)
    {
        std::vector<Entry> entries = list();
        if (entries.empty())
        {
            UTILS::UI::show_message_dialog(hal, title, "No saved chats", 0);
            return;
        }
        run_list(hal, entries, mode);
        if (entries.empty())
            UTILS::UI::show_message_dialog(hal, title, "No saved chats left", 0);
    }

    void view(HAL::Hal* hal) { manage(hal, LIST_VIEW, "View chats"); }

    void forget(HAL::Hal* hal) { manage(hal, LIST_FORGET, "Forget chats"); }
} // namespace CHAT_STORE
#endif
