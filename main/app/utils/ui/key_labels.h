/**
 * @file key_labels.h
 * @brief Names of the ESC and TAB keys in the on-screen hints
 *
 * The T-Deck has neither key: ESC is a held trackball click and TAB is Alt+mic.
 */
#pragma once
#include "sdkconfig.h"

#ifdef CONFIG_BOARD_TDECK
#define KEY_LABEL_ESC "[HOLD]"
#define KEY_LABEL_TAB "[ALT+MIC]"
#else
#define KEY_LABEL_ESC "[ESC]"
#define KEY_LABEL_TAB "[TAB]"
#endif
