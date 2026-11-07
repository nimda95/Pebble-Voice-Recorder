#include "common.h"
#include "settings.h"
#include "storage.h"
#include "recorder.h"
#include "comm.h"
#include "player.h"
#include "ui.h"

static void prv_init(void) {
  settings_load();
  storage_init();
  recorder_init();
  comm_init();
  ui_recorder_push();
  if (launch_reason() == APP_LAUNCH_QUICK_LAUNCH && g_settings.ql_autostart) {
    ui_recorder_autostart();
  }
}

static void prv_deinit(void) {
  recorder_deinit();  // saves a recording that is still going
  player_close();
  comm_deinit();
  storage_deinit();
  settings_save();
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
