#pragma once
#include "common.h"

typedef enum { RecIdle = 0, RecRecording = 1, RecPaused = 2, RecStarting = 3 } RecState;
typedef enum { ModeLocal = 0, ModeStream = 1 } RecMode;
typedef enum { HealthGood = 0, HealthWarn = 1, HealthBad = 2 } HealthLevel;
typedef enum {
  HrIdle = 0,
  HrGood,
  HrSlowStorage,
  HrStorageLow,
  HrDropping,
  HrPhoneLag,
  HrPhoneSilent,
  HrConnecting,
  HrPaused,
} HealthReason;

typedef void (*RecorderListener)(void);

void recorder_init(void);
// Saves a recording in progress.
void recorder_deinit(void);

// Starts a recording on the chosen target. Returns false and sets recorder_error() on failure.
bool recorder_start(void);
void recorder_pause(void);
void recorder_resume(void);
void recorder_stop_save(void);
void recorder_discard(void);

RecState recorder_state(void);
RecMode recorder_mode(void);
uint16_t recorder_current_id(void);
uint32_t recorder_elapsed_ms(void);
uint32_t recorder_lost_ms(void);

HealthLevel recorder_health(HealthReason *reason);
const char *recorder_health_text(HealthReason reason);

// Space and time left on whichever target the current (or next) recording uses.
bool recorder_uses_phone(void);
uint32_t recorder_free_bytes(void);
uint32_t recorder_seconds_left(void);

const char *recorder_error(void);
// A one-off message for the recorder screen, e.g. after falling back to the watch. Read once.
const char *recorder_take_notice(void);

void recorder_set_listener(RecorderListener listener);
void recorder_on_stream_stat(uint16_t id, uint32_t received_ms);
