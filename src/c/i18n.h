#pragma once
#include "common.h"

// The watch's own language decides the app's: English, French, German, Spanish, Italian or
// Portuguese (the languages PebbleOS has language packs for), English for anything else.
typedef enum { UiEn = 0, UiFr, UiDe, UiEs, UiIt, UiPt, UiLangCount } UiLang;

typedef enum {
#define S(id, text) id,
#include "strings.def"
#undef S
  S_COUNT
} StrId;

void i18n_init(void);
void i18n_deinit(void);
UiLang i18n_lang(void);
// Two-letter code of the app's language, e.g. "fr"; sent to the phone so its messages match.
const char *i18n_code(void);
const char *tr(StrId id);
// The name of transcription language number index (src/c/lang_names.def) in the app language.
const char *i18n_language_name(int index, const char *english);
