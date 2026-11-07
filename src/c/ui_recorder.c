// The recorder: the app opens straight to it.
//   Standby:   Up = settings, Select = record, Down = recordings
//   Recording: Up = stop & save, Select = pause, Down = discard (asks first)
//   Paused:    Up = stop & save, Select = continue, Down = discard (asks first)
#include "ui.h"
#include "recorder.h"
#include "storage.h"
#include "settings.h"
#include "comm.h"

static Window *s_window;
static StatusBarLayer *s_status_bar;
static ActionBarLayer *s_action_bar;
static TextLayer *s_state_layer;
static TextLayer *s_timer_layer;
static TextLayer *s_health_layer;
static TextLayer *s_free_layer;
static TextLayer *s_left_layer;

static GBitmap *s_icon_record, *s_icon_pause, *s_icon_stop, *s_icon_trash, *s_icon_list,
    *s_icon_settings;

static char s_timer_buf[12];
static char s_health_buf[48];
static char s_free_buf[64];
static char s_left_buf[40];

static RecState s_shown_state = (RecState)0xff;
static AppTimer *s_idle_timer;
static AppTimer *s_flash_timer;
static const char *s_flash;           // replaces the state line for a moment ("Saved")
static uint32_t s_flash_elapsed;
static bool s_resume_after_confirm;
static bool s_hours_font;
static GFont s_small_font;  // the font of the free and left lines

static GFont prv_timer_font(bool hours) {
  if (hours) return fonts_get_system_font(BIG_SCREEN ? FONT_KEY_LECO_32_BOLD_NUMBERS
                                                     : FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);
  return fonts_get_system_font(BIG_SCREEN ? FONT_KEY_LECO_42_NUMBERS : FONT_KEY_LECO_36_BOLD_NUMBERS);
}

// ---- Layout -------------------------------------------------------------------------------------

static void prv_layout(void) {
  if (!s_window) return;
  Layer *root = window_get_root_layer(s_window);
  GRect full = layer_get_unobstructed_bounds(root);
  int16_t top = STATUS_BAR_LAYER_HEIGHT;
  int16_t width = full.size.w - ACTION_BAR_WIDTH;
  int16_t avail = full.size.h - top;

  const int16_t state_h = BIG_SCREEN ? 34 : 28;
  const int16_t timer_h = BIG_SCREEN ? 50 : 42;
  const int16_t line_h = BIG_SCREEN ? 28 : 21;
  bool compact = avail < state_h + timer_h + 3 * line_h + 4;
  int16_t lines = compact ? 2 : 3;
  int16_t block = state_h + timer_h + lines * line_h;
  int16_t y = top + (avail - block) / 2;
  if (y < top) y = top;
  int16_t pad = PBL_IF_ROUND_ELSE(BIG_SCREEN ? 18 : 10, 4);
  int16_t x = pad;
  int16_t w = width - 2 * pad;

  layer_set_frame(text_layer_get_layer(s_state_layer), GRect(x, y, w, state_h));
  y += state_h - (BIG_SCREEN ? 6 : 4);
  layer_set_frame(text_layer_get_layer(s_timer_layer), GRect(x, y, w, timer_h + 4));
  y += timer_h + (BIG_SCREEN ? 6 : 4);
  layer_set_frame(text_layer_get_layer(s_health_layer), GRect(x, y, w, line_h));
  y += line_h;
  layer_set_frame(text_layer_get_layer(s_free_layer), GRect(x, y, w, line_h));
  y += line_h;
  layer_set_frame(text_layer_get_layer(s_left_layer), GRect(x, y, w, line_h));
  layer_set_hidden(text_layer_get_layer(s_left_layer), compact);
}

// ---- Content ------------------------------------------------------------------------------------

static GColor prv_health_color(HealthLevel level) {
#if defined(PBL_COLOR)
  switch (level) {
    case HealthBad: return GColorRed;
    case HealthWarn: return GColorWindsorTan;
    default: return GColorIslamicGreen;
  }
#else
  return GColorBlack;
#endif
}

static bool prv_fits(const char *text, GFont font, int16_t width) {
  GSize size = graphics_text_layout_get_content_size(text, font, GRect(0, 0, 400, 60),
                                                     GTextOverflowModeWordWrap, GTextAlignmentCenter);
  return size.w <= width;
}

static void prv_update_action_bar(RecState state, bool animated) {
  if (state == RecIdle) {
    action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_UP, s_icon_settings, animated);
    action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_SELECT, s_icon_record, animated);
    action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_DOWN, s_icon_list, animated);
  } else {
    action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_UP, s_icon_stop, animated);
    action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_SELECT,
                                       state == RecPaused ? s_icon_record : s_icon_pause, animated);
    action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_DOWN, s_icon_trash, animated);
  }
}

static void prv_update(void) {
  if (!s_window) return;
  RecState state = recorder_state();
  if (state != s_shown_state) {
    prv_update_action_bar(state, s_shown_state != (RecState)0xff);
    s_shown_state = state;
  }

  // State line
  const char *label;
  GColor label_color = GColorBlack;
  switch (state) {
    case RecRecording: label = tr(S_RECORDING); label_color = ACCENT_COLOR; break;
    case RecPaused: label = tr(S_PAUSED); break;
    case RecStarting: label = tr(S_STARTING); label_color = ACCENT_COLOR; break;
    default: label = s_flash ? s_flash : tr(S_READY); break;
  }
  text_layer_set_text(s_state_layer, label);
  text_layer_set_text_color(s_state_layer, label_color);

  // Timer
  uint32_t elapsed = state == RecIdle ? (s_flash ? s_flash_elapsed : 0) : recorder_elapsed_ms();
  bool hours = elapsed >= 3600000;
  if (hours != s_hours_font) {
    s_hours_font = hours;
    text_layer_set_font(s_timer_layer, prv_timer_font(hours));
  }
  fmt_duration(s_timer_buf, sizeof(s_timer_buf), elapsed / 1000);
  text_layer_set_text(s_timer_layer, s_timer_buf);

  // Health (while recording) or where the next recording goes
  bool phone = recorder_uses_phone();
  if (state == RecIdle) {
    const char *where = phone ? tr(S_SAVING_TO_PHONE)
                      : g_settings.target == TargetPhone ? tr(S_PHONE_OFFLINE) : tr(S_SAVING_TO_WATCH);
    snprintf(s_health_buf, sizeof(s_health_buf), "%s", where);
    text_layer_set_text_color(s_health_layer, GColorBlack);
    text_layer_set_font(s_health_layer, fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18));
  } else {
    HealthReason reason;
    HealthLevel level = recorder_health(&reason);
    GFont bold = fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD);
    GFont font = bold;
    if (reason == HrPaused) {
      snprintf(s_health_buf, sizeof(s_health_buf), "%s", tr(S_MIC_PAUSED));
    } else {
      // "Health: Dropping audio" is too wide for most screens; the reason alone, in the
      // warning colour, says the same.
      const char *text = recorder_health_text(reason);
      int16_t w = layer_get_frame(text_layer_get_layer(s_health_layer)).size.w;
      snprintf(s_health_buf, sizeof(s_health_buf), tr(S_HEALTH_FMT), text);
      if (!prv_fits(s_health_buf, bold, w)) {
        snprintf(s_health_buf, sizeof(s_health_buf), "%s", text);
        if (!prv_fits(s_health_buf, bold, w)) {
          font = fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18);
        }
      }
    }
    text_layer_set_text_color(s_health_layer, prv_health_color(level));
    text_layer_set_font(s_health_layer, font);
  }
  text_layer_set_text(s_health_layer, s_health_buf);

  // Space and time left
  char size[16], left[12];
  fmt_size(size, sizeof(size), recorder_free_bytes());
  fmt_duration(left, sizeof(left), recorder_seconds_left());
  bool compact = layer_get_hidden(text_layer_get_layer(s_left_layer));
  if (compact) {
    snprintf(s_free_buf, sizeof(s_free_buf), tr(S_SIZE_LEFT_FMT), size, left);
    // Some languages' wording doesn't fit the 144 px screen; the numbers alone still read.
    if (!prv_fits(s_free_buf, s_small_font, layer_get_frame(text_layer_get_layer(s_free_layer)).size.w)) {
      snprintf(s_free_buf, sizeof(s_free_buf), "%s, %s", size, left);
    }
  } else {
    snprintf(s_free_buf, sizeof(s_free_buf), tr(phone ? S_FREE_ON_PHONE_FMT : S_FREE_FMT), size);
    snprintf(s_left_buf, sizeof(s_left_buf), tr(S_LEFT_FMT), left);
  }
  text_layer_set_text(s_free_layer, s_free_buf);
  text_layer_set_text(s_left_layer, s_left_buf);
}

static void prv_show_notice(void) {
  const char *notice = recorder_take_notice();
  if (notice) {
    dialog_message(notice, RESOURCE_ID_DLG_WARNING, PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite), 3500);
  }
}

static void prv_recorder_changed(void) {
  prv_update();
  if (window_stack_get_top_window() == s_window) prv_show_notice();
}

static void prv_flash_done(void *ctx) {
  s_flash_timer = NULL;
  s_flash = NULL;
  prv_update();
}

static void prv_flash(const char *text, uint32_t elapsed) {
  s_flash = text;
  s_flash_elapsed = elapsed;
  if (s_flash_timer) app_timer_cancel(s_flash_timer);
  s_flash_timer = app_timer_register(2000, prv_flash_done, NULL);
  prv_update();
}

static void prv_idle_tick(void *ctx) {
  s_idle_timer = app_timer_register(5000, prv_idle_tick, NULL);
  if (recorder_state() == RecIdle) prv_update();
}

static void prv_comm_event(CommEvent ev, uint16_t id, const char *text) {
  if (ev == CommEvPhone || ev == CommEvList) prv_update();
}

// ---- Actions ------------------------------------------------------------------------------------

static void prv_start(void) {
  if (recorder_start()) {
    prv_show_notice();
    return;
  }
  const char *err = recorder_error();
  dialog_message(err ? err : tr(S_ERR_START), RESOURCE_ID_DLG_MIC, ACCENT_DARK, 5000);
}

static void prv_save(void) {
  uint32_t elapsed = recorder_elapsed_ms();
  recorder_stop_save();
  vibes_short_pulse();
  prv_flash(tr(S_SAVED), elapsed);
  prv_show_notice();
}

static void prv_discard_answer(bool confirmed, void *ctx) {
  if (confirmed) {
    recorder_discard();
    prv_flash(tr(S_DISCARDED), 0);
  } else if (s_resume_after_confirm) {
    recorder_resume();
  }
  s_resume_after_confirm = false;
}

static void prv_ask_discard(void) {
  s_resume_after_confirm = recorder_state() == RecRecording || recorder_state() == RecStarting;
  recorder_pause();
  dialog_confirm(tr(S_ASK_DISCARD), RESOURCE_ID_DLG_TRASH, ACCENT_COLOR,
                 prv_discard_answer, NULL);
}

static void prv_up(ClickRecognizerRef r, void *ctx) {
  if (recorder_state() == RecIdle) ui_settings_push();
  else prv_save();
}

static void prv_select(ClickRecognizerRef r, void *ctx) {
  switch (recorder_state()) {
    case RecIdle: prv_start(); break;
    case RecRecording:
    case RecStarting: recorder_pause(); break;
    case RecPaused: recorder_resume(); break;
  }
}

static void prv_down(ClickRecognizerRef r, void *ctx) {
  if (recorder_state() == RecIdle) ui_list_push();
  else prv_ask_discard();
}

static void prv_clicks(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_up);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down);
}

static void prv_unobstructed_change(AnimationProgress progress, void *ctx) {
  prv_layout();
  prv_update();
}

// ---- Window -------------------------------------------------------------------------------------

static TextLayer *prv_text(Layer *root, GFont font) {
  TextLayer *t = text_layer_create(GRect(0, 0, 10, 10));
  text_layer_set_font(t, font);
  text_layer_set_background_color(t, GColorClear);
  text_layer_set_text_color(t, GColorBlack);
  text_layer_set_text_alignment(t, GTextAlignmentCenter);
  text_layer_set_overflow_mode(t, GTextOverflowModeTrailingEllipsis);
  layer_add_child(root, text_layer_get_layer(t));
  return t;
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_icon_record = gbitmap_create_with_resource(RESOURCE_ID_AB_RECORD);
  s_icon_pause = gbitmap_create_with_resource(RESOURCE_ID_AB_PAUSE);
  s_icon_stop = gbitmap_create_with_resource(RESOURCE_ID_AB_STOP);
  s_icon_trash = gbitmap_create_with_resource(RESOURCE_ID_AB_TRASH);
  s_icon_list = gbitmap_create_with_resource(RESOURCE_ID_AB_LIST);
  s_icon_settings = gbitmap_create_with_resource(RESOURCE_ID_AB_SETTINGS);

  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorClear, GColorBlack);
  status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_set_frame(status_bar_layer_get_layer(s_status_bar),
                  GRect(0, 0, bounds.size.w - PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH, 0),
                        STATUS_BAR_LAYER_HEIGHT));
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));

  s_state_layer = prv_text(root, fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD));
  s_timer_layer = prv_text(root, prv_timer_font(false));
  s_health_layer = prv_text(root, fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18));
  s_small_font = fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18);
  s_free_layer = prv_text(root, s_small_font);
  s_left_layer = prv_text(root, fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18));

  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  action_bar_layer_set_click_config_provider(s_action_bar, prv_clicks);
  action_bar_layer_add_to_window(s_action_bar, window);

  s_shown_state = (RecState)0xff;
  prv_layout();
  prv_update();

  unobstructed_area_service_subscribe((UnobstructedAreaHandlers){ .change = prv_unobstructed_change }, NULL);
  recorder_set_listener(prv_recorder_changed);
  comm_add_listener(prv_comm_event);
  s_idle_timer = app_timer_register(5000, prv_idle_tick, NULL);
}

static void prv_appear(Window *window) {
  prv_update();
  prv_show_notice();
}

static void prv_unload(Window *window) {
  unobstructed_area_service_unsubscribe();
  recorder_set_listener(NULL);
  comm_remove_listener(prv_comm_event);
  if (s_idle_timer) app_timer_cancel(s_idle_timer);
  if (s_flash_timer) app_timer_cancel(s_flash_timer);
  s_idle_timer = s_flash_timer = NULL;
  action_bar_layer_destroy(s_action_bar);
  status_bar_layer_destroy(s_status_bar);
  text_layer_destroy(s_state_layer);
  text_layer_destroy(s_timer_layer);
  text_layer_destroy(s_health_layer);
  text_layer_destroy(s_free_layer);
  text_layer_destroy(s_left_layer);
  gbitmap_destroy(s_icon_record);
  gbitmap_destroy(s_icon_pause);
  gbitmap_destroy(s_icon_stop);
  gbitmap_destroy(s_icon_trash);
  gbitmap_destroy(s_icon_list);
  gbitmap_destroy(s_icon_settings);
  window_destroy(s_window);
  s_window = NULL;
}

void ui_recorder_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_load,
    .appear = prv_appear,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}

void ui_recorder_bring_to_front(void) {
  if (!s_window) return;
  while (window_stack_get_top_window() && window_stack_get_top_window() != s_window) {
    window_stack_pop(false);
  }
}

static void prv_autostart_go(void *ctx) {
  if (recorder_state() == RecIdle) prv_start();
}

static void prv_autostart_buzz(void *ctx) {
  vibes_short_pulse();
  // Start once the buzz is over so it isn't in the recording.
  app_timer_register(350, prv_autostart_go, NULL);
}

void ui_recorder_autostart(void) {
  app_timer_register(300, prv_autostart_buzz, NULL);
}
