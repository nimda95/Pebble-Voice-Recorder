// Message and choice dialogs, laid out like the PebbleOS dialog patterns: a centred icon over
// bold text, on a coloured background on colour watches. Choice dialogs confirm with Up (check)
// and cancel with Down (cross) or Back.
#include "ui.h"

typedef struct {
  Window *window;
  BitmapLayer *icon_layer;
  TextLayer *text_layer;
  ActionBarLayer *action_bar;
  GBitmap *icon;
  GBitmap *check;
  GBitmap *cross;
  uint32_t icon_res;
  GColor background;
  bool is_choice;
  bool answered;
  bool confirmed;
  DialogConfirmCallback callback;
  void *context;
  AppTimer *timeout;
  char message[160];
} Dialog;

static void prv_finish(void *data) {
  Dialog *d = data;
  if (d->is_choice && d->callback) d->callback(d->confirmed, d->context);
  window_destroy(d->window);
  free(d);
}

static void prv_close(Dialog *d, bool confirmed) {
  if (d->answered) return;
  d->answered = true;
  d->confirmed = confirmed;
  window_stack_remove(d->window, true);
}

static void prv_up(ClickRecognizerRef r, void *ctx) { prv_close(ctx, true); }
static void prv_down(ClickRecognizerRef r, void *ctx) { prv_close(ctx, false); }
static void prv_any(ClickRecognizerRef r, void *ctx) { prv_close(ctx, false); }

static void prv_choice_clicks(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down);
}

static void prv_message_clicks(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_any);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_any);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_any);
}

static void prv_timeout(void *ctx) {
  Dialog *d = ctx;
  d->timeout = NULL;
  prv_close(d, false);
}

static void prv_load(Window *window) {
  Dialog *d = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, PBL_IF_COLOR_ELSE(d->background, GColorWhite));

  int16_t width = bounds.size.w - (d->is_choice ? ACTION_BAR_WIDTH : 0);
  const int16_t icon_size = 40;
  GFont font = fonts_get_system_font(BIG_SCREEN ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD);
  int16_t pad = PBL_IF_ROUND_ELSE(BIG_SCREEN ? 30 : 18, 8);
  GRect text_box = GRect(pad, 0, width - 2 * pad, bounds.size.h);
  GSize text = graphics_text_layout_get_content_size(d->message, font, text_box,
                                                     GTextOverflowModeWordWrap, GTextAlignmentCenter);
  int16_t block = icon_size + 6 + text.h;
  int16_t top = (bounds.size.h - block) / 2 - 6;
  if (top < 4) top = 4;

  d->icon = gbitmap_create_with_resource(d->icon_res);
  d->icon_layer = bitmap_layer_create(GRect((width - icon_size) / 2, top, icon_size, icon_size));
  bitmap_layer_set_bitmap(d->icon_layer, d->icon);
  bitmap_layer_set_compositing_mode(d->icon_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(d->icon_layer));

  d->text_layer = text_layer_create(GRect(pad, top + icon_size + 4, width - 2 * pad,
                                          bounds.size.h - (top + icon_size + 4)));
  text_layer_set_text(d->text_layer, d->message);
  text_layer_set_font(d->text_layer, font);
  text_layer_set_background_color(d->text_layer, GColorClear);
  text_layer_set_text_color(d->text_layer,
                            PBL_IF_COLOR_ELSE(gcolor_legible_over(d->background), GColorBlack));
  text_layer_set_text_alignment(d->text_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(d->text_layer));

  if (d->is_choice) {
    d->check = gbitmap_create_with_resource(RESOURCE_ID_AB_CHECK);
    d->cross = gbitmap_create_with_resource(RESOURCE_ID_AB_CROSS);
    d->action_bar = action_bar_layer_create();
    action_bar_layer_set_background_color(d->action_bar, GColorBlack);
    action_bar_layer_set_icon(d->action_bar, BUTTON_ID_UP, d->check);
    action_bar_layer_set_icon(d->action_bar, BUTTON_ID_DOWN, d->cross);
    action_bar_layer_set_context(d->action_bar, d);
    action_bar_layer_set_click_config_provider(d->action_bar, prv_choice_clicks);
    action_bar_layer_add_to_window(d->action_bar, window);
  } else {
    window_set_click_config_provider_with_context(window, prv_message_clicks, d);
  }
}

static void prv_unload(Window *window) {
  Dialog *d = window_get_user_data(window);
  if (d->timeout) app_timer_cancel(d->timeout);
  d->timeout = NULL;
  if (d->action_bar) action_bar_layer_destroy(d->action_bar);
  text_layer_destroy(d->text_layer);
  bitmap_layer_destroy(d->icon_layer);
  gbitmap_destroy(d->icon);
  if (d->check) gbitmap_destroy(d->check);
  if (d->cross) gbitmap_destroy(d->cross);
  d->answered = true;
  // Run the callback once the window is fully off the stack.
  app_timer_register(0, prv_finish, d);
}

static Dialog *prv_create(const char *message, uint32_t icon, GColor bg) {
  Dialog *d = calloc(1, sizeof(Dialog));
  if (!d) return NULL;
  strncpy(d->message, message, sizeof(d->message) - 1);
  d->icon_res = icon;
  d->background = bg;
  d->window = window_create();
  window_set_user_data(d->window, d);
  window_set_window_handlers(d->window, (WindowHandlers){ .load = prv_load, .unload = prv_unload });
  return d;
}

void dialog_confirm(const char *message, uint32_t icon_resource, GColor background,
                    DialogConfirmCallback callback, void *context) {
  Dialog *d = prv_create(message, icon_resource, background);
  if (!d) {
    if (callback) callback(false, context);
    return;
  }
  d->is_choice = true;
  d->callback = callback;
  d->context = context;
  window_stack_push(d->window, true);
}

void dialog_message(const char *message, uint32_t icon_resource, GColor background,
                    uint32_t timeout_ms) {
  Dialog *d = prv_create(message, icon_resource, background);
  if (!d) return;
  window_stack_push(d->window, true);
  if (timeout_ms) d->timeout = app_timer_register(timeout_ms, prv_timeout, d);
}
