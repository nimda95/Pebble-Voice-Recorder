#pragma once
#include "common.h"

// Languages offered for transcription (ISO 639-1 codes, as Whisper takes them).
typedef struct {
  const char *code;
  const char *name;
} Language;

int lang_count(void);
const Language *lang_get(int index);
int lang_index_of(const char *code);
