#include "adpcm.h"

static const int16_t STEP_TABLE[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
  449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
  9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t INDEX_TABLE[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int clamp16(int v) {
  return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

static inline int clamp_index(int i) {
  return i < 0 ? 0 : (i > 88 ? 88 : i);
}

void adpcm_encoder_reset(AdpcmEncoder *e) {
  e->predictor = 0;
  e->index = 0;
  e->pos = 0;
}

static uint8_t prv_encode(AdpcmEncoder *e, int sample) {
  int step = STEP_TABLE[(int)e->index];
  int diff = sample - e->predictor;
  uint8_t nib = 0;
  if (diff < 0) {
    nib = 8;
    diff = -diff;
  }
  int delta = step >> 3;
  if (diff >= step) { nib |= 4; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { nib |= 2; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { nib |= 1; delta += step; }
  e->predictor = clamp16((nib & 8) ? e->predictor - delta : e->predictor + delta);
  e->index = clamp_index(e->index + INDEX_TABLE[nib]);
  return nib;
}

bool adpcm_encoder_push(AdpcmEncoder *e, int16_t sample) {
  if (e->pos == 0) {
    e->predictor = sample;
    e->block[0] = (uint8_t)(sample & 0xff);
    e->block[1] = (uint8_t)((sample >> 8) & 0xff);
    e->block[2] = (uint8_t)e->index;
    e->block[3] = 0;
    e->pos = 1;
    return false;
  }
  uint8_t nib = prv_encode(e, sample);
  uint16_t k = e->pos - 1;
  uint8_t *b = &e->block[4 + (k >> 1)];
  if (k & 1) {
    *b |= (uint8_t)(nib << 4);
  } else {
    *b = nib;
  }
  if (++e->pos == ADPCM_BLOCK_SAMPLES) {
    e->pos = 0;
    return true;
  }
  return false;
}

uint16_t adpcm_encoder_flush(AdpcmEncoder *e) {
  if (e->pos == 0) return 0;
  uint16_t real = e->pos;
  int16_t hold = e->predictor;
  while (!adpcm_encoder_push(e, hold)) {}
  return real;
}

void adpcm_decode_block(const uint8_t *b, int16_t *out) {
  int pred = (int16_t)(b[0] | (b[1] << 8));
  int index = clamp_index(b[2]);
  out[0] = (int16_t)pred;
  for (int k = 0; k < ADPCM_BLOCK_SAMPLES - 1; k++) {
    uint8_t byte = b[4 + (k >> 1)];
    uint8_t nib = (k & 1) ? (byte >> 4) : (byte & 0x0f);
    int step = STEP_TABLE[index];
    int delta = step >> 3;
    if (nib & 4) delta += step;
    if (nib & 2) delta += step >> 1;
    if (nib & 1) delta += step >> 2;
    pred = clamp16((nib & 8) ? pred - delta : pred + delta);
    index = clamp_index(index + INDEX_TABLE[nib]);
    out[k + 1] = (int16_t)pred;
  }
}

void decimator_reset(Decimator *d) {
  memset(d, 0, sizeof(*d));
}

bool decimator_push(Decimator *d, int16_t in, int16_t *out) {
  // 7-tap half-band: [-1, 0, 9, 16, 9, 0, -1] / 32
  memmove(&d->x[0], &d->x[1], 6 * sizeof(int16_t));
  d->x[6] = in;
  d->phase ^= 1;
  if (d->phase) return false;
  int32_t y = -d->x[0] + 9 * d->x[2] + 16 * d->x[3] + 9 * d->x[4] - d->x[6];
  *out = (int16_t)clamp16(y >> 5);
  return true;
}
