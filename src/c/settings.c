#include "settings.h"
#include "comm.h"

#define SETTINGS_VERSION 2
// Size of version 1, which ended at phone_used.
#define SETTINGS_V1_SIZE (offsetof(Settings, gain))

Settings g_settings;

static void prv_defaults_v2(void) {
  g_settings.gain = GAIN_DEFAULT;
  g_settings.volume = VOLUME_DEFAULT;
  strncpy(g_settings.tlang, "en", LANG_CODE_LEN);
}

void settings_load(void) {
  memset(&g_settings, 0, sizeof(g_settings));
  int n = persist_read_data(PKEY_SETTINGS, &g_settings, sizeof(g_settings));
  if (n == (int)SETTINGS_V1_SIZE && g_settings.version == 1) {
    // Keep the version 1 settings and add the new ones.
    g_settings.version = SETTINGS_VERSION;
    prv_defaults_v2();
  } else if (n != (int)sizeof(g_settings) || g_settings.version != SETTINGS_VERSION) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.version = SETTINGS_VERSION;
    g_settings.quality = QualityStandard;
    g_settings.target = TargetWatch;
    g_settings.ql_autostart = 1;
    strncpy(g_settings.lang, "en", LANG_CODE_LEN);
    g_settings.next_id = 1;
    prv_defaults_v2();
  }
  if (g_settings.quality >= QualityCount) g_settings.quality = QualityStandard;
  if (g_settings.target >= TargetCount) g_settings.target = TargetWatch;
  if (g_settings.gain >= GainCount) g_settings.gain = GAIN_DEFAULT;
  if (g_settings.volume > 100) g_settings.volume = VOLUME_DEFAULT;
  g_settings.lang[LANG_CODE_LEN - 1] = '\0';
  g_settings.tlang[LANG_CODE_LEN - 1] = '\0';
  if (!g_settings.tlang[0]) strncpy(g_settings.tlang, "en", LANG_CODE_LEN);
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
