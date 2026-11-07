#include "lang.h"
#include "i18n.h"

// Codes and English names; the other languages' names come with their translations (i18n.c).
typedef struct {
  const char *code;
  const char *name;
} Language;

static const Language LANGS[] = {
#define L(code, en, fr, de, es, it, pt) { code, en },
#include "lang_names.def"
#undef L
};

int lang_count(void) { return ARRAY_LENGTH(LANGS); }

const char *lang_code(int index) {
  return (index >= 0 && index < lang_count()) ? LANGS[index].code : NULL;
}

const char *lang_name(int index) {
  return (index >= 0 && index < lang_count()) ? i18n_language_name(index, LANGS[index].name) : "";
}

int lang_index_of(const char *code) {
  for (int i = 0; i < lang_count(); i++) {
    if (strcmp(LANGS[i].code, code) == 0) return i;
  }
  return -1;
}
