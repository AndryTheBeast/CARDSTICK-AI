#pragma once

#include <stdint.h>
#include "sdkconfig.h"

#define SYSTEM_CHANNEL 0
#define AUDIO_CHANNEL 1
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1

// Shared static buffer used by the S2S speaker ring buffer.
// Sized for ~0.67s of 24kHz 16-bit mono output.
// (portMAX_DELAY backpressure prevents data loss even with smaller buffer)
#ifdef CONFIG_BOARD_STICKS3
#define AUDIO_BUFFER_SIZE (1024 * 32)
#else
// Half of that on the Cardputer, ~0.33s. It has no more RAM than the StickS3 and more
// to fit in it: the 16 KB go to the TLS buffers, without which a long reply or a voice
// turn loses the connection.
#define AUDIO_BUFFER_SIZE (1024 * 16)
#endif

extern uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
