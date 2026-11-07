#pragma once
#include "common.h"

// IMA ADPCM, WAV block layout (mono, 256-byte blocks of 505 samples).
typedef struct {
  int16_t predictor;
  int8_t index;
  uint16_t pos;                      // samples placed in the current block
  uint8_t block[ADPCM_BLOCK_BYTES];
} AdpcmEncoder;

void adpcm_encoder_reset(AdpcmEncoder *e);
// Adds one sample. Returns true when e->block holds a complete block.
bool adpcm_encoder_push(AdpcmEncoder *e, int16_t sample);
// Completes a partial block by holding the last value. Returns how many of its samples are real
// (0 when nothing was pending).
uint16_t adpcm_encoder_flush(AdpcmEncoder *e);

// Decodes one block into ADPCM_BLOCK_SAMPLES samples.
void adpcm_decode_block(const uint8_t *block, int16_t *out);

// Half-band low-pass and 2:1 decimation, 16 kHz -> 8 kHz.
typedef struct {
  int16_t x[7];
  uint8_t phase;
} Decimator;

void decimator_reset(Decimator *d);
// Feeds one 16 kHz sample; returns true with *out set every second sample.
bool decimator_push(Decimator *d, int16_t in, int16_t *out);
