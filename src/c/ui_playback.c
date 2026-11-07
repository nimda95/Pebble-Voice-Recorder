// Playback. With a transcript: one row per timed segment; the highlight follows the audio, Up and
// Down move between segments (and seek while playing), Select plays or pauses. Without one: a
// player with Up = back 10 s, Select = play / pause, Down = forward 10 s.
#include "ui.h"
#include "storage.h"
#include "comm.h"
#include "player.h"

#if BIG_SCREEN
  #define MAX_TEXT_BYTES 8192
#else
  #define MAX_TEXT_BYTES 3072
#endif
#define MAX_SEGMENTS 160
#define LOAD_TIMEOUT_MS 20000
#define SKIP_MS 10000

typedef struct {
  uint32_t start_ms;
  char *text;
  int16_t height;
} Segment;

static Window *s_window;
static StatusBarLayer *s_status_bar;
static MenuLayer *s_menu;
static TextLayer *s_info;
static uint16_t s_id;
static bool s_with_transcript;
static bool s_autoplay;
static bool s_player_ready;
static char s_info_buf[96];

// Transcript
static Segment *s_segs;
static uint16_t s_seg_count;
static uint16_t s_seg_cap;
static size_t s_text_bytes;
static bool s_estimated;
static bool s_loaded;
static bool s_truncated;
static int s_follow = -1;
static bool s_programmatic;
static AppTimer *s_load_timer;

// Plain player
static ActionBarLayer *s_action_bar;
static GBitmap *s_icon_play, *s_icon_pause, *s_icon_rewind, *s_icon_forward;
static TextLayer *s_title_layer, *s_time_layer, *s_total_layer, *s_state_layer;
static char s_title_buf[REC_NAME_LEN + 4], s_time_buf[12], s_total_buf[20];

static AppTimer *s_ui_timer;
static char s_header_buf[40];

static GFont prv_text_font(void) {
  return fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18);
}
static GFont prv_time_font(void) {
  return fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD);
}
#define ROW_PAD PBL_IF_ROUND_ELSE(BIG_SCREEN ? 30 : 18, 6)
#define TIME_H (BIG_SCREEN ? 20 : 16)

static bool prv_ensure_player(void) {
  if (s_player_ready) return true;
  if (!player_available()) return false;
  s_player_ready = player_open(s_id);
  if (s_player_ready && player_muted()) {
    dialog_message("The speaker is muted in Settings or by Quiet Time.", RESOURCE_ID_DLG_WARNING,
                   ACCENT_DARK, 3000);
  }
  if (!s_player_ready) {
    const char *err = player_error();
    dialog_message(err ? err : "Can't play this recording.", RESOURCE_ID_DLG_PHONE, ACCENT_DARK, 3000);
  }
  return s_player_ready;
}

static bool prv_playing(void) {
  PlayerState st = player_state();
  return s_player_ready && (st == PlayerPlaying || st == PlayerBuffering);
}

static void prv_free_transcript(void) {
  for (int i = 0; i < s_seg_count; i++) free(s_segs[i].text);
  free(s_segs);
  s_segs = NULL;
  s_seg_count = 0;
  s_seg_cap = 0;
  s_text_bytes = 0;
}

static void prv_show_info(const char *text) {
  if (!s_info) return;
  strncpy(s_info_buf, text, sizeof(s_info_buf) - 1);
  text_layer_set_text(s_info, s_info_buf);
  layer_set_hidden(text_layer_get_layer(s_info), false);
  layer_set_hidden(menu_layer_get_layer(s_menu), true);
}

// ---- Transcript rows ----------------------------------------------------------------------------

static uint16_t prv_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return s_seg_count + (s_truncated ? 1 : 0);
}

static int16_t prv_header_height(MenuLayer *m, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  char pos[12], dur[12];
  RecMeta *m = storage_find(s_id);
  uint32_t total = m ? storage_duration_ms(m) : 0;
  fmt_duration(dur, sizeof(dur), (total + 500) / 1000);
  if (s_player_ready) {
    fmt_duration(pos, sizeof(pos), player_position_ms() / 1000);
    const char *st = player_state() == PlayerPlaying ? "Playing" :
                     player_state() == PlayerBuffering ? "Loading" : "Paused";
    snprintf(s_header_buf, sizeof(s_header_buf), "%s %s / %s", st, pos, dur);
  } else {
    snprintf(s_header_buf, sizeof(s_header_buf), "%s%s", dur,
             player_available() ? ", Select to play" : "");
  }
#if defined(PBL_ROUND)
  // The basic header is left-aligned, which the round screen cuts off.
  GRect b = layer_get_bounds(cell);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_header_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(0, -2, b.size.w, b.size.h + 2), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
#else
  menu_cell_basic_header_draw(ctx, cell, s_header_buf);
#endif
}

static int16_t prv_row_height(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (idx->row >= s_seg_count) return BIG_SCREEN ? 40 : 32;
  return s_segs[idx->row].height;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  GRect b = layer_get_bounds(cell);
  bool hl = menu_cell_layer_is_highlighted(cell);
  graphics_context_set_text_color(ctx, hl ? GColorWhite : GColorBlack);
  GTextAlignment align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  if (idx->row >= s_seg_count) {
    graphics_draw_text(ctx, "Rest of the text is on the phone", prv_time_font(),
                       GRect(ROW_PAD, 4, b.size.w - 2 * ROW_PAD, b.size.h - 4),
                       GTextOverflowModeWordWrap, align, NULL);
    return;
  }
  const Segment *seg = &s_segs[idx->row];
  char ts[16];
  char d[12];
  fmt_duration(d, sizeof(d), seg->start_ms / 1000);
  snprintf(ts, sizeof(ts), "%s%s", s_estimated ? "~" : "", d);
  graphics_draw_text(ctx, ts, prv_time_font(), GRect(ROW_PAD, 0, b.size.w - 2 * ROW_PAD, TIME_H),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
  graphics_draw_text(ctx, seg->text, prv_text_font(),
                     GRect(ROW_PAD, TIME_H - 2, b.size.w - 2 * ROW_PAD, b.size.h - TIME_H),
                     GTextOverflowModeWordWrap, align, NULL);
}

static int prv_segment_at(uint32_t ms) {
  int found = 0;
  for (int i = 0; i < s_seg_count; i++) {
    if (s_segs[i].start_ms <= ms) found = i;
    else break;
  }
  return found;
}

static void prv_follow(void) {
  if (!s_menu || !s_seg_count || !prv_playing()) return;
  int idx = prv_segment_at(player_position_ms());
  if (idx != s_follow) {
    s_follow = idx;
    s_programmatic = true;
    menu_layer_set_selected_index(s_menu, MenuIndex(0, idx), MenuRowAlignTop, true);
    s_programmatic = false;
  }
}

static void prv_row_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (!player_available()) return;
  if (prv_playing()) {
    player_pause();
    return;
  }
  if (!prv_ensure_player()) return;
  if (idx->row < s_seg_count && (int)idx->row != s_follow) {
    player_seek_ms(s_segs[idx->row].start_ms);
  }
  s_follow = idx->row;
  player_play();
}

static void prv_selection_changed(MenuLayer *m, MenuIndex now, MenuIndex before, void *ctx) {
  if (s_programmatic || !prv_playing() || now.row >= s_seg_count) return;
  s_follow = now.row;
  player_seek_ms(s_segs[now.row].start_ms);
}

// ---- Transcript loading -------------------------------------------------------------------------

static void prv_tr_begin(uint16_t id, uint16_t count, bool estimated) {
  if (id != s_id) return;
  prv_free_transcript();
  s_estimated = estimated;
  s_truncated = false;
  s_seg_cap = count > MAX_SEGMENTS ? MAX_SEGMENTS : (count ? count : 1);
  s_segs = calloc(s_seg_cap, sizeof(Segment));
  if (!s_segs) s_seg_cap = 0;
}

static void prv_tr_segment(uint16_t id, uint16_t index, uint32_t start_ms, const char *text) {
  if (id != s_id || !s_segs || !s_window) return;
  size_t len = strlen(text);
  if (s_seg_count >= s_seg_cap || s_text_bytes + len + 1 > MAX_TEXT_BYTES) {
    s_truncated = true;
    return;
  }
  char *copy = malloc(len + 1);
  if (!copy) {
    s_truncated = true;
    return;
  }
  memcpy(copy, text, len + 1);
  Segment *seg = &s_segs[s_seg_count++];
  seg->start_ms = start_ms;
  seg->text = copy;
  s_text_bytes += len + 1;
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  GSize size = graphics_text_layout_get_content_size(
      copy, prv_text_font(), GRect(0, 0, b.size.w - 2 * ROW_PAD, 2000), GTextOverflowModeWordWrap,
      PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  seg->height = TIME_H + size.h + (BIG_SCREEN ? 10 : 8);
}

static void prv_tr_end(uint16_t id) {
  if (id != s_id || !s_window) return;
  if (s_load_timer) app_timer_cancel(s_load_timer);
  s_load_timer = NULL;
  s_loaded = true;
  if (s_seg_count == 0) {
    prv_show_info("The transcript is empty.");
    return;
  }
  layer_set_hidden(text_layer_get_layer(s_info), true);
  layer_set_hidden(menu_layer_get_layer(s_menu), false);
  menu_layer_reload_data(s_menu);
  if (s_autoplay && prv_ensure_player()) {
    s_follow = 0;
    player_play();
  }
}

static void prv_tr_fail(uint16_t id, const char *reason) {
  if (id != s_id) return;
  if (s_load_timer) app_timer_cancel(s_load_timer);
  s_load_timer = NULL;
  prv_show_info(reason && reason[0] ? reason : "No transcript for this recording.");
}

static const TranscriptSink SINK = {
  .begin = prv_tr_begin,
  .segment = prv_tr_segment,
  .end = prv_tr_end,
  .fail = prv_tr_fail,
};

static void prv_load_timeout(void *ctx) {
  s_load_timer = NULL;
  if (!s_loaded) prv_show_info("The phone didn't send the transcript. Try again.");
}

// ---- Plain player -------------------------------------------------------------------------------

static void prv_update_player(void) {
  if (!s_time_layer) return;
  fmt_duration(s_time_buf, sizeof(s_time_buf), player_position_ms() / 1000);
  text_layer_set_text(s_time_layer, s_time_buf);
  const char *st = "Paused";
  switch (player_state()) {
    case PlayerPlaying: st = "Playing"; break;
    case PlayerBuffering: st = "Loading from phone"; break;
    case PlayerEnded: st = "Finished"; break;
    default: break;
  }
  text_layer_set_text(s_state_layer, s_player_ready ? st : "Can't play");
  action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_SELECT,
                                     prv_playing() ? s_icon_pause : s_icon_play, true);
}

static void prv_player_up(ClickRecognizerRef r, void *ctx) { if (s_player_ready) player_skip_ms(-SKIP_MS); }
static void prv_player_select(ClickRecognizerRef r, void *ctx) { if (s_player_ready) player_toggle(); }
static void prv_player_down(ClickRecognizerRef r, void *ctx) { if (s_player_ready) player_skip_ms(SKIP_MS); }

static void prv_player_clicks(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 400, prv_player_up);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_player_select);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 400, prv_player_down);
}

static TextLayer *prv_text(Layer *root, GRect frame, GFont font) {
  TextLayer *t = text_layer_create(frame);
  text_layer_set_font(t, font);
  text_layer_set_background_color(t, GColorClear);
  text_layer_set_text_alignment(t, GTextAlignmentCenter);
  text_layer_set_overflow_mode(t, GTextOverflowModeTrailingEllipsis);
  layer_add_child(root, text_layer_get_layer(t));
  return t;
}

static void prv_build_player(Layer *root, GRect b) {
  s_icon_play = gbitmap_create_with_resource(RESOURCE_ID_AB_PLAY);
  s_icon_pause = gbitmap_create_with_resource(RESOURCE_ID_AB_PAUSE);
  s_icon_rewind = gbitmap_create_with_resource(RESOURCE_ID_AB_REWIND);
  s_icon_forward = gbitmap_create_with_resource(RESOURCE_ID_AB_FORWARD);
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_icon_rewind);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_play);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_icon_forward);
  action_bar_layer_set_click_config_provider(s_action_bar, prv_player_clicks);
  action_bar_layer_add_to_window(s_action_bar, s_window);

  int16_t w = b.size.w - ACTION_BAR_WIDTH;
  int16_t pad = PBL_IF_ROUND_ELSE(BIG_SCREEN ? 22 : 12, 4);
  int16_t y = STATUS_BAR_LAYER_HEIGHT + (b.size.h - STATUS_BAR_LAYER_HEIGHT - (BIG_SCREEN ? 150 : 118)) / 2;
  RecMeta *m = storage_find(s_id);
  fmt_title(s_title_buf, sizeof(s_title_buf), m ? m->name : NULL, m ? m->created : 0);
  s_title_layer = prv_text(root, GRect(pad, y, w - 2 * pad, BIG_SCREEN ? 30 : 24),
                           fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_title_layer, s_title_buf);
  y += BIG_SCREEN ? 30 : 24;
  s_time_layer = prv_text(root, GRect(pad, y, w - 2 * pad, BIG_SCREEN ? 50 : 42),
                          fonts_get_system_font(BIG_SCREEN ? FONT_KEY_LECO_42_NUMBERS : FONT_KEY_LECO_36_BOLD_NUMBERS));
  y += BIG_SCREEN ? 52 : 44;
  char dur[12];
  fmt_duration(dur, sizeof(dur), m ? (storage_duration_ms(m) + 500) / 1000 : 0);
  snprintf(s_total_buf, sizeof(s_total_buf), "of %s", dur);
  s_total_layer = prv_text(root, GRect(pad, y, w - 2 * pad, BIG_SCREEN ? 30 : 24),
                           fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_total_layer, s_total_buf);
  y += BIG_SCREEN ? 30 : 24;
  s_state_layer = prv_text(root, GRect(pad, y, w - 2 * pad, BIG_SCREEN ? 30 : 24),
                           fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_color(s_state_layer, ACCENT_COLOR);
}

// ---- Window -------------------------------------------------------------------------------------

static void prv_ui_tick(void *ctx) {
  s_ui_timer = app_timer_register(prv_playing() ? 250 : 1000, prv_ui_tick, NULL);
  if (s_with_transcript) {
    prv_follow();
    if (s_menu) layer_mark_dirty(menu_layer_get_layer(s_menu));
  } else {
    prv_update_player();
  }
}

static void prv_player_changed(void) {
  if (s_with_transcript) {
    if (s_menu) layer_mark_dirty(menu_layer_get_layer(s_menu));
  } else {
    prv_update_player();
  }
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_player_ready = false;
  s_loaded = false;
  s_follow = -1;

  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorWhite, GColorBlack);
  status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
  if (!s_with_transcript) {
    layer_set_frame(status_bar_layer_get_layer(s_status_bar),
                    GRect(0, 0, b.size.w - PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH, 0), STATUS_BAR_LAYER_HEIGHT));
    status_bar_layer_set_colors(s_status_bar, GColorClear, GColorBlack);
  }
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));
  player_set_listener(prv_player_changed);

  if (s_with_transcript) {
    s_menu = menu_layer_create(GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w, b.size.h - STATUS_BAR_LAYER_HEIGHT));
    menu_layer_set_highlight_colors(s_menu, ACCENT_COLOR, GColorWhite);
    menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
      .get_num_rows = prv_rows,
      .get_header_height = prv_header_height,
      .draw_header = prv_draw_header,
      .get_cell_height = prv_row_height,
      .draw_row = prv_draw_row,
      .select_click = prv_row_select,
      .selection_changed = prv_selection_changed,
    });
    menu_layer_set_click_config_onto_window(s_menu, window);
    layer_add_child(root, menu_layer_get_layer(s_menu));

    s_info = text_layer_create(GRect(ROW_PAD + 4, b.size.h / 2 - 40, b.size.w - 2 * ROW_PAD - 8, 90));
    text_layer_set_font(s_info, fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_info, GTextAlignmentCenter);
    layer_add_child(root, text_layer_get_layer(s_info));

    comm_set_transcript_sink(&SINK);
    if (comm_phone_ready()) {
      prv_show_info("Loading transcript...");
      comm_request_transcript(s_id);
      s_load_timer = app_timer_register(LOAD_TIMEOUT_MS, prv_load_timeout, NULL);
    } else {
      prv_show_info("Connect your phone to read the transcript.");
    }
  } else {
    prv_build_player(root, b);
    prv_ensure_player();
    if (s_autoplay && s_player_ready) player_play();
    prv_update_player();
  }
  s_ui_timer = app_timer_register(250, prv_ui_tick, NULL);
}

static void prv_unload(Window *window) {
  if (s_ui_timer) app_timer_cancel(s_ui_timer);
  if (s_load_timer) app_timer_cancel(s_load_timer);
  s_ui_timer = s_load_timer = NULL;
  player_set_listener(NULL);
  player_close();
  comm_set_transcript_sink(NULL);
  prv_free_transcript();
  if (s_menu) menu_layer_destroy(s_menu);
  if (s_info) text_layer_destroy(s_info);
  s_menu = NULL;
  s_info = NULL;
  if (s_action_bar) {
    action_bar_layer_destroy(s_action_bar);
    text_layer_destroy(s_title_layer);
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_total_layer);
    text_layer_destroy(s_state_layer);
    gbitmap_destroy(s_icon_play);
    gbitmap_destroy(s_icon_pause);
    gbitmap_destroy(s_icon_rewind);
    gbitmap_destroy(s_icon_forward);
    s_action_bar = NULL;
    s_time_layer = NULL;
  }
  status_bar_layer_destroy(s_status_bar);
  window_destroy(s_window);
  s_window = NULL;
}

void ui_playback_push(uint16_t id, bool with_transcript, bool autoplay) {
  if (!with_transcript && !player_available()) {
    dialog_message("This watch has no speaker.", RESOURCE_ID_DLG_WARNING, ACCENT_DARK, 2500);
    return;
  }
  s_id = id;
  s_with_transcript = with_transcript;
  s_autoplay = autoplay && player_available();
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = prv_load, .unload = prv_unload });
  window_stack_push(s_window, true);
}
