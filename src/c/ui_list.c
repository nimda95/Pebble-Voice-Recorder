// Past recordings, newest at the top, opened on the newest.
#include "ui.h"
#include "storage.h"
#include "settings.h"
#include "comm.h"
#include "recorder.h"
#include "player.h"
#include "lang.h"

static Window *s_window;
static StatusBarLayer *s_status_bar;
static MenuLayer *s_menu;
static TextLayer *s_empty;
static char s_title[REC_NAME_LEN + 4];
static char s_subtitle[64];
static uint16_t s_selected_id;

// ---- Rows ---------------------------------------------------------------------------------------

// Storage is sorted oldest first; the list shows it the other way round.
static RecMeta *prv_at(int row) { return storage_get(storage_count() - 1 - row); }

static int prv_row_of(uint16_t id) {
  int idx = storage_index_of(id);
  return idx < 0 ? -1 : storage_count() - 1 - idx;
}

static uint16_t prv_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return storage_count();
}

static void prv_subtitle(const RecMeta *m, char *buf, size_t size) {
  const JobStatus *job = comm_job_for(m->id);
  if (m->flags & RecOpen) {
    snprintf(buf, size, "%s", tr(S_LIST_RECORDING));
    return;
  }
  if (job) {
    const char *what = tr(job->kind == JobTranscribe ? S_JOB_TRANSCRIBING :
                          job->kind == JobTranslate ? S_JOB_TRANSLATING :
                          job->kind == JobUpload ? S_JOB_UPLOADING : S_JOB_SAVING);
    if (job->status == JobRunning && job->pct > 0 && job->pct < 100) {
      snprintf(buf, size, "%s %d%%", what, job->pct);
    } else {
      snprintf(buf, size, "%s...", what);
    }
    return;
  }
  char size_s[12], dur[12];
  fmt_size(size_s, sizeof(size_s), storage_size_bytes(m));
  fmt_duration(dur, sizeof(dur), (storage_duration_ms(m) + 500) / 1000);
  const char *where = "";
  if (!(m->flags & RecLocal)) where = tr((m->flags & RecPhone) ? S_PREFIX_PHONE : S_PREFIX_TEXT);
  if (!(m->flags & (RecLocal | RecPhone))) {
    snprintf(buf, size, "%s%s", where, dur);
  } else {
    snprintf(buf, size, "%s%s, %s", where, size_s, dur);
  }
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  const RecMeta *m = prv_at(idx->row);
  if (!m) return;
  fmt_title(s_title, sizeof(s_title), m->name, m->created);
  prv_subtitle(m, s_subtitle, sizeof(s_subtitle));
  menu_cell_basic_draw(ctx, cell, s_title, s_subtitle, NULL);
}

static int16_t prv_row_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
#if defined(PBL_ROUND)
  return menu_layer_is_index_selected(menu, idx) ? MENU_CELL_ROUND_FOCUSED_TALL_CELL_HEIGHT
                                                 : MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT;
#else
  return BIG_SCREEN ? 56 : 44;
#endif
}

static void prv_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  const RecMeta *m = prv_at(idx->row);
  if (!m) return;
  if (m->flags & RecOpen) {
    dialog_message(tr(S_STILL_RECORDING), RESOURCE_ID_DLG_MIC, ACCENT_DARK, 3000);
    return;
  }
  ui_actions_open(m->id);
}

static void prv_selection_changed(MenuLayer *menu, MenuIndex new_idx, MenuIndex old_idx, void *ctx) {
  const RecMeta *m = prv_at(new_idx.row);
  if (m) s_selected_id = m->id;
}

static void prv_reload(void) {
  if (!s_window) return;
  menu_layer_reload_data(s_menu);
  int n = storage_count();
  layer_set_hidden(text_layer_get_layer(s_empty), n > 0);
  layer_set_hidden(menu_layer_get_layer(s_menu), n == 0);
  int row = prv_row_of(s_selected_id);
  if (row < 0) row = 0;
  if (n > 0) {
    menu_layer_set_selected_index(s_menu, MenuIndex(0, row), MenuRowAlignCenter, false);
  }
}

static void prv_comm_event(CommEvent ev, uint16_t id, const char *text) {
  if (!s_window) return;
  if (ev == CommEvList || ev == CommEvJob || ev == CommEvJobFailed) prv_reload();
  if (ev == CommEvJobFailed && window_stack_get_top_window() == s_window) {
    dialog_message(text && text[0] ? text : tr(S_DIDNT_WORK), RESOURCE_ID_DLG_WARNING,
                   ACCENT_DARK, 5000);
  }
}

// ---- Window -------------------------------------------------------------------------------------

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorWhite, GColorBlack);
  status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));

  GRect menu_frame = GRect(0, STATUS_BAR_LAYER_HEIGHT, bounds.size.w,
                           bounds.size.h - STATUS_BAR_LAYER_HEIGHT);
  s_menu = menu_layer_create(menu_frame);
  menu_layer_set_highlight_colors(s_menu, ACCENT_COLOR, GColorWhite);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = prv_rows,
    .draw_row = prv_draw_row,
    .get_cell_height = prv_row_height,
    .select_click = prv_select,
    .selection_changed = prv_selection_changed,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  s_empty = text_layer_create(GRect(10, bounds.size.h / 2 - 30, bounds.size.w - 20, 60));
  text_layer_set_text(s_empty, tr(S_NO_RECORDINGS));
  text_layer_set_font(s_empty, fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_empty, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_empty));

  s_selected_id = 0;
  prv_reload();
  comm_add_listener(prv_comm_event);
}

static void prv_appear(Window *window) { prv_reload(); }

static void prv_unload(Window *window) {
  comm_remove_listener(prv_comm_event);
  menu_layer_destroy(s_menu);
  text_layer_destroy(s_empty);
  status_bar_layer_destroy(s_status_bar);
  window_destroy(s_window);
  s_window = NULL;
}

void ui_list_push(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_load,
    .appear = prv_appear,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}

// ---- Action menu --------------------------------------------------------------------------------

typedef enum {
  AcNone = 0,
  AcPlay,
  AcTranscript,
  AcTranslation,
  AcTranscribe,
  AcTranslate,
  AcUpload,
  AcSave,
  AcDeleteWatch,
  AcDeletePhone,
  AcDeleteAll,
} ActionCode;

#define ACTION(code, lang) ((void *)(uintptr_t)((code) | ((lang) << 8)))
#define LANG_ENGLISH_ONLY 0xff

static uint16_t s_action_id;
static ActionCode s_pending;
static int s_pending_lang;
static ActionMenuLevel *s_root;

static void prv_perform(ActionMenu *menu, const ActionMenuItem *item, void *ctx) {
  uintptr_t v = (uintptr_t)action_menu_item_get_action_data(item);
  s_pending = (ActionCode)(v & 0xff);
  s_pending_lang = (int)(v >> 8);
}

static bool prv_need_phone(void) {
  if (comm_phone_ready()) return true;
  dialog_message(tr(S_CONNECT_PHONE), RESOURCE_ID_DLG_PHONE, ACCENT_DARK, 3000);
  return false;
}

static void prv_delete_answer(bool confirmed, void *ctx) {
  if (!confirmed) return;
  ActionCode code = (ActionCode)(uintptr_t)ctx;
  uint16_t id = s_action_id;
  switch (code) {
    case AcDeleteWatch:
      storage_delete_local(id);
      comm_send_simple(W_DELETED, id, 0);
      break;
    case AcDeletePhone:
      comm_send_action(id, ActDeletePhone, NULL);
      break;
    case AcDeleteAll:
      storage_remove(id);
      comm_send_action(id, ActDeleteAll, NULL);
      break;
    default:
      break;
  }
  prv_reload();
}

// Remembers the language picked, so it is listed first next time.
static const char *prv_pick_lang(char *setting) {
  const char *code = lang_code(s_pending_lang);
  if (code && strcmp(setting, code) != 0) {
    strncpy(setting, code, LANG_CODE_LEN - 1);
    setting[LANG_CODE_LEN - 1] = '\0';
    settings_changed_on_watch();
  }
  return code ? code : setting;
}

static void prv_run_pending(void) {
  uint16_t id = s_action_id;
  RecMeta *m = storage_find(id);
  if (!m) return;
  switch (s_pending) {
    case AcPlay:
      ui_playback_push(id, (m->flags & RecTranscript) ? TextTranscript : TextNone, true);
      break;
    case AcTranscript:
      ui_playback_push(id, TextTranscript, false);
      break;
    case AcTranslation:
      ui_playback_push(id, TextTranslation, false);
      break;
    case AcTranscribe:
      if (prv_need_phone()) comm_send_action(id, ActTranscribe, prv_pick_lang(g_settings.lang));
      break;
    case AcTranslate:
      if (prv_need_phone()) {
        // LANG_ENGLISH_ONLY: Whisper's translation, which is always into English.
        comm_send_action(id, ActTranslate,
                         s_pending_lang == LANG_ENGLISH_ONLY ? "en" : prv_pick_lang(g_settings.tlang));
      }
      break;
    case AcUpload:
      if (prv_need_phone()) comm_send_action(id, ActUpload, NULL);
      break;
    case AcSave:
      if (prv_need_phone()) comm_send_action(id, ActSaveToPhone, NULL);
      break;
    case AcDeleteWatch:
      dialog_confirm(tr((m->flags & RecPhone) ? S_ASK_DELETE_WATCH_KEEP
                        : (m->flags & RecTranscript) ? S_ASK_DELETE_AUDIO_KEEP_TEXT : S_ASK_DELETE),
                     RESOURCE_ID_DLG_TRASH, ACCENT_COLOR, prv_delete_answer,
                     (void *)(uintptr_t)AcDeleteWatch);
      break;
    case AcDeletePhone:
      if (prv_need_phone()) {
        dialog_confirm(tr(S_ASK_DELETE), RESOURCE_ID_DLG_TRASH, ACCENT_COLOR,
                       prv_delete_answer, (void *)(uintptr_t)AcDeletePhone);
      }
      break;
    case AcDeleteAll:
      if (prv_need_phone()) {
        dialog_confirm(tr(S_ASK_DELETE_BOTH), RESOURCE_ID_DLG_TRASH, ACCENT_COLOR,
                       prv_delete_answer, (void *)(uintptr_t)AcDeleteAll);
      }
      break;
    default:
      break;
  }
}

static void prv_did_close(ActionMenu *menu, const ActionMenuItem *item, void *ctx) {
  action_menu_hierarchy_destroy(s_root, NULL, NULL);
  s_root = NULL;
  prv_run_pending();
  s_pending = AcNone;
}

// A level listing every language, the last one used first.
static ActionMenuLevel *prv_language_level(ActionCode code, const char *last_code) {
  int n = lang_count();
  int last = lang_index_of(last_code);
  ActionMenuLevel *level = action_menu_level_create(n);
  if (last >= 0) action_menu_level_add_action(level, lang_name(last), prv_perform, ACTION(code, last));
  for (int i = 0; i < n; i++) {
    if (i != last) action_menu_level_add_action(level, lang_name(i), prv_perform, ACTION(code, i));
  }
  return level;
}

void ui_actions_open(uint16_t id) {
  RecMeta *m = storage_find(id);
  if (!m) return;
  s_action_id = id;
  s_pending = AcNone;

  bool local = m->flags & RecLocal;
  bool phone_audio = m->flags & RecPhone;
  bool transcript = m->flags & RecTranscript;
  bool translation = m->flags & RecTranslation;
  bool has_audio = local || phone_audio;

  s_root = action_menu_level_create(9);
  if (player_available() && has_audio) {
    action_menu_level_add_action(s_root, tr(S_A_PLAY), prv_perform, ACTION(AcPlay, 0));
  }
  if (transcript) {
    action_menu_level_add_action(s_root, tr(S_A_TRANSCRIPT), prv_perform, ACTION(AcTranscript, 0));
  }
  if (translation) {
    action_menu_level_add_action(s_root, tr(S_A_TRANSLATION), prv_perform, ACTION(AcTranslation, 0));
  }
  if ((g_settings.caps & CAP_TRANSCRIBE) && has_audio) {
    action_menu_level_add_child(s_root, prv_language_level(AcTranscribe, g_settings.lang),
                                tr(transcript ? S_A_TRANSCRIBE_AGAIN : S_A_TRANSCRIBE));
  }
  // A chat model translates the transcript (making one first if needed); Whisper only into
  // English, from the audio.
  bool can_text = transcript || ((g_settings.caps & CAP_TRANSCRIBE) && has_audio);
  if ((g_settings.caps & CAP_TRANSLATE) && can_text) {
    action_menu_level_add_child(s_root, prv_language_level(AcTranslate, g_settings.tlang),
                                tr(translation ? S_A_TRANSLATE_AGAIN : S_A_TRANSLATE));
  } else if ((g_settings.caps & CAP_TRANSLATE_EN) && has_audio) {
    action_menu_level_add_action(s_root, tr(S_A_TRANSLATE_EN), prv_perform, ACTION(AcTranslate, LANG_ENGLISH_ONLY));
  }
  if ((g_settings.caps & CAP_WEBDAV) && has_audio) {
    action_menu_level_add_action(s_root, tr((m->flags & RecUploaded) ? S_A_UPLOAD_AGAIN : S_A_UPLOAD),
                                 prv_perform, ACTION(AcUpload, 0));
  }
  if (local && !phone_audio) {
    action_menu_level_add_action(s_root, tr(S_A_SAVE), prv_perform, ACTION(AcSave, 0));
  }
  if (local && (phone_audio || transcript || translation)) {
    ActionMenuLevel *del = action_menu_level_create(2);
    action_menu_level_add_action(del, tr(S_A_FROM_WATCH), prv_perform, ACTION(AcDeleteWatch, 0));
    action_menu_level_add_action(del, tr(S_A_EVERYWHERE), prv_perform, ACTION(AcDeleteAll, 0));
    action_menu_level_add_child(s_root, del, tr(S_A_DELETE));
  } else if (local) {
    action_menu_level_add_action(s_root, tr(S_A_DELETE), prv_perform, ACTION(AcDeleteWatch, 0));
  } else {
    action_menu_level_add_action(s_root, tr(S_A_DELETE), prv_perform, ACTION(AcDeletePhone, 0));
  }

  ActionMenuConfig config = {
    .root_level = s_root,
    .colors = {
      .background = ACCENT_COLOR,
      .foreground = GColorBlack,
    },
    .did_close = prv_did_close,
    .align = ActionMenuAlignCenter,
  };
  action_menu_open(&config);
}
