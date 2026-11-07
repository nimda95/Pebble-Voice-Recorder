#pragma once
#include "common.h"
#include "i18n.h"

// Recorder (the app's first window)
void ui_recorder_push(void);
void ui_recorder_bring_to_front(void);
void ui_recorder_autostart(void);

// Recordings list and per-recording action menu
void ui_list_push(void);
void ui_actions_open(uint16_t id);

// Settings
void ui_settings_push(void);

// Playback: the plain player, or the transcript or translation following the audio
typedef enum { TextNone = 0, TextTranscript, TextTranslation } TextKind;
void ui_playback_push(uint16_t id, TextKind text, bool autoplay);

// Dialogs in the PebbleOS style: message with an icon, and choice with a check / cross action bar
typedef void (*DialogConfirmCallback)(bool confirmed, void *context);
void dialog_confirm(const char *message, uint32_t icon_resource, GColor background,
                    DialogConfirmCallback callback, void *context);
void dialog_message(const char *message, uint32_t icon_resource, GColor background,
                    uint32_t timeout_ms);

// Formatting
void fmt_duration(char *buf, size_t size, uint32_t seconds);
void fmt_size(char *buf, size_t size, uint32_t bytes);
void fmt_datetime(char *buf, size_t size, time_t t);
void fmt_title(char *buf, size_t size, const char *name, time_t created);
