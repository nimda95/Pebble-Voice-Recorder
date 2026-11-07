#include "lang.h"

// Keep in sync with src/pkjs/languages.js.
static const Language LANGS[] = {
  { "en", "English" },
  { "ar", "Arabic" },
  { "fr", "French" },
  { "es", "Spanish" },
  { "de", "German" },
  { "it", "Italian" },
  { "pt", "Portuguese" },
  { "nl", "Dutch" },
  { "tr", "Turkish" },
  { "ru", "Russian" },
  { "uk", "Ukrainian" },
  { "pl", "Polish" },
  { "cs", "Czech" },
  { "ro", "Romanian" },
  { "hu", "Hungarian" },
  { "el", "Greek" },
  { "sv", "Swedish" },
  { "da", "Danish" },
  { "no", "Norwegian" },
  { "fi", "Finnish" },
  { "he", "Hebrew" },
  { "fa", "Persian" },
  { "hi", "Hindi" },
  { "ur", "Urdu" },
  { "id", "Indonesian" },
  { "ms", "Malay" },
  { "vi", "Vietnamese" },
  { "th", "Thai" },
  { "zh", "Chinese" },
  { "ja", "Japanese" },
  { "ko", "Korean" },
  { "ca", "Catalan" },
};

int lang_count(void) { return ARRAY_LENGTH(LANGS); }

const Language *lang_get(int index) {
  return (index >= 0 && index < lang_count()) ? &LANGS[index] : NULL;
}

int lang_index_of(const char *code) {
  for (int i = 0; i < lang_count(); i++) {
    if (strcmp(LANGS[i].code, code) == 0) return i;
  }
  return -1;
}
