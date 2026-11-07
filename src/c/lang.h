#pragma once
#include "common.h"

// Languages offered for transcription and translation (ISO 639-1 codes, as Whisper takes them),
// named in the app's language.
int lang_count(void);
const char *lang_code(int index);
const char *lang_name(int index);
int lang_index_of(const char *code);
