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

    // What is kept in memory of a chat, to show it and to hand to Gemini
    static constexpr size_t MAX_TURNS = 40;
    static constexpr size_t MAX_TURNS_BYTES = 4000;

    // Mounts the card for the functions below, and makes the folders. False without a
    // usable card. This is a mount of its own, not SDCard::mount(): see chat_store.cpp.
    bool mount(HAL::Hal* hal);
    void unmount(HAL::Hal* hal);

    // Drops the oldest turns until what is left fits the limits above
    void trim(std::vector<Turn>& turns);

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

    // The same list, to read chats from, and to edit or delete their messages. TAB
    // does the same from the list of pick().
    void view(HAL::Hal* hal);
    // The same list, to delete chats from: one at a time, or all of them
    void forget(HAL::Hal* hal);

    // The message that gives a new Gemini session the end of an earlier conversation.
    // Empty when there is nothing to give.
    std::string history_message(const std::vector<Turn>& turns);
} // namespace CHAT_STORE
#endif
