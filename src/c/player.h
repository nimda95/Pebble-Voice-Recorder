#pragma once
#include "common.h"

typedef enum {
  PlayerStopped = 0,
  PlayerPlaying,
  PlayerPaused,
  PlayerBuffering,
  PlayerEnded,
} PlayerState;

typedef void (*PlayerListener)(void);

// True on watches with a speaker.
bool player_available(void);
// The system mutes the speaker in Settings or during Quiet Time; apps can't override it.
bool player_muted(void);

// Prepares a recording: its watch copy, or the phone copy streamed over Bluetooth.
bool player_open(uint16_t id);
void player_close(void);

void player_play(void);
void player_pause(void);
void player_toggle(void);
void player_seek_ms(uint32_t ms);
void player_skip_ms(int32_t delta_ms);

uint32_t player_position_ms(void);
uint32_t player_duration_ms(void);
PlayerState player_state(void);
const char *player_error(void);

void player_set_listener(PlayerListener listener);

void player_on_remote_data(uint16_t id, uint16_t block, const uint8_t *data, uint16_t len);
void player_on_remote_error(uint16_t id, const char *reason);
