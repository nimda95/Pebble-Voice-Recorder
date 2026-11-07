#pragma once
#include "common.h"

#define LANG_CODE_LEN 6

typedef struct __attribute__((packed)) {
  uint8_t version;
  uint8_t quality;        // Quality
  uint8_t target;         // Target
  uint8_t ql_autostart;   // start recording when opened with Quick Launch
  char lang[LANG_CODE_LEN];  // last language picked for transcription
  uint8_t caps;           // CAP_* from the phone
  uint8_t dirty;          // changed on the watch since the phone last heard
  uint16_t next_id;
  uint32_t phone_free;    // ADPCM bytes the phone can still store
  uint32_t phone_used;
  // Version 2
  uint8_t gain;           // MicGain
  uint8_t volume;         // playback volume, 0-100 (watch only)
  char tlang[LANG_CODE_LEN];  // last language picked for translation
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Marks a watch-side change so it is sent to the phone.
void settings_changed_on_watch(void);
