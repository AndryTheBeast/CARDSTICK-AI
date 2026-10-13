/**
 * @file chat_store.h
 * @brief Chats kept on the SD card, so that one can be continued after a restart
 */
#pragma once

#include "hal_config.h"
#if HAL_USE_SDCARD
#include "hal.h"
#include <string>
#include <vector>

namespace CHAT_STORE
{
    struct Turn
    {
        bool user;
        std::string text;
    };

    // Mounts the card if it is not, and makes the folders. False without a usable card.
    bool mount(HAL::Hal* hal);

    // The chat list: returns the id of the chat to continue, 0 for a new chat and -1 if
    // the user went back. Returns 0 straight away when nothing is saved yet.
    int pick(HAL::Hal* hal);

    // The last turns of a chat and the session handle saved with it, if any
    bool load(int id, std::vector<Turn>& turns, std::string& handle);

    // An id for a new chat, 0 if there is no room for one
    int create();
    bool append(int id, const Turn& turn);
    // An empty handle removes the saved one
    void save_handle(int id, const std::string& handle);

    // Returns how many chats were deleted
    int remove_all();

    // The message that gives a new Gemini session the end of an earlier conversation.
    // Empty when there is nothing to give.
    std::string history_message(const std::vector<Turn>& turns);
} // namespace CHAT_STORE
#endif
