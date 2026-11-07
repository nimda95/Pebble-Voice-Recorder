#include "i18n.h"

// English is compiled in. Other languages are resources (resources/strings/<lang>.bin, built from
// strings_<lang>.def by tools/build_strings.py), and only the watch's language is loaded, so the
// translations cost one language's text in RAM rather than all of them.

static const char *const EN[S_COUNT] = {
#define S(id, text) [id] = text,
#include "strings.def"
#undef S
};

static const char *const CODES[UiLangCount] = { "en", "fr", "de", "es", "it", "pt" };

static UiLang s_lang;
static uint8_t *s_blob;        // the loaded language file
static uint16_t s_blob_strings;
static uint16_t s_blob_langs;

static uint32_t prv_resource(UiLang lang) {
  switch (lang) {
    case UiFr: return RESOURCE_ID_STRINGS_FR;
    case UiDe: return RESOURCE_ID_STRINGS_DE;
    case UiEs: return RESOURCE_ID_STRINGS_ES;
    case UiIt: return RESOURCE_ID_STRINGS_IT;
    case UiPt: return RESOURCE_ID_STRINGS_PT;
    default: return 0;
  }
}

static void prv_load(UiLang lang) {
  uint32_t id = prv_resource(lang);
  if (!id) return;
  ResHandle h = resource_get_handle(id);
  size_t size = resource_size(h);
  if (size < 4) return;
  s_blob = malloc(size);
  if (!s_blob) {
    s_lang = UiEn;  // not enough memory: stay in English
    return;
  }
  resource_load(h, s_blob, size);
  s_blob_strings = s_blob[0] | (s_blob[1] << 8);
  s_blob_langs = s_blob[2] | (s_blob[3] << 8);
}

// Text number n in the loaded file (strings, then language names), or NULL.
static const char *prv_blob_text(uint16_t n) {
  if (!s_blob || n >= s_blob_strings + s_blob_langs) return NULL;
  const uint8_t *off = s_blob + 4 + 2 * n;
  uint16_t at = off[0] | (off[1] << 8);
  return at ? (const char *)s_blob + at : NULL;
}

void i18n_init(void) {
  s_lang = UiEn;
  const char *locale = i18n_get_system_locale();
  for (int i = 1; locale && i < UiLangCount; i++) {
    if (strncmp(locale, CODES[i], 2) == 0) s_lang = (UiLang)i;
  }
  prv_load(s_lang);
}

void i18n_deinit(void) {
  free(s_blob);
  s_blob = NULL;
}

UiLang i18n_lang(void) { return s_lang; }
const char *i18n_code(void) { return CODES[s_lang]; }

const char *tr(StrId id) {
  if (id >= S_COUNT) return "";
  const char *s = s_blob_strings == S_COUNT ? prv_blob_text(id) : NULL;
  return s ? s : EN[id];
}

const char *i18n_language_name(int index, const char *english) {
  const char *s = s_blob ? prv_blob_text(s_blob_strings + index) : NULL;
  return s ? s : english;
}
