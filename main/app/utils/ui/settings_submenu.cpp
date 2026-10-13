/**
 * @file settings_submenu.cpp
 * @brief A list of settings opened from a row of the settings screen
 *
 * The settings screen knows two levels, groups and their items. This adds a third as
 * a screen of its own, so that the two levels stay as they are for the StickS3.
 */

// nothing else ahead of the guard: without an LED this file has to compile to nothing
#include "hal_config.h"
#if HAL_USE_LED
#include "settings_submenu.h"
#include "settings_screen.h"
#include "../theme/theme_define.h"
#include "draw_helper.h"
#include "key_repeat.h"
#include "../anim/hl_text.h"
#include "common_define.h"

#define HINT_HEIGHT 12
#define ITEMS_Y_OFFSET 20

static const char* HINT_ITEMS = "[\xE2\x86\x91][\xE2\x86\x93] [ESC] [ENTER]";

namespace UTILS
{
    namespace UI
    {
        static void render(HAL::Hal* hal, SETTINGS::SettingGroup_t& group, int selected, int scroll_offset)
        {
            auto* canvas = hal->canvas();
            canvas->fillScreen(THEME_COLOR_BG);
            canvas->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
            canvas->setFont(FONT_16);
            canvas->drawString(group.name.c_str(), 5, 0);

            const int max_width = canvas->width() - 5 - 6;
            const int line_height = canvas->fontHeight(FONT_16) + 2 + 1;
            const int max_visible_lines = (canvas->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
            int y_offset = ITEMS_Y_OFFSET;

            for (int i = scroll_offset; i < (int)group.items.size() && i < scroll_offset + max_visible_lines; i++)
            {
                auto& item = group.items[i];
                const bool is_back = item.type == SETTINGS::TYPE_NONE;

                if (i == selected)
                {
                    canvas->fillRect(5, y_offset + 1, max_width, line_height - 1, THEME_COLOR_BG_SELECTED);
                    canvas->setTextColor(TFT_BLACK, THEME_COLOR_BG_SELECTED);
                }
                else
                {
                    canvas->setTextColor(is_back ? TFT_YELLOW : TFT_CYAN, THEME_COLOR_BG);
                }
                canvas->drawString(item.label.c_str(), 10, y_offset + 1);

                switch (item.type)
                {
                case SETTINGS::TYPE_BOOL:
                    item.value = hal->settings()->getBool(group.nvs_namespace, item.key) ? "true" : "false";
                    break;
                case SETTINGS::TYPE_NUMBER:
                    item.value = std::to_string(hal->settings()->getNumber(group.nvs_namespace, item.key));
                    break;
                case SETTINGS::TYPE_STRING:
                    item.value = hal->settings()->getString(group.nvs_namespace, item.key);
                    break;
                default:
                    break;
                }
                if (!is_back)
                {
                    std::string display_value = item.value;
                    if (item.type == SETTINGS::TYPE_NUMBER && item.percent)
                        display_value = std::to_string(SETTINGS::number_to_percent(item, std::stoi(item.value))) + "%";
                    if (i == selected)
                        canvas->setTextColor(TFT_BLACK, THEME_COLOR_BG_SELECTED);
                    else
                        canvas->setTextColor(TFT_WHITE, THEME_COLOR_BG);
                    canvas->drawRightString(display_value.c_str(), canvas->width() - 10, y_offset + 1);
                }
                y_offset += line_height;
            }

            draw_scrollbar(canvas,
                           canvas->width() - 6 - 1,
                           ITEMS_Y_OFFSET,
                           6,
                           line_height * max_visible_lines,
                           (int)group.items.size(),
                           max_visible_lines,
                           scroll_offset);
        }

        void show_settings_submenu(HAL::Hal* hal, SETTINGS::SettingGroup_t& group)
        {
            if (group.items.empty())
                return;

            const int count = (int)group.items.size();
            const int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
            const int max_visible_lines = (hal->canvas()->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
            // the first setting rather than the back row
            int selected = count > 1 ? 1 : 0;
            int scroll_offset = 0;
            bool need_render = true;
            bool is_repeat = false;
            uint32_t next_fire_ts = 0xFFFFFFFF;

            HL_TEXT::HLTextContext_t hint_ctx;
            HL_TEXT::hl_text_init(&hint_ctx, hal->canvas(), 20, 1500);

            while (true)
            {
                bool update_needed = need_render;
                if (need_render)
                {
                    render(hal, group, selected, scroll_offset);
                    need_render = false;
                }
                update_needed |= HL_TEXT::hl_text_render(&hint_ctx,
                                                         HINT_ITEMS,
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
                    auto& item = group.items[selected];
                    if (item.type == SETTINGS::TYPE_NONE)
                        break;
                    SETTINGS_SCREEN::handle_setting_change(hal, group, item);
                    if (item.callback)
                        item.callback(item);
                    need_render = true;
                }
                delay(10);
            }

            HL_TEXT::hl_text_free(&hint_ctx);
        }
    } // namespace UI
} // namespace UTILS
#endif
