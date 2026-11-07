// Settings: recording quality, where recordings go, Quick Launch behaviour, and storage.
#include "ui.h"
#include "settings.h"
#include "storage.h"
#include "comm.h"
#include "recorder.h"

enum { SecRecording = 0, SecStorage, SecAbout, SecCount };
enum { RowQuality = 0, RowTarget, RowQuickLaunch, RecordingRows };
enum { RowWatch = 0, RowPhone, RowCounts, RowDeleteAll, StorageRows };

static Window *s_window;
static StatusBarLayer *s_status_bar;
static MenuLayer *s_menu;
static char s_sub[64];

static const char *const QUALITY_NAMES[] = { "Standard", "High" };
static const char *const QUALITY_INFO[] = { "8 kHz, 4 KB per second", "16 kHz, 8 KB per second" };
static const char *const TARGET_NAMES[] = { "Watch", "Phone" };
static const char *const TARGET_INFO[] = { "Works without the phone", "Streams; much longer recordings" };

// ---- Option list (radio choices with a check mark) -----------------------------------------------

typedef void (*OptionChosen)(int index);

static struct {
  Window *window;
  MenuLayer *menu;
  StatusBarLayer *status_bar;
  GBitmap *check_black;
  GBitmap *check_white;
  const char *const *names;
  const char *const *infos;
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
  menu_cell_basic_draw(ctx, cell, s_opt.names[idx->row], s_opt.infos ? s_opt.infos[idx->row] : NULL, icon);
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

static void prv_choose(const char *const *names, const char *const *infos, int count, int current,
                       OptionChosen chosen) {
  s_opt.names = names;
  s_opt.infos = infos;
  s_opt.count = count;
  s_opt.current = current;
  s_opt.chosen = chosen;
  s_opt.window = window_create();
  window_set_window_handlers(s_opt.window, (WindowHandlers){ .load = prv_opt_load, .unload = prv_opt_unload });
  window_stack_push(s_opt.window, true);
}

// ---- Settings menu ------------------------------------------------------------------------------

static void prv_quality_chosen(int i) {
  if (g_settings.quality == i) return;
  g_settings.quality = i;
  settings_changed_on_watch();
}

static void prv_target_chosen(int i) {
  if (g_settings.target == i) return;
  g_settings.target = i;
  settings_changed_on_watch();
}

static void prv_delete_all_answer(bool confirmed, void *ctx) {
  if (!confirmed) return;
  storage_delete_all_local();
  comm_send_list();
  if (s_menu) menu_layer_reload_data(s_menu);
}

static uint16_t prv_sections(MenuLayer *m, void *ctx) { return SecCount; }

static uint16_t prv_rows(MenuLayer *m, uint16_t section, void *ctx) {
  switch (section) {
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
  static const char *const TITLES[] = { "Recording", "Storage", "About" };
  menu_cell_basic_header_draw(ctx, cell, TITLES[section]);
}
#endif

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  const char *title = "";
  const char *sub = s_sub;
  s_sub[0] = '\0';
  char a[16], b[16];
  if (idx->section == SecRecording) {
    switch (idx->row) {
      case RowQuality:
        title = "Quality";
        snprintf(s_sub, sizeof(s_sub), "%s, %s kHz", QUALITY_NAMES[g_settings.quality],
                 g_settings.quality ? "16" : "8");
        break;
      case RowTarget:
        title = "Save to";
        sub = TARGET_NAMES[g_settings.target];
        break;
      case RowQuickLaunch:
        title = "Quick Launch";
        sub = g_settings.ql_autostart ? "Starts recording" : "Opens the recorder";
        break;
    }
  } else if (idx->section == SecStorage) {
    switch (idx->row) {
      case RowWatch: {
        title = "Watch";
        fmt_size(a, sizeof(a), storage_free());
        fmt_size(b, sizeof(b), storage_capacity());
        char left[12];
        fmt_duration(left, sizeof(left), storage_seconds_left(g_settings.quality ? 1 : 0));
        // The capacity doesn't fit beside the rest on the 144 px screen.
        if (BIG_SCREEN) snprintf(s_sub, sizeof(s_sub), "%s of %s free, %s", a, b, left);
        else snprintf(s_sub, sizeof(s_sub), "%s free, %s left", a, left);
        break;
      }
      case RowPhone:
        title = "Phone";
        if (comm_phone_ready()) {
          fmt_size(a, sizeof(a), g_settings.phone_free);
          char left[12];
          fmt_duration(left, sizeof(left),
                       g_settings.phone_free / adpcm_bytes_per_sec(g_settings.quality ? 1 : 0));
          snprintf(s_sub, sizeof(s_sub), "%s free, %s", a, left);
        } else {
          sub = "Not connected";
        }
        break;
      case RowCounts:
        title = "Recordings";
        snprintf(s_sub, sizeof(s_sub), "%d on watch, %d on phone", storage_local_count(),
                 storage_phone_count());
        break;
      case RowDeleteAll:
        title = "Delete all on watch";
        sub = storage_deleting() ? "Freeing space..." : "Phone copies stay";
        break;
    }
  } else {
    title = "Voice Recorder";
    snprintf(s_sub, sizeof(s_sub), "Version %s", APP_VERSION);
  }
  menu_cell_basic_draw(ctx, cell, title, sub[0] ? sub : NULL, NULL);
}

static void prv_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (idx->section == SecRecording) {
    switch (idx->row) {
      case RowQuality:
        prv_choose(QUALITY_NAMES, QUALITY_INFO, QualityCount, g_settings.quality, prv_quality_chosen);
        break;
      case RowTarget:
        prv_choose(TARGET_NAMES, TARGET_INFO, TargetCount, g_settings.target, prv_target_chosen);
        break;
      case RowQuickLaunch:
        g_settings.ql_autostart = !g_settings.ql_autostart;
        settings_changed_on_watch();
        menu_layer_reload_data(m);
        break;
    }
  } else if (idx->section == SecStorage && idx->row == RowDeleteAll) {
    if (storage_local_count() == 0) return;
    if (recorder_state() != RecIdle) {
      dialog_message("Stop the recording first.", RESOURCE_ID_DLG_MIC, ACCENT_DARK, 2500);
      return;
    }
    dialog_confirm("Delete every recording on the watch?", RESOURCE_ID_DLG_TRASH, ACCENT_COLOR,
                   prv_delete_all_answer, NULL);
  }
}

static void prv_comm_event(CommEvent ev, uint16_t id, const char *text) {
  if (s_menu) menu_layer_reload_data(s_menu);
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
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
