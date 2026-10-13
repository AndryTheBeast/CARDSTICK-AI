/**
 * @file settings_submenu.h
 * @brief A list of settings opened from a row of the settings screen
 */

#pragma once

#include "hal.h"
#include "../../../settings/settings.h"

namespace UTILS
{
    namespace UI
    {
        /**
         * @brief Show the items of a group and let the user change them
         *
         * Blocks until the user goes back. Changes are saved as they are made.
         */
        void show_settings_submenu(HAL::Hal* hal, SETTINGS::SettingGroup_t& group);
    } // namespace UI
} // namespace UTILS
