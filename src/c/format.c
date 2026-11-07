#include "ui.h"
#include "i18n.h"

void fmt_duration(char *buf, size_t size, uint32_t secs) {
  int h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
  if (h > 0) {
    snprintf(buf, size, "%d:%02d:%02d", h, m, s);
  } else {
    snprintf(buf, size, "%d:%02d", m, s);
  }
}

void fmt_size(char *buf, size_t size, uint32_t bytes) {
  if (bytes < 1024) {
    snprintf(buf, size, "%d B", (int)bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, size, "%d KB", (int)((bytes + 512) / 1024));
  } else {
    uint32_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
    if (tenths % 10 == 0) {
      snprintf(buf, size, "%d MB", (int)(tenths / 10));
    } else {
      snprintf(buf, size, "%d.%d MB", (int)(tenths / 10), (int)(tenths % 10));
    }
  }
}

void fmt_datetime(char *buf, size_t size, time_t t) {
  struct tm *tm = localtime(&t);
  const char *mon = tr((StrId)(S_MON_1 + tm->tm_mon));
  // "Nov 7" in English, "7 nov." / "7. Nov." elsewhere.
  char day[24];
  UiLang l = i18n_lang();
  if (l == UiEn) snprintf(day, sizeof(day), "%s %d", mon, tm->tm_mday);
  else if (l == UiDe) snprintf(day, sizeof(day), "%d. %s", tm->tm_mday, mon);
  else snprintf(day, sizeof(day), "%d %s", tm->tm_mday, mon);
  if (clock_is_24h_style()) {
    snprintf(buf, size, "%s, %02d:%02d", day, tm->tm_hour, tm->tm_min);
  } else {
    int h = tm->tm_hour % 12;
    snprintf(buf, size, "%s, %d:%02d %s", day, h ? h : 12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
  }
}

void fmt_title(char *buf, size_t size, const char *name, time_t created) {
  if (name && name[0]) {
    strncpy(buf, name, size - 1);
    buf[size - 1] = '\0';
  } else {
    fmt_datetime(buf, size, created);
  }
}
