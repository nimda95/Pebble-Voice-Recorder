#include "settings.h"
#include "comm.h"

#define SETTINGS_VERSION 1

Settings g_settings;

void settings_load(void) {
  memset(&g_settings, 0, sizeof(g_settings));
  if (persist_read_data(PKEY_SETTINGS, &g_settings, sizeof(g_settings)) != (int)sizeof(g_settings) ||
      g_settings.version != SETTINGS_VERSION) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.version = SETTINGS_VERSION;
    g_settings.quality = QualityStandard;
    g_settings.target = TargetWatch;
    g_settings.ql_autostart = 1;
    strncpy(g_settings.lang, "en", LANG_CODE_LEN);
    g_settings.next_id = 1;
  }
  if (g_settings.quality >= QualityCount) g_settings.quality = QualityStandard;
  if (g_settings.target >= TargetCount) g_settings.target = TargetWatch;
  g_settings.lang[LANG_CODE_LEN - 1] = '\0';
  if (g_settings.next_id == 0 || g_settings.next_id >= 0x8000) g_settings.next_id = 1;
}

void settings_save(void) {
  persist_write_data(PKEY_SETTINGS, &g_settings, sizeof(g_settings));
}

void settings_changed_on_watch(void) {
  g_settings.dirty = 1;
  settings_save();
  comm_send_settings();
}
