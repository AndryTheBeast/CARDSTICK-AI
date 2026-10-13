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

// the SD card slot of the Cardputer, as in hal/sdcard
#define PIN_NUM_MISO 39
#define PIN_NUM_MOSI 14
#define PIN_NUM_CLK 40
#define PIN_NUM_CS GPIO_NUM_12

#define MAX_CHAT_ID 9999
#define MAX_LISTED_CHATS 200
#define MAX_TITLE_BYTES 80

#define HINT_HEIGHT 12
#define ITEMS_Y_OFFSET 20

static const char* HINT_PICK = "[\xE2\x86\x91][\xE2\x86\x93] [ENTER] OPEN [DEL] FORGET [ESC]";
static const char* HINT_FORGET = "[\xE2\x86\x91][\xE2\x86\x93] [ENTER] FORGET [ESC] BACK";

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
        esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
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
        ESP_LOGI(TAG, "Card mounted, heap=%d", (int)esp_get_free_heap_size());
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

    // ── The chat list ──

    // The first row is not a chat: it starts a new one, or forgets them all
    static void render(HAL::Hal* hal, const std::vector<Entry>& entries, bool forgetting, int selected, int scroll_offset)
    {
        auto* canvas = hal->canvas();
        canvas->fillScreen(THEME_COLOR_BG);
        canvas->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
        canvas->setFont(FONT_16);
        canvas->drawString(forgetting ? "Forget chats" : "Chats", 5, 0);

        const int count = (int)entries.size() + 1;
        const int max_width = canvas->width() - 5 - 6;
        const int line_height = canvas->fontHeight(FONT_16) + 2 + 1;
        const int max_visible_lines = (canvas->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
        int y_offset = ITEMS_Y_OFFSET;

        for (int i = scroll_offset; i < count && i < scroll_offset + max_visible_lines; i++)
        {
            if (i == selected)
            {
                canvas->fillRect(5, y_offset + 1, max_width, line_height - 1, THEME_COLOR_BG_SELECTED);
                canvas->setTextColor(TFT_BLACK, THEME_COLOR_BG_SELECTED);
            }
            else
            {
                canvas->setTextColor(i == 0 ? TFT_YELLOW : TFT_CYAN, THEME_COLOR_BG);
            }

            // cut to the width of the row, on a whole character
            std::string label = i > 0 ? entries[i - 1].title : forgetting ? "Forget all chats" : "+ New chat";
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
    // user leaves or nothing is left to show (-1). When forgetting it only ever leaves.
    static int run_list(HAL::Hal* hal, std::vector<Entry>& entries, bool forgetting)
    {
        const int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
        const int max_visible_lines = (hal->canvas()->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
        // the most recent chat rather than the new one
        int selected = 1;
        int scroll_offset = 0;
        int result = -1;
        bool need_render = true;
        bool is_repeat = false;
        uint32_t next_fire_ts = 0xFFFFFFFF;

        UTILS::HL_TEXT::HLTextContext_t hint_ctx;
        UTILS::HL_TEXT::hl_text_init(&hint_ctx, hal->canvas(), 20, 1500);

        while (true)
        {
            const int count = (int)entries.size() + 1;
            bool update_needed = need_render;
            if (need_render)
            {
                render(hal, entries, forgetting, selected, scroll_offset);
                need_render = false;
            }
            update_needed |= UTILS::HL_TEXT::hl_text_render(&hint_ctx,
                                                            forgetting ? HINT_FORGET : HINT_PICK,
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
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) || hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
            {
                const bool enter = hal->keyboard()->isKeyPressing(KEY_NUM_ENTER);
                hal->playNextSound();
                hal->keyboard()->waitForRelease(enter ? KEY_NUM_ENTER : KEY_NUM_BACKSPACE);
                if (enter && !forgetting)
                {
                    result = selected == 0 ? 0 : entries[selected - 1].id;
                    break;
                }

                if (selected > 0)
                {
                    if (UTILS::UI::show_confirmation_dialog(hal, "Forget chat", entries[selected - 1].title, "Forget"))
                    {
                        remove(entries[selected - 1].id);
                        entries.erase(entries.begin() + selected - 1);
                    }
                }
                else if (enter && UTILS::UI::show_confirmation_dialog(hal,
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
                selected = std::min(selected, (int)entries.size());
                scroll_offset = std::max(0, std::min(scroll_offset, (int)entries.size() + 1 - max_visible_lines));
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
        return run_list(hal, entries, false);
    }

    void forget(HAL::Hal* hal)
    {
        std::vector<Entry> entries = list();
        if (entries.empty())
        {
            UTILS::UI::show_message_dialog(hal, "Forget chats", "No saved chats", 0);
            return;
        }
        run_list(hal, entries, true);
        if (entries.empty())
            UTILS::UI::show_message_dialog(hal, "Forget chats", "No saved chats left", 0);
    }
} // namespace CHAT_STORE
#endif
