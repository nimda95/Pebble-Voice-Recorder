// Settings: recording quality, mic gain, where recordings go, Quick Launch behaviour, playback
// volume (watches with a speaker), and storage.
#include "ui.h"
#include "settings.h"
#include "storage.h"
#include "comm.h"
#include "recorder.h"
#include "player.h"

typedef enum { SecRecording = 0, SecPlayback, SecStorage, SecAbout } Section;
enum { RowQuality = 0, RowGain, RowTarget, RowQuickLaunch, RecordingRows };
enum { RowWatch = 0, RowPhone, RowCounts, RowDeleteAll, StorageRows };

#define VOLUME_STEPS 10

static Window *s_window;
static StatusBarLayer *s_status_bar;
static MenuLayer *s_menu;
static char s_sub[64];
static Section s_sections[4];
static int s_section_count;

static const char *const GAIN_NAMES[GainCount] = { "-6 dB", "0 dB", "+6 dB", "+12 dB", "+18 dB" };

// ---- Option list (radio choices with a check mark) -----------------------------------------------

typedef void (*OptionChosen)(int index);

#define MAX_OPTIONS 10

static struct {
  Window *window;
  MenuLayer *menu;
  StatusBarLayer *status_bar;
  GBitmap *check_black;
  GBitmap *check_white;
  const char *names[MAX_OPTIONS];
  const char *infos[MAX_OPTIONS];
  int count;
  int current;
  OptionChosen chosen;
} s_opt;

static uint16_t prv_opt_rows(MenuLayer *m, uint16_t section, void *ctx) { return s_opt.count; }

static void prv_opt_draw(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  GBitmap *icon = NULL;
  if (idx->row == s_opt.current) {
    icon = menu_cell_layer_is_highlighted(cell) ? s_opt.check_white : s_opt.check_black;
  }
  menu_cell_basic_draw(ctx, cell, s_opt.names[idx->row], s_opt.infos[idx->row], icon);
}

static void prv_opt_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  s_opt.current = idx->row;
  layer_mark_dirty(menu_layer_get_layer(m));
  if (s_opt.chosen) s_opt.chosen(idx->row);
  window_stack_remove(s_opt.window, true);
}

static void prv_opt_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_opt.check_black = gbitmap_create_with_resource(RESOURCE_ID_MENU_CHECK_BLACK);
  s_opt.check_white = gbitmap_create_with_resource(RESOURCE_ID_MENU_CHECK_WHITE);
  s_opt.status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_opt.status_bar, GColorWhite, GColorBlack);
  status_bar_layer_set_separator_mode(s_opt.status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_opt.status_bar));
  s_opt.menu = menu_layer_create(GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w, b.size.h - STATUS_BAR_LAYER_HEIGHT));
  menu_layer_set_highlight_colors(s_opt.menu, ACCENT_COLOR, GColorWhite);
  menu_layer_set_callbacks(s_opt.menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = prv_opt_rows,
    .draw_row = prv_opt_draw,
    .select_click = prv_opt_select,
  });
  menu_layer_set_click_config_onto_window(s_opt.menu, window);
  menu_layer_set_selected_index(s_opt.menu, MenuIndex(0, s_opt.current), MenuRowAlignCenter, false);
  layer_add_child(root, menu_layer_get_layer(s_opt.menu));
}

static void prv_opt_unload(Window *window) {
  menu_layer_destroy(s_opt.menu);
  status_bar_layer_destroy(s_opt.status_bar);
  gbitmap_destroy(s_opt.check_black);
  gbitmap_destroy(s_opt.check_white);
  window_destroy(s_opt.window);
  s_opt.window = NULL;
}

// names and infos are copied; infos may be NULL.
static void prv_choose(const char *const *names, const char *const *infos, int count, int current,
                       OptionChosen chosen) {
  if (count > MAX_OPTIONS) count = MAX_OPTIONS;
  for (int i = 0; i < count; i++) {
    s_opt.names[i] = names[i];
    s_opt.infos[i] = infos ? infos[i] : NULL;
  }
  s_opt.count = count;
  s_opt.current = current;
  s_opt.chosen = chosen;
  s_opt.window = window_create();
  window_set_window_handlers(s_opt.window, (WindowHandlers){ .load = prv_opt_load, .unload = prv_opt_unload });
  window_stack_push(s_opt.window, true);
}

// ---- Choices ------------------------------------------------------------------------------------

static void prv_quality_chosen(int i) {
  if (g_settings.quality == i) return;
  g_settings.quality = i;
  settings_changed_on_watch();
}

static void prv_gain_chosen(int i) {
  if (g_settings.gain == i) return;
  g_settings.gain = i;
  settings_changed_on_watch();
}

static void prv_target_chosen(int i) {
  if (g_settings.target == i) return;
  g_settings.target = i;
  settings_changed_on_watch();
}

// Volume options run from 100% at the top down to 10%.
static int prv_volume_row(void) {
  int row = (100 - g_settings.volume + 5) / 10;
  return row < 0 ? 0 : row >= VOLUME_STEPS ? VOLUME_STEPS - 1 : row;
}

static void prv_volume_chosen(int i) {
  g_settings.volume = 100 - i * 10;
  settings_save();  // the volume stays on the watch
}

static void prv_choose_quality(void) {
  const char *names[] = { tr(S_Q_STANDARD), tr(S_Q_HIGH) };
  const char *infos[] = { tr(S_Q_STANDARD_INFO), tr(S_Q_HIGH_INFO) };
  prv_choose(names, infos, QualityCount, g_settings.quality, prv_quality_chosen);
}

static void prv_choose_gain(void) {
  const char *infos[] = { tr(S_G_MINUS6_INFO), tr(S_G_0_INFO), tr(S_G_PLUS6_INFO),
                          tr(S_G_PLUS12_INFO), tr(S_G_PLUS18_INFO) };
  prv_choose(GAIN_NAMES, infos, GainCount, g_settings.gain, prv_gain_chosen);
}

static void prv_choose_target(void) {
  const char *names[] = { tr(S_T_WATCH), tr(S_T_PHONE) };
  const char *infos[] = { tr(S_T_WATCH_INFO), tr(S_T_PHONE_INFO) };
  prv_choose(names, infos, TargetCount, g_settings.target, prv_target_chosen);
}

static void prv_choose_volume(void) {
  static const char *const NAMES[VOLUME_STEPS] = { "100%", "90%", "80%", "70%", "60%",
                                                   "50%", "40%", "30%", "20%", "10%" };
  prv_choose(NAMES, NULL, VOLUME_STEPS, prv_volume_row(), prv_volume_chosen);
}

static void prv_delete_all_answer(bool confirmed, void *ctx) {
  if (!confirmed) return;
  storage_delete_all_local();
  comm_send_list();
  if (s_menu) menu_layer_reload_data(s_menu);
}

// ---- Settings menu ------------------------------------------------------------------------------

static uint16_t prv_sections(MenuLayer *m, void *ctx) { return s_section_count; }

static uint16_t prv_rows(MenuLayer *m, uint16_t section, void *ctx) {
  switch (s_sections[section]) {
    case SecRecording: return RecordingRows;
    case SecStorage: return StorageRows;
    default: return 1;
  }
}

#if defined(PBL_RECT)
static int16_t prv_header_height(MenuLayer *m, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  static const StrId TITLES[] = { S_SEC_RECORDING, S_SEC_PLAYBACK, S_SEC_STORAGE, S_SEC_ABOUT };
  menu_cell_basic_header_draw(ctx, cell, tr(TITLES[s_sections[section]]));
}
#endif

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  const char *title = "";
  const char *sub = s_sub;
  s_sub[0] = '\0';
  char a[16], b[16], left[12];
  switch (s_sections[idx->section]) {
    case SecRecording:
      switch (idx->row) {
        case RowQuality:
          title = tr(S_QUALITY);
          snprintf(s_sub, sizeof(s_sub), tr(S_QUALITY_SUB_FMT),
                   tr(g_settings.quality ? S_Q_HIGH : S_Q_STANDARD), g_settings.quality ? "16" : "8");
          break;
        case RowGain:
          title = tr(S_MIC_GAIN);
          sub = GAIN_NAMES[g_settings.gain];
          break;
        case RowTarget:
          title = tr(S_SAVE_TO);
          sub = tr(g_settings.target ? S_T_PHONE : S_T_WATCH);
          break;
        case RowQuickLaunch:
          title = tr(S_QUICK_LAUNCH);
          sub = tr(g_settings.ql_autostart ? S_QL_STARTS : S_QL_OPENS);
          break;
      }
      break;
    case SecPlayback:
      title = tr(S_VOLUME);
      snprintf(s_sub, sizeof(s_sub), "%d%%", g_settings.volume);
      break;
    case SecStorage:
      switch (idx->row) {
        case RowWatch:
          title = tr(S_ST_WATCH);
          fmt_size(a, sizeof(a), storage_free());
          fmt_size(b, sizeof(b), storage_capacity());
          fmt_duration(left, sizeof(left), storage_seconds_left(g_settings.quality ? 1 : 0));
          // The capacity doesn't fit beside the rest on the 144 px screen.
          if (BIG_SCREEN) snprintf(s_sub, sizeof(s_sub), tr(S_ST_BIG_FMT), a, b, left);
          else snprintf(s_sub, sizeof(s_sub), tr(S_ST_FMT), a, left);
          break;
        case RowPhone:
          title = tr(S_ST_PHONE);
          if (comm_phone_ready()) {
            fmt_size(a, sizeof(a), g_settings.phone_free);
            fmt_duration(left, sizeof(left),
                         g_settings.phone_free / adpcm_bytes_per_sec(g_settings.quality ? 1 : 0));
            snprintf(s_sub, sizeof(s_sub), tr(S_ST_FMT), a, left);
          } else {
            sub = tr(S_ST_NOT_CONNECTED);
          }
          break;
        case RowCounts:
          title = tr(S_ST_RECORDINGS);
          snprintf(s_sub, sizeof(s_sub), tr(S_ST_COUNTS_FMT), storage_local_count(),
                   storage_phone_count());
          break;
        case RowDeleteAll:
          title = tr(S_ST_DELETE_ALL);
          sub = tr(storage_deleting() ? S_ST_FREEING : S_ST_COPIES_STAY);
          break;
      }
      break;
    case SecAbout:
      title = tr(S_APP_NAME);
      snprintf(s_sub, sizeof(s_sub), tr(S_VERSION_FMT), APP_VERSION);
      break;
  }
  menu_cell_basic_draw(ctx, cell, title, sub[0] ? sub : NULL, NULL);
}

static void prv_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  switch (s_sections[idx->section]) {
    case SecRecording:
      switch (idx->row) {
        case RowQuality: prv_choose_quality(); break;
        case RowGain: prv_choose_gain(); break;
        case RowTarget: prv_choose_target(); break;
        case RowQuickLaunch:
          g_settings.ql_autostart = !g_settings.ql_autostart;
          settings_changed_on_watch();
          menu_layer_reload_data(m);
          break;
      }
      break;
    case SecPlayback:
      prv_choose_volume();
      break;
    case SecStorage:
      if (idx->row != RowDeleteAll || storage_local_count() == 0) return;
      if (recorder_state() != RecIdle) {
        dialog_message(tr(S_STOP_FIRST), RESOURCE_ID_DLG_MIC, ACCENT_DARK, 2500);
        return;
      }
      dialog_confirm(tr(S_ASK_DELETE_ALL), RESOURCE_ID_DLG_TRASH, ACCENT_COLOR,
                     prv_delete_all_answer, NULL);
      break;
    default:
      break;
  }
}

static void prv_comm_event(CommEvent ev, uint16_t id, const char *text) {
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_section_count = 0;
  s_sections[s_section_count++] = SecRecording;
  if (player_available()) s_sections[s_section_count++] = SecPlayback;
  s_sections[s_section_count++] = SecStorage;
  s_sections[s_section_count++] = SecAbout;

  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorWhite, GColorBlack);
  status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));
  s_menu = menu_layer_create(GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w, b.size.h - STATUS_BAR_LAYER_HEIGHT));
  menu_layer_set_highlight_colors(s_menu, ACCENT_COLOR, GColorWhite);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections = prv_sections,
    .get_num_rows = prv_rows,
    .get_header_height = PBL_IF_RECT_ELSE(prv_header_height, NULL),
    .draw_header = PBL_IF_RECT_ELSE(prv_draw_header, NULL),
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
  comm_add_listener(prv_comm_event);
}

static void prv_appear(Window *window) {
  menu_layer_reload_data(s_menu);
}

static void prv_unload(Window *window) {
  comm_remove_listener(prv_comm_event);
  menu_layer_destroy(s_menu);
  status_bar_layer_destroy(s_status_bar);
  s_menu = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void ui_settings_push(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_load,
    .appear = prv_appear,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}
