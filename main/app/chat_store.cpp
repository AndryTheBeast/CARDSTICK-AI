/**
 * @file chat_store.cpp
 * @brief Chats kept on the SD card, so that one can be continued after a restart
 *
 * One text file per chat in /M5Gemini/chats, a line per turn: "U " and what the user
 * said or typed, "M " and what Gemini answered. Beside it, while Google still honours
 * it, the handle that resumes the session itself.
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
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* TAG = "CHAT_STORE";
static const char* APP_DIR = "/sdcard/M5Gemini";
static const char* CHATS_DIR = "/sdcard/M5Gemini/chats";

#define MAX_CHAT_ID 9999
#define MAX_LISTED_CHATS 200
#define MAX_TITLE_BYTES 80
// What is read back of a chat, to show it and to hand to Gemini
#define MAX_LOADED_TURNS 60
// What a new session is given of the conversation so far. It is paid for as input
// tokens every time a session starts, hence the limits.
#define MAX_HISTORY_TURNS 40
#define MAX_HISTORY_BYTES 6000

#define HINT_HEIGHT 12
#define ITEMS_Y_OFFSET 20

static const char* HINT_LIST = "[\xE2\x86\x91][\xE2\x86\x93] [ENTER] OPEN [DEL] FORGET [ESC]";
static const char* NEW_CHAT_LABEL = "+ New chat";

namespace CHAT_STORE
{
    struct Entry
    {
        int id;
        std::string title;
    };

    static std::string chat_path(int id, const char* extension)
    {
        char path[48];
        snprintf(path, sizeof(path), "%s/%04d.%s", CHATS_DIR, id, extension);
        return path;
    }

    static bool read_line(FILE* file, std::string& line)
    {
        line.clear();
        int c;
        while ((c = fgetc(file)) != EOF)
        {
            if (c == '\n')
                return true;
            if (c != '\r')
                line += (char)c;
        }
        return !line.empty();
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
        DIR* dir = opendir(CHATS_DIR);
        if (!dir)
            return ids;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            const char* dot = strrchr(entry->d_name, '.');
            if (!dot || strcasecmp(dot, ".txt") != 0)
                continue;
            const int id = atoi(entry->d_name);
            if (id > 0 && id <= MAX_CHAT_ID)
                ids.push_back(id);
        }
        closedir(dir);
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
            FILE* file = fopen(chat_path(id, "txt").c_str(), "r");
            if (file)
            {
                std::string line;
                Turn turn;
                if (read_line(file, line) && parse_line(line, turn))
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
                fclose(file);
            }
            if (entry.title.empty())
                entry.title = "Chat " + std::to_string(id);
            entries.push_back(entry);
        }
        return entries;
    }

    static void remove(int id)
    {
        unlink(chat_path(id, "txt").c_str());
        unlink(chat_path(id, "ses").c_str());
    }

    bool mount(HAL::Hal* hal)
    {
        if (!hal->sdcard()->is_mounted() && !hal->sdcard()->mount(false))
            return false;
        mkdir(APP_DIR, 0775);
        mkdir(CHATS_DIR, 0775);
        struct stat st;
        return stat(CHATS_DIR, &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool load(int id, std::vector<Turn>& turns, std::string& handle)
    {
        turns.clear();
        handle.clear();
        FILE* file = fopen(chat_path(id, "txt").c_str(), "r");
        if (!file)
            return false;
        std::string line;
        Turn turn;
        while (read_line(file, line))
        {
            if (!parse_line(line, turn))
                continue;
            if (turns.size() >= MAX_LOADED_TURNS)
                turns.erase(turns.begin());
            turns.push_back(turn);
        }
        fclose(file);

        file = fopen(chat_path(id, "ses").c_str(), "r");
        if (file)
        {
            read_line(file, handle);
            fclose(file);
        }
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
        FILE* file = fopen(chat_path(id, "txt").c_str(), "a");
        if (!file)
        {
            ESP_LOGE(TAG, "Cannot write chat %d", id);
            return false;
        }
        const bool written = fprintf(file, "%c %s\n", turn.user ? 'U' : 'M', escape_line(turn.text).c_str()) > 0;
        return (fclose(file) == 0) && written;
    }

    void save_handle(int id, const std::string& handle)
    {
        const std::string path = chat_path(id, "ses");
        if (handle.empty())
        {
            unlink(path.c_str());
            return;
        }
        FILE* file = fopen(path.c_str(), "w");
        if (!file)
            return;
        fputs(handle.c_str(), file);
        fclose(file);
    }

    int remove_all()
    {
        const std::vector<int> ids = list_ids();
        for (int id : ids)
            remove(id);
        ESP_LOGI(TAG, "Forgot %d chats", (int)ids.size());
        return (int)ids.size();
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
        // as many of the last turns as fit, starting on one of the user's
        size_t first = turns.size();
        size_t bytes = 0;
        while (first > 0 && turns.size() - first < MAX_HISTORY_TURNS && bytes + turns[first - 1].text.size() <= MAX_HISTORY_BYTES)
        {
            first--;
            bytes += turns[first].text.size();
        }
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

    static void render(HAL::Hal* hal, const std::vector<Entry>& entries, int selected, int scroll_offset)
    {
        auto* canvas = hal->canvas();
        canvas->fillScreen(THEME_COLOR_BG);
        canvas->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
        canvas->setFont(FONT_16);
        canvas->drawString("Chats", 5, 0);

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
            std::string label = i == 0 ? NEW_CHAT_LABEL : entries[i - 1].title;
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

    int pick(HAL::Hal* hal)
    {
        std::vector<Entry> entries = list();
        if (entries.empty())
            return 0;

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
                render(hal, entries, selected, scroll_offset);
                need_render = false;
            }
            update_needed |= UTILS::HL_TEXT::hl_text_render(&hint_ctx,
                                                            HINT_LIST,
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
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
            {
                hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
                if (selected > 0)
                {
                    hal->playNextSound();
                    if (UTILS::UI::show_confirmation_dialog(hal, "Forget chat", entries[selected - 1].title, "Forget"))
                    {
                        remove(entries[selected - 1].id);
                        entries.erase(entries.begin() + selected - 1);
                        if (entries.empty())
                            break;
                        selected = std::min(selected, (int)entries.size());
                        scroll_offset = std::max(0, std::min(scroll_offset, (int)entries.size() + 1 - max_visible_lines));
                    }
                    need_render = true;
                }
            }
            else if (hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
            {
                hal->playNextSound();
                hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
                result = selected == 0 ? 0 : entries[selected - 1].id;
                break;
            }
            delay(10);
        }

        UTILS::HL_TEXT::hl_text_free(&hint_ctx);
        return result;
    }
} // namespace CHAT_STORE
#endif
