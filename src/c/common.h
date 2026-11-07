#pragma once
#include <pebble.h>

#define APP_VERSION "1.1"

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

// Mic gain steps, applied to the samples before encoding (the SDK has no hardware gain).
typedef enum { GainMinus6 = 0, Gain0, GainPlus6, GainPlus12, GainPlus18, GainCount } MicGain;
#define GAIN_DEFAULT Gain0
// Gain in Q8 (256 = 1x) for each step.
static inline int32_t gain_q8(uint8_t gain) {
  static const int16_t Q8[GainCount] = { 128, 256, 512, 1024, 2048 };
  return Q8[gain < GainCount ? gain : GAIN_DEFAULT];
}
// Applies a gain with a soft knee above 3/4 of full scale, so loud speech bends instead of
// clipping hard.
static inline int16_t gain_apply(int16_t v, int32_t q8) {
  int32_t x = ((int32_t)v * q8) >> 8;
  int32_t a = x < 0 ? -x : x;
  if (a > 24576) {
    a = 24576 + (a - 24576) / 4;
    if (a > 32767) a = 32767;
  }
  return (int16_t)(x < 0 ? -a : a);
}

#define VOLUME_DEFAULT 80

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
#define CAP_TRANSLATE    0x04  // into any language (a chat model)
#define CAP_TRANSLATE_EN 0x08  // into English only (Whisper's translation endpoint)

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
