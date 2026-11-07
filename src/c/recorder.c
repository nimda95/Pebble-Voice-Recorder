#include "recorder.h"
#include "storage.h"
#include "settings.h"
#include "comm.h"
#include "adpcm.h"

#define STREAM_START_TIMEOUT_MS 6000
#define FRAME_MS 20                  // the mic delivers 320 samples (20 ms) per callback
#define LOSS_SHOW_MS 5000            // how long "Dropping audio" stays up after a loss
#define SLOW_WRITE_MS 700            // a write this slow risks overrunning the mic's buffer
#define LOW_SECONDS 20

static struct {
  RecState state;
  RecMode mode;
  uint16_t id;
  uint8_t rate_code;
  bool full;
  bool auto_paused;

  uint32_t elapsed_before;   // ms recorded in earlier segments (before pauses)
  int32_t seg_start;         // now_ms() at the start of the current segment

  // Health, local capture: frames received against frames the clock says were due.
  uint32_t frames;
  uint8_t ticks;
  int32_t baseline;
  int32_t prev_deficit;
  int32_t confirmed_max;
  uint32_t lost_frames;
  int32_t last_loss_ms;
  int32_t worst_write;
  int32_t worst_write_at;

  // Health, phone stream: how much audio the phone reports having.
  uint32_t phone_received_ms;
  int32_t last_stat_ms;
  uint32_t phone_free_at_start;

  AdpcmEncoder enc;
  Decimator dec;

  AppTimer *tick_timer;
  AppTimer *start_timer;
  AppTimer *stop_timer;
  uint8_t ticks_since_state;

  const char *error;
  const char *notice;
  RecorderListener listener;
} s;

static bool prv_start_local(void);
static void prv_stream_lost(void);

// ---- Helpers ------------------------------------------------------------------------------------

static void prv_notify(bool state_changed) {
  if (s.listener) s.listener();
  if (state_changed) comm_send_rec_state();
}

static void prv_cancel(AppTimer **t) {
  if (*t) {
    app_timer_cancel(*t);
    *t = NULL;
  }
}

static void prv_reset(void) {
  prv_cancel(&s.tick_timer);
  prv_cancel(&s.start_timer);
  prv_cancel(&s.stop_timer);
  s.state = RecIdle;
  s.mode = ModeLocal;
  s.id = 0;
  s.full = false;
  s.auto_paused = false;
  s.elapsed_before = 0;
  s.lost_frames = 0;
  s.last_loss_ms = 0;
  s.phone_received_ms = 0;
  storage_set_busy(BusyRecording, false);
}

static uint16_t prv_alloc_id(void) {
  uint16_t id = g_settings.next_id;
  for (int guard = 0; guard < 0x7fff && (id == 0 || storage_id_in_use(id)); guard++) {
    id = (id >= 0x7ffe) ? 1 : id + 1;
  }
  g_settings.next_id = (id >= 0x7ffe) ? 1 : id + 1;
  settings_save();
  return id;
}

static void prv_begin_segment(void) {
  s.seg_start = now_ms();
  s.frames = 0;
  s.ticks = 0;
  s.baseline = 0;
  s.prev_deficit = 0;
  s.confirmed_max = 0;
  s.worst_write = 0;
  s.last_stat_ms = 0;
}

static void prv_end_segment(void) {
  if (s.state == RecRecording) {
    s.elapsed_before += now_ms() - s.seg_start;
  }
}

static uint32_t prv_estimated_samples(void) {
  return (uint32_t)(((uint64_t)recorder_elapsed_ms() * rate_for_code(s.rate_code)) / 1000);
}

static const char *prv_capture_error(MicCaptureStartResult r) {
  switch (r) {
    case MicCaptureStartErrPermissionDenied:
      return "Allow the microphone for Voice Recorder in the Pebble app.";
    case MicCaptureStartErrBusy: return "The microphone is in use.";
    case MicCaptureStartErrUnavailable: return "This watch has no microphone.";
    case MicCaptureStartErrNoMemory: return "Not enough memory to record.";
    case MicCaptureStartErrNotForeground: return "Open the app to record.";
    default: return "Couldn't start the microphone.";
  }
}

// ---- Health -------------------------------------------------------------------------------------

static void prv_health_tick(void) {
  int32_t now = now_ms();
  int32_t deficit = (now - s.seg_start) / FRAME_MS - (int32_t)s.frames;
  if (++s.ticks == 1) {
    // Frames still in flight at the first second are start-up latency, not loss.
    s.baseline = deficit > 0 ? deficit : 0;
    s.prev_deficit = deficit;
    return;
  }
  // A stalled app task gets its frames late but intact, so only a deficit that is still there a
  // second later counts as lost audio.
  int32_t confirmed = (deficit < s.prev_deficit ? deficit : s.prev_deficit) - s.baseline;
  s.prev_deficit = deficit;
  if (confirmed > s.confirmed_max) {
    int32_t inc = confirmed - s.confirmed_max;
    if (inc > 1500) {
      // The clock jumped (time sync), not the audio.
      s.baseline += inc;
    } else {
      if (inc > 5) {  // more than 100 ms: below that it's clock jitter
        s.lost_frames += inc;
        s.last_loss_ms = now;
      }
      s.confirmed_max = confirmed;  // slow drift is absorbed without counting
    }
  }
  if (s.worst_write && now - s.worst_write_at > 4000) s.worst_write = 0;
}

HealthLevel recorder_health(HealthReason *reason) {
  HealthReason r = HrGood;
  HealthLevel level = HealthGood;
  int32_t now = now_ms();
  if (s.state == RecIdle) {
    r = HrIdle;
  } else if (s.state == RecStarting) {
    r = HrConnecting;
    level = HealthWarn;
  } else if (s.state == RecPaused) {
    r = HrPaused;
  } else if (s.mode == ModeLocal) {
    if (s.last_loss_ms && now - s.last_loss_ms < LOSS_SHOW_MS) {
      r = HrDropping;
      level = HealthBad;
    } else if (recorder_seconds_left() < LOW_SECONDS) {
      r = HrStorageLow;
      level = HealthWarn;
    } else if (s.worst_write >= SLOW_WRITE_MS) {
      r = HrSlowStorage;
      level = HealthWarn;
    }
  } else {
    int32_t since = s.last_stat_ms ? now - s.last_stat_ms : now - s.seg_start;
    int32_t lag = (int32_t)recorder_elapsed_ms() - (int32_t)s.phone_received_ms;
    if (!connection_service_peek_pebble_app_connection()) {
      r = HrPhoneSilent;
      level = HealthBad;
    } else if (since > 6000) {
      r = HrPhoneSilent;
      level = HealthWarn;
    } else if (lag > 10000) {
      r = HrPhoneLag;
      level = HealthBad;
    } else if (lag > 4000) {
      r = HrPhoneLag;
      level = HealthWarn;
    } else if (recorder_seconds_left() < LOW_SECONDS) {
      r = HrStorageLow;
      level = HealthWarn;
    }
  }
  if (reason) *reason = r;
  return level;
}

const char *recorder_health_text(HealthReason reason) {
  switch (reason) {
    case HrGood: return "Good";
    case HrSlowStorage: return "Slow storage";
    case HrStorageLow: return "Storage low";
    case HrDropping: return "Dropping audio";
    case HrPhoneLag: return "Phone lagging";
    case HrPhoneSilent: return "Phone silent";
    case HrConnecting: return "Connecting...";
    case HrPaused: return "Paused";
    default: return "Ready";
  }
}

// ---- Ticks --------------------------------------------------------------------------------------

static void prv_tick(void *ctx) {
  s.tick_timer = NULL;
  if (s.state != RecRecording) return;
  if (s.mode == ModeLocal) prv_health_tick();
  if (s.mode == ModeStream && recorder_seconds_left() == 0) {
    s.notice = "Phone storage is full. Saved.";
    recorder_stop_save();
    return;
  }
  prv_notify(false);
  if (++s.ticks_since_state >= 3) {
    s.ticks_since_state = 0;
    comm_send_rec_state();
  }
  uint32_t delay = 1000 - recorder_elapsed_ms() % 1000;
  if (delay < 200) delay += 1000;
  s.tick_timer = app_timer_register(delay, prv_tick, NULL);
}

static void prv_start_ticks(void) {
  prv_cancel(&s.tick_timer);
  s.ticks_since_state = 0;
  s.tick_timer = app_timer_register(1000, prv_tick, NULL);
}

// ---- Local capture ------------------------------------------------------------------------------

static void prv_full_stop(void *ctx) {
  s.stop_timer = NULL;
  recorder_stop_save();
}

static void prv_write_block(void) {
  if (s.full) return;
  RecMeta *m = storage_find(s.id);
  if (!m) return;
  if (m->blocks >= MAX_BLOCKS_PER_REC || storage_free() < BLOCK_COST * 2) {
    s.full = true;
    s.notice = m->blocks >= MAX_BLOCKS_PER_REC ? "Longest recording reached. Saved."
                                               : "Watch storage is full. Saved.";
    if (!s.stop_timer) s.stop_timer = app_timer_register(0, prv_full_stop, NULL);
    return;
  }
  int32_t t0 = now_ms();
  status_t st = storage_append_block(m, s.enc.block);
  int32_t dt = now_ms() - t0;
  if (dt > s.worst_write) {
    s.worst_write = dt;
    s.worst_write_at = t0;
  }
  if (st != S_SUCCESS) {
    s.full = true;
    s.notice = "Watch storage is full. Saved.";
    if (!s.stop_timer) s.stop_timer = app_timer_register(0, prv_full_stop, NULL);
    return;
  }
  m->samples += ADPCM_BLOCK_SAMPLES;
  // Keep the saved length current so a crash loses little.
  if ((m->blocks & 63) == 0) storage_commit(m);
}

static void prv_mic_data(const int16_t *samples, size_t count, void *ctx) {
  if (s.state != RecRecording || s.mode != ModeLocal) return;
  s.frames++;
  bool decimate = s.rate_code == 0;
  for (size_t i = 0; i < count; i++) {
    int16_t v = samples[i];
    if (decimate && !decimator_push(&s.dec, v, &v)) continue;
    if (adpcm_encoder_push(&s.enc, v)) prv_write_block();
  }
}

static void prv_mic_stopped(MicCaptureStopReason reason, void *ctx) {
  if (s.state != RecRecording || s.mode != ModeLocal) return;
  if (reason == MicCaptureStopReasonPermissionRevoked) {
    s.error = "The microphone permission was turned off.";
    recorder_stop_save();
    return;
  }
  // A notification or the system took the mic: pause, and resume once the app is back.
  prv_end_segment();
  prv_cancel(&s.tick_timer);
  s.state = RecPaused;
  s.auto_paused = true;
  prv_notify(true);
}

static bool prv_capture_begin(void) {
  MicCaptureStartResult r = mic_capture_start((MicCaptureHandlers){
    .data = prv_mic_data,
    .stopped = prv_mic_stopped,
  }, NULL);
  if (r != MicCaptureStartOk) {
    s.error = prv_capture_error(r);
    return false;
  }
  return true;
}

static bool prv_start_local(void) {
  if (storage_free() < BLOCK_COST * 16) {
    s.error = "Watch storage is full. Delete or move recordings first.";
    return false;
  }
  uint16_t id = prv_alloc_id();
  RecMeta *m = storage_create(id, s.rate_code, (uint32_t)time(NULL), RecLocal | RecOpen);
  if (!m) {
    s.error = "Too many recordings. Delete some first.";
    return false;
  }
  storage_commit(m);
  adpcm_encoder_reset(&s.enc);
  decimator_reset(&s.dec);
  s.id = id;
  s.mode = ModeLocal;
  s.full = false;
  s.state = RecRecording;
  if (!prv_capture_begin()) {
    storage_remove(id);
    prv_reset();
    return false;
  }
  storage_set_busy(BusyRecording, true);
  prv_begin_segment();
  prv_start_ticks();
  prv_notify(true);
  return true;
}

static void prv_finish_local(bool save) {
  mic_capture_stop();
  prv_end_segment();
  RecMeta *m = storage_find(s.id);
  if (m && save) {
    if (!s.full) {
      uint16_t real = adpcm_encoder_flush(&s.enc);
      if (real && storage_append_block(m, s.enc.block) == S_SUCCESS) m->samples += real;
    }
    m->lost_ds = (uint16_t)((s.lost_frames * FRAME_MS) / 100);
    m->flags &= ~RecOpen;
    if (m->blocks == 0) {
      storage_remove(s.id);
    } else {
      storage_commit(m);
      comm_send_item(W_SAVED, m);
    }
  } else if (m) {
    storage_remove(s.id);
  }
}

// ---- Phone stream -------------------------------------------------------------------------------

static void prv_stream_started(void *ctx) {
  prv_cancel(&s.start_timer);
  if (s.state != RecStarting) return;
  s.state = RecRecording;
  prv_begin_segment();
  prv_start_ticks();
  prv_notify(true);
}

static void prv_stream_timeout(void *ctx) {
  s.start_timer = NULL;
  if (s.state != RecStarting) return;
  mic_stream_to_phone_stop();
  prv_stream_lost();
}

static void prv_stream_stopped(MicStreamStopReason reason, void *ctx) {
  prv_cancel(&s.start_timer);
  if (s.mode != ModeStream || s.state == RecIdle || s.state == RecPaused) return;
  if (reason == MicStreamStopReasonFocusLost || reason == MicStreamStopReasonInterrupted) {
    prv_end_segment();
    prv_cancel(&s.tick_timer);
    s.state = RecPaused;
    s.auto_paused = true;
    comm_send_simple(W_REC_PAUSE, s.id, 0);
    prv_notify(true);
    return;
  }
  prv_stream_lost();
}

static bool prv_stream_open(void) {
  MicStreamStartResult r = mic_stream_to_phone_start((MicStreamHandlers){
    .started = prv_stream_started,
    .stopped = prv_stream_stopped,
  }, NULL);
  if (r != MicStreamStartOk) return false;
  s.state = RecStarting;
  prv_cancel(&s.start_timer);
  s.start_timer = app_timer_register(STREAM_START_TIMEOUT_MS, prv_stream_timeout, NULL);
  return true;
}

static bool prv_start_stream(void) {
  uint16_t id = prv_alloc_id();
  RecMeta *m = storage_create(id, s.rate_code, (uint32_t)time(NULL), RecPhone | RecOpen);
  if (!m) {
    s.error = "Too many recordings. Delete some first.";
    return false;
  }
  storage_commit(m);
  s.id = id;
  s.mode = ModeStream;
  s.elapsed_before = 0;
  s.phone_received_ms = 0;
  s.phone_free_at_start = g_settings.phone_free;
  comm_send_rec_begin(id, s.rate_code, m->created);
  if (!prv_stream_open()) {
    comm_send_simple(W_REC_DISCARD, id, 0);
    storage_remove(id);
    prv_reset();
    return false;
  }
  prv_notify(true);
  return true;
}

// The phone stopped taking audio: keep what it got and carry on recording on the watch.
static void prv_stream_lost(void) {
  prv_end_segment();
  uint32_t elapsed = s.elapsed_before;
  uint16_t old = s.id;
  RecMeta *m = storage_find(old);
  if (m && elapsed > 0) {
    m->samples = prv_estimated_samples();
    m->flags &= ~RecOpen;
    storage_commit(m);
    comm_send_simple(W_REC_END, old, (int32_t)elapsed);
  } else if (m) {
    comm_send_simple(W_REC_DISCARD, old, 0);
    storage_remove(old);
  }
  uint8_t rate = s.rate_code;
  prv_reset();
  s.rate_code = rate;
  s.notice = "Phone stopped answering. Recording on the watch.";
  if (prv_start_local()) {
    vibes_double_pulse();
  } else {
    prv_notify(true);
  }
}

static void prv_finish_stream(bool save) {
  mic_stream_to_phone_stop();
  prv_end_segment();
  RecMeta *m = storage_find(s.id);
  if (save && m && s.elapsed_before > 0) {
    m->samples = prv_estimated_samples();
    m->flags &= ~RecOpen;
    storage_commit(m);
    comm_send_simple(W_REC_END, s.id, (int32_t)s.elapsed_before);
  } else {
    comm_send_simple(W_REC_DISCARD, s.id, 0);
    if (m) storage_remove(s.id);
  }
}

// ---- Public -------------------------------------------------------------------------------------

static void prv_focus(bool in_focus) {
  if (in_focus && s.auto_paused && s.state == RecPaused) {
    recorder_resume();
  }
}

void recorder_init(void) {
  memset(&s, 0, sizeof(s));
  app_focus_service_subscribe_handlers((AppFocusHandlers){ .did_focus = prv_focus });
}

void recorder_deinit(void) {
  if (s.state != RecIdle) recorder_stop_save();
  app_focus_service_unsubscribe();
}

bool recorder_start(void) {
  if (s.state != RecIdle) return true;
  s.error = NULL;
  s.notice = NULL;
  s.rate_code = g_settings.quality == QualityHigh ? 1 : 0;
  if (g_settings.target == TargetPhone) {
    if (!comm_phone_ready()) {
      s.notice = "Phone not connected. Saving on the watch.";
    } else if (g_settings.phone_free < adpcm_bytes_per_sec(s.rate_code) * 10) {
      s.notice = "Phone storage is full. Saving on the watch.";
    } else if (prv_start_stream()) {
      return true;
    } else if (!s.error) {
      s.notice = "Phone refused the stream. Saving on the watch.";
    } else {
      return false;
    }
  }
  return prv_start_local();
}

void recorder_pause(void) {
  if (s.state != RecRecording && s.state != RecStarting) return;
  s.auto_paused = false;
  if (s.mode == ModeLocal) {
    mic_capture_stop();
  } else {
    prv_cancel(&s.start_timer);
    mic_stream_to_phone_stop();
    comm_send_simple(W_REC_PAUSE, s.id, 0);
  }
  prv_end_segment();
  prv_cancel(&s.tick_timer);
  s.state = RecPaused;
  prv_notify(true);
}

void recorder_resume(void) {
  if (s.state != RecPaused) return;
  s.auto_paused = false;
  s.error = NULL;
  if (s.mode == ModeLocal) {
    s.state = RecRecording;
    if (!prv_capture_begin()) {
      s.state = RecPaused;
      prv_notify(true);
      return;
    }
    prv_begin_segment();
    prv_start_ticks();
    prv_notify(true);
  } else {
    if (!comm_phone_ready()) {
      prv_stream_lost();
      return;
    }
    comm_send_simple(W_REC_RESUME, s.id, 0);
    if (!prv_stream_open()) {
      prv_stream_lost();
      return;
    }
    prv_notify(true);
  }
}

void recorder_stop_save(void) {
  if (s.state == RecIdle) return;
  if (s.mode == ModeLocal) {
    prv_finish_local(true);
  } else {
    prv_finish_stream(true);
  }
  prv_reset();
  prv_notify(true);
}

void recorder_discard(void) {
  if (s.state == RecIdle) return;
  if (s.mode == ModeLocal) {
    prv_finish_local(false);
  } else {
    prv_finish_stream(false);
  }
  prv_reset();
  prv_notify(true);
}

RecState recorder_state(void) { return s.state; }
RecMode recorder_mode(void) { return s.mode; }
uint16_t recorder_current_id(void) { return s.id; }

uint32_t recorder_elapsed_ms(void) {
  uint32_t ms = s.elapsed_before;
  if (s.state == RecRecording) ms += now_ms() - s.seg_start;
  return ms;
}

uint32_t recorder_lost_ms(void) { return s.lost_frames * FRAME_MS; }

bool recorder_uses_phone(void) {
  if (s.state != RecIdle) return s.mode == ModeStream;
  return g_settings.target == TargetPhone && comm_phone_ready();
}

uint32_t recorder_free_bytes(void) {
  if (!recorder_uses_phone()) return storage_free();
  if (s.state == RecIdle) return g_settings.phone_free;
  uint32_t used = recorder_elapsed_ms() / 1000 * adpcm_bytes_per_sec(s.rate_code);
  return s.phone_free_at_start > used ? s.phone_free_at_start - used : 0;
}

uint32_t recorder_seconds_left(void) {
  uint8_t code = s.state != RecIdle ? s.rate_code : (g_settings.quality == QualityHigh ? 1 : 0);
  if (recorder_uses_phone()) return recorder_free_bytes() / adpcm_bytes_per_sec(code);
  uint32_t secs = storage_seconds_left(code);
  if (s.state != RecIdle && s.mode == ModeLocal) {
    // One recording can't pass the block limit.
    RecMeta *m = storage_find(s.id);
    if (m) {
      uint32_t cap = (uint32_t)(MAX_BLOCKS_PER_REC - m->blocks) * ADPCM_BLOCK_SAMPLES /
                     rate_for_code(code);
      if (cap < secs) secs = cap;
    }
  }
  return secs;
}

const char *recorder_error(void) { return s.error; }

const char *recorder_take_notice(void) {
  const char *n = s.notice;
  s.notice = NULL;
  return n;
}

void recorder_set_listener(RecorderListener listener) { s.listener = listener; }

void recorder_on_stream_stat(uint16_t id, uint32_t received_ms) {
  if (id != s.id || s.mode != ModeStream) return;
  s.phone_received_ms = received_ms;
  s.last_stat_ms = now_ms();
}
