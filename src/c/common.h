#pragma once
#include <pebble.h>

#define APP_VERSION "1.0"

// ---- Audio --------------------------------------------------------------------------------------
// Audio is stored as IMA ADPCM in 256-byte blocks, the same block layout a WAV file uses
// (format 0x11, block align 256): a 4-byte header with the first sample and step index, then
// 504 four-bit samples. Every block decodes on its own, so playback can seek to any block.
#define ADPCM_BLOCK_BYTES    256
#define ADPCM_BLOCK_SAMPLES  505
// Bookkeeping the settings file adds per key, measured on firmware 4.44 (3912 x 256 B in 1 MB).
#define PERSIST_KEY_OVERHEAD 12
#define BLOCK_COST           (ADPCM_BLOCK_BYTES + PERSIST_KEY_OVERHEAD)
// Block numbers are 12 bits of the persist key.
#define MAX_BLOCKS_PER_REC   4095

typedef enum { QualityStandard = 0, QualityHigh = 1, QualityCount } Quality;
typedef enum { TargetWatch = 0, TargetPhone = 1, TargetCount } Target;

static inline uint32_t rate_for_code(uint8_t code) { return code ? 16000 : 8000; }
// Stored ADPCM bytes per second for a rate code (header bytes included).
static inline uint32_t adpcm_bytes_per_sec(uint8_t code) {
  return (rate_for_code(code) * ADPCM_BLOCK_BYTES + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES;
}

// ---- Persist keys -------------------------------------------------------------------------------
#define PKEY_SETTINGS      1
#define PKEY_DELETE_QUEUE  2
#define PKEY_META_BASE     0x100
#define PKEY_AUDIO(id, block) (0x10000000u | ((uint32_t)(id) << 12) | (uint32_t)(block))

// ---- Phone capabilities (sent by the phone in its hello) ---------------------------------------
#define CAP_TRANSCRIBE 0x01
#define CAP_WEBDAV     0x02

// ---- Platform sizing ----------------------------------------------------------------------------
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  #define BIG_SCREEN 1
#else
  #define BIG_SCREEN 0
#endif

#define ACCENT_COLOR     PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
#define ACCENT_DARK      PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack)

// A monotonic-enough millisecond clock for measuring intervals.
static inline int32_t now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (int32_t)((s % 1000000) * 1000 + ms);
}
