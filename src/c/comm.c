#include "comm.h"
#include "settings.h"
#include "recorder.h"
#include "player.h"
#include "ui.h"

#if BIG_SCREEN
  #define MSG_BLOCKS 24
#else
  #define MSG_BLOCKS 8
#endif
#define MSG_SIZE (96 + MSG_BLOCKS * ADPCM_BLOCK_BYTES)

#define OUTQ_LEN 16
#define STR_LEN 40
#define MAX_LISTENERS 4
#define MAX_JOBS 8
#define MAX_PULLS 6

typedef struct {
  uint8_t cmd;
  uint8_t nv;          // how many of v[] to send
  uint16_t id;
  int32_t v[4];
  char str[STR_LEN];
} OutMsg;

typedef enum { InflightNone = 0, InflightQueued, InflightAudio, InflightList } Inflight;

static OutMsg s_queue[OUTQ_LEN];
static int s_qhead, s_qcount;
static Inflight s_inflight;
static uint8_t s_inflight_cmd;
static int s_retries;
static AppTimer *s_retry_timer;

static bool s_phone_ready;
static uint16_t s_outbox_blocks;

// Watch list sync: -2 idle, -1 begin pending, 0..n-1 next item, n end pending
static int s_list_pos = -2;

// Audio transfer to the phone
static struct {
  bool active;
  uint16_t id;
  uint16_t next;
  uint16_t total;
  uint8_t *buf;
  uint16_t chunk;      // blocks in the chunk in flight
} s_xfer;
static uint16_t s_pulls[MAX_PULLS];
static int s_pull_count;

static JobStatus s_jobs[MAX_JOBS];
static CommListener s_listeners[MAX_LISTENERS];
static const TranscriptSink *s_sink;

// Which entries the phone listed during a phone list sync
static uint64_t s_seen;
static bool s_phone_sync;

static void prv_pump(void);
static void prv_send_hello(void);

// ---- Listeners ----------------------------------------------------------------------------------

void comm_add_listener(CommListener l) {
  for (int i = 0; i < MAX_LISTENERS; i++) {
    if (!s_listeners[i]) { s_listeners[i] = l; return; }
  }
}

void comm_remove_listener(CommListener l) {
  for (int i = 0; i < MAX_LISTENERS; i++) {
    if (s_listeners[i] == l) s_listeners[i] = NULL;
  }
}

static void prv_emit(CommEvent ev, uint16_t id, const char *text) {
  for (int i = 0; i < MAX_LISTENERS; i++) {
    if (s_listeners[i]) s_listeners[i](ev, id, text);
  }
}

void comm_set_transcript_sink(const TranscriptSink *sink) { s_sink = sink; }

// ---- Outbox -------------------------------------------------------------------------------------

static OutMsg *prv_enqueue(uint8_t cmd, uint16_t id) {
  if (cmd == W_REC_STATE) {
    // Only the latest state matters.
    for (int i = 0; i < s_qcount; i++) {
      int idx = (s_qhead + i) % OUTQ_LEN;
      bool sending = (i == 0 && s_inflight == InflightQueued);
      if (s_queue[idx].cmd == W_REC_STATE && !sending) return &s_queue[idx];
    }
  }
  if (s_qcount == OUTQ_LEN) {
    if (s_inflight == InflightQueued) return NULL;  // keep the one in flight intact
    s_qhead = (s_qhead + 1) % OUTQ_LEN;
    s_qcount--;
  }
  OutMsg *m = &s_queue[(s_qhead + s_qcount) % OUTQ_LEN];
  s_qcount++;
  memset(m, 0, sizeof(*m));
  m->cmd = cmd;
  m->id = id;
  return m;
}

static void prv_retry_cb(void *ctx) {
  s_retry_timer = NULL;
  prv_pump();
}

static void prv_write_meta_fields(DictionaryIterator *it, const RecMeta *m) {
  dict_write_int32(it, MESSAGE_KEY_A, (int32_t)m->created);
  dict_write_int32(it, MESSAGE_KEY_B, (int32_t)m->samples);
  dict_write_int32(it, MESSAGE_KEY_C,
                   (int32_t)(m->flags | (m->rate_code << 8) | ((uint32_t)m->blocks << 16)));
  dict_write_int32(it, MESSAGE_KEY_D, m->lost_ds);
  dict_write_cstring(it, MESSAGE_KEY_Str, m->name);
}

static bool prv_send_queued(void) {
  OutMsg *m = &s_queue[s_qhead];
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_int32(it, MESSAGE_KEY_Cmd, m->cmd);
  dict_write_int32(it, MESSAGE_KEY_Id, m->id);
  static const uint32_t *keys[4] = { &MESSAGE_KEY_A, &MESSAGE_KEY_B, &MESSAGE_KEY_C, &MESSAGE_KEY_D };
  for (int i = 0; i < m->nv; i++) dict_write_int32(it, *keys[i], m->v[i]);
  if (m->str[0]) dict_write_cstring(it, MESSAGE_KEY_Str, m->str);
  if (app_message_outbox_send() != APP_MSG_OK) return false;
  s_inflight = InflightQueued;
  s_inflight_cmd = m->cmd;
  return true;
}

static bool prv_send_list_step(void) {
  DictionaryIterator *it;
  int n = storage_count();
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  if (s_list_pos == -1) {
    dict_write_int32(it, MESSAGE_KEY_Cmd, W_LIST_BEGIN);
    dict_write_int32(it, MESSAGE_KEY_A, n);
  } else if (s_list_pos >= n) {
    dict_write_int32(it, MESSAGE_KEY_Cmd, W_LIST_END);
    dict_write_int32(it, MESSAGE_KEY_A, (int32_t)storage_used());
    dict_write_int32(it, MESSAGE_KEY_B, (int32_t)storage_capacity());
    dict_write_int32(it, MESSAGE_KEY_C, (int32_t)storage_free());
  } else {
    const RecMeta *m = storage_get(s_list_pos);
    dict_write_int32(it, MESSAGE_KEY_Cmd, W_LIST_ITEM);
    dict_write_int32(it, MESSAGE_KEY_Id, m->id);
    prv_write_meta_fields(it, m);
  }
  if (app_message_outbox_send() != APP_MSG_OK) return false;
  s_inflight = InflightList;
  return true;
}

static void prv_xfer_finish(void) {
  if (s_xfer.buf) free(s_xfer.buf);
  s_xfer.buf = NULL;
  s_xfer.active = false;
  storage_set_busy(BusyTransfer, false);
}

static void prv_xfer_start_next(void) {
  while (!s_xfer.active && s_pull_count > 0) {
    uint16_t id = s_pulls[0];
    memmove(&s_pulls[0], &s_pulls[1], (s_pull_count - 1) * sizeof(uint16_t));
    s_pull_count--;
    RecMeta *m = storage_find(id);
    if (!m || !(m->flags & RecLocal) || (m->flags & RecOpen) || m->blocks == 0) {
      comm_send_simple(W_PULL_ERR, id, 1);
      continue;
    }
    s_xfer.buf = malloc(s_outbox_blocks * ADPCM_BLOCK_BYTES);
    if (!s_xfer.buf) {
      comm_send_simple(W_PULL_ERR, id, 2);
      continue;
    }
    s_xfer.active = true;
    s_xfer.id = id;
    s_xfer.next = 0;
    s_xfer.total = m->blocks;
    // Hold off background deletes so reads stay quick.
    storage_set_busy(BusyTransfer, true);
  }
}

static bool prv_send_audio_chunk(void) {
  RecMeta *m = storage_find(s_xfer.id);
  if (!m || !(m->flags & RecLocal)) {
    prv_xfer_finish();
    comm_send_simple(W_PULL_ERR, s_xfer.id, 1);
    return false;
  }
  if (s_xfer.next >= s_xfer.total) {
    OutMsg *e = prv_enqueue(W_AUDIO_END, m->id);
    if (e) {
      e->nv = 4;
      e->v[0] = m->blocks;
      e->v[1] = (int32_t)m->samples;
      e->v[2] = m->rate_code;
      e->v[3] = (int32_t)m->created;
    }
    prv_xfer_finish();
    prv_xfer_start_next();
    return false;
  }
  uint16_t count = s_xfer.total - s_xfer.next;
  if (count > s_outbox_blocks) count = s_outbox_blocks;
  for (uint16_t i = 0; i < count; i++) {
    if (!storage_read_block(m->id, s_xfer.next + i, s_xfer.buf + i * ADPCM_BLOCK_BYTES)) {
      memset(s_xfer.buf + i * ADPCM_BLOCK_BYTES, 0, ADPCM_BLOCK_BYTES);
    }
  }
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return false;
  dict_write_int32(it, MESSAGE_KEY_Cmd, W_AUDIO);
  dict_write_int32(it, MESSAGE_KEY_Id, m->id);
  dict_write_int32(it, MESSAGE_KEY_A, s_xfer.next);
  dict_write_data(it, MESSAGE_KEY_Data, s_xfer.buf, count * ADPCM_BLOCK_BYTES);
  if (app_message_outbox_send() != APP_MSG_OK) return false;
  s_xfer.chunk = count;
  s_inflight = InflightAudio;
  return true;
}

static void prv_pump(void) {
  if (s_inflight != InflightNone || s_retry_timer) return;
  if (!connection_service_peek_pebble_app_connection()) return;
  for (int guard = 0; guard < 4; guard++) {
    if (s_qcount > 0) {
      if (!prv_send_queued()) break;
      return;
    }
    if (s_list_pos > -2 && s_phone_ready) {
      if (!prv_send_list_step()) break;
      return;
    }
    if (s_xfer.active && s_phone_ready) {
      if (prv_send_audio_chunk()) return;
      continue;  // the transfer ended; something else may be waiting
    }
    return;
  }
  // The outbox was busy; try again shortly.
  s_retry_timer = app_timer_register(150, prv_retry_cb, NULL);
}

static void prv_outbox_sent(DictionaryIterator *it, void *ctx) {
  Inflight was = s_inflight;
  s_inflight = InflightNone;
  s_retries = 0;
  switch (was) {
    case InflightQueued:
      if (s_inflight_cmd == W_SETTINGS) {
        g_settings.dirty = 0;
        settings_save();
      }
      s_qhead = (s_qhead + 1) % OUTQ_LEN;
      s_qcount--;
      break;
    case InflightList: {
      int n = storage_count();
      s_list_pos = (s_list_pos >= n) ? -2 : s_list_pos + 1;
      break;
    }
    case InflightAudio:
      s_xfer.next += s_xfer.chunk;
      break;
    default:
      break;
  }
  prv_pump();
}

static void prv_outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *ctx) {
  Inflight was = s_inflight;
  s_inflight = InflightNone;
  if (++s_retries > 5) {
    s_retries = 0;
    if (was == InflightQueued) {
      s_qhead = (s_qhead + 1) % OUTQ_LEN;
      s_qcount--;
    } else if (was == InflightList) {
      s_list_pos = -2;
    } else if (was == InflightAudio) {
      prv_xfer_finish();
      prv_xfer_start_next();
    }
  }
  if (!s_retry_timer) {
    s_retry_timer = app_timer_register(300 + s_retries * 300, prv_retry_cb, NULL);
  }
}

// ---- Public senders -----------------------------------------------------------------------------

void comm_send_simple(uint8_t cmd, uint16_t id, int32_t a) {
  OutMsg *m = prv_enqueue(cmd, id);
  if (!m) return;
  m->nv = 1;
  m->v[0] = a;
  prv_pump();
}

void comm_send_settings(void) {
  OutMsg *m = prv_enqueue(W_SETTINGS, 0);
  if (!m) return;
  m->nv = 3;
  m->v[0] = g_settings.quality;
  m->v[1] = g_settings.target;
  m->v[2] = g_settings.ql_autostart;
  strncpy(m->str, g_settings.lang, STR_LEN - 1);
  prv_pump();
}

void comm_send_rec_state(void) {
  if (!s_phone_ready) return;
  OutMsg *m = prv_enqueue(W_REC_STATE, recorder_current_id());
  if (!m) return;
  HealthReason reason;
  HealthLevel level = recorder_health(&reason);
  m->nv = 4;
  m->v[0] = recorder_state();
  m->v[1] = (int32_t)recorder_elapsed_ms();
  m->v[2] = recorder_mode();
  m->v[3] = level | (reason << 8);
  prv_pump();
}

void comm_send_item(uint8_t cmd, const RecMeta *meta) {
  OutMsg *m = prv_enqueue(cmd, meta->id);
  if (!m) return;
  m->nv = 4;
  m->v[0] = (int32_t)meta->created;
  m->v[1] = (int32_t)meta->samples;
  m->v[2] = (int32_t)(meta->flags | (meta->rate_code << 8) | ((uint32_t)meta->blocks << 16));
  m->v[3] = meta->lost_ds;
  strncpy(m->str, meta->name, STR_LEN - 1);
  prv_pump();
}

void comm_send_rec_begin(uint16_t id, uint8_t rate_code, uint32_t created) {
  OutMsg *m = prv_enqueue(W_REC_BEGIN, id);
  if (!m) return;
  m->nv = 2;
  m->v[0] = rate_code;
  m->v[1] = (int32_t)created;
  prv_pump();
}

void comm_send_action(uint16_t id, uint8_t action, const char *lang) {
  OutMsg *m = prv_enqueue(W_ACTION, id);
  if (!m) return;
  m->nv = 1;
  m->v[0] = action;
  if (lang) strncpy(m->str, lang, STR_LEN - 1);
  prv_pump();
}

void comm_request_transcript(uint16_t id) {
  comm_send_simple(W_TR_REQ, id, 0);
}

void comm_request_play(uint16_t id, uint16_t block, uint16_t count) {
  OutMsg *m = prv_enqueue(W_PLAY_REQ, id);
  if (!m) return;
  m->nv = 2;
  m->v[0] = block;
  m->v[1] = count;
  prv_pump();
}

void comm_send_list(void) {
  s_list_pos = -1;
  prv_pump();
}

static void prv_send_hello(void) {
  OutMsg *m = prv_enqueue(W_HELLO, 0);
  if (!m) return;
  m->nv = 4;
  m->v[0] = (int32_t)storage_capacity();
  m->v[1] = (int32_t)storage_used();
  m->v[2] = (player_available() ? FEATURE_SPEAKER : 0) | PBL_IF_ROUND_ELSE(FEATURE_ROUND, 0);
  m->v[3] = MSG_BLOCKS;
  strncpy(m->str, APP_VERSION, STR_LEN - 1);
  prv_pump();
}

bool comm_phone_ready(void) {
  return s_phone_ready && connection_service_peek_pebble_app_connection();
}

uint16_t comm_inbox_blocks(void) { return MSG_BLOCKS; }

const JobStatus *comm_job_for(uint16_t id) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (s_jobs[i].id == id && s_jobs[i].status <= JobRunning) return &s_jobs[i];
  }
  return NULL;
}

// ---- Inbox --------------------------------------------------------------------------------------

static int32_t prv_int(DictionaryIterator *it, uint32_t key) {
  Tuple *t = dict_find(it, key);
  if (!t) return 0;
  switch (t->length) {
    case 1: return t->type == TUPLE_INT ? t->value->int8 : t->value->uint8;
    case 2: return t->type == TUPLE_INT ? t->value->int16 : t->value->uint16;
    default: return t->value->int32;
  }
}

static const char *prv_str(DictionaryIterator *it) {
  Tuple *t = dict_find(it, MESSAGE_KEY_Str);
  return (t && t->type == TUPLE_CSTRING) ? t->value->cstring : "";
}

static void prv_set_job(uint16_t id, uint8_t kind, uint8_t status, uint8_t pct) {
  int slot = -1;
  for (int i = 0; i < MAX_JOBS; i++) {
    if (s_jobs[i].id == id && s_jobs[i].kind == kind) { slot = i; break; }
  }
  if (slot < 0) {
    for (int i = 0; i < MAX_JOBS; i++) {
      if (s_jobs[i].id == 0 || s_jobs[i].status >= JobDone) { slot = i; break; }
    }
  }
  if (slot < 0) slot = 0;
  s_jobs[slot] = (JobStatus){ .id = id, .kind = kind, .status = status, .pct = pct };
}

static void prv_apply_phone_item(uint16_t id, uint32_t created, uint32_t samples, uint32_t c,
                                 uint16_t lost, const char *name) {
  uint8_t pflags = c & (RecPhone | RecTranscript | RecUploaded);
  uint8_t rate = (c >> 8) & 0xff;
  RecMeta *m = storage_find(id);
  if (!m) {
    if (!pflags) return;
    m = storage_create(id, rate, created, pflags);
    if (!m) return;
    m->samples = samples;
    m->lost_ds = lost;
    strncpy(m->name, name, REC_NAME_LEN - 1);
    storage_commit(m);
  } else {
    uint8_t flags = (m->flags & ~(RecPhone | RecTranscript | RecUploaded)) | pflags;
    bool changed = flags != m->flags || strncmp(m->name, name, REC_NAME_LEN - 1) != 0;
    m->flags = flags;
    strncpy(m->name, name, REC_NAME_LEN - 1);
    m->name[REC_NAME_LEN - 1] = '\0';
    if (!(m->flags & RecLocal) && samples && samples != m->samples) {
      m->samples = samples;
      changed = true;
    }
    if (changed) storage_commit(m);
  }
  int idx = storage_index_of(id);
  if (idx >= 0) s_seen |= ((uint64_t)1 << idx);
}

static void prv_phone_gone(uint16_t id) {
  RecMeta *m = storage_find(id);
  if (!m || (m->flags & RecOpen)) return;
  m->flags &= ~(RecPhone | RecTranscript | RecUploaded);
  if (!(m->flags & RecLocal)) {
    storage_remove(id);
  } else {
    storage_commit(m);
  }
}

static void prv_end_phone_sync(void) {
  // Anything the phone no longer lists is gone from it. Walk back so removals don't shift us.
  uint16_t gone[MAX_RECS];
  int n = 0;
  for (int i = storage_count() - 1; i >= 0; i--) {
    RecMeta *m = storage_get(i);
    if (!(s_seen & ((uint64_t)1 << i)) && (m->flags & (RecPhone | RecTranscript | RecUploaded))) {
      gone[n++] = m->id;
    }
  }
  for (int i = 0; i < n; i++) prv_phone_gone(gone[i]);
  s_phone_sync = false;
}

static void prv_inbox(DictionaryIterator *it, void *ctx) {
  int32_t cmd = prv_int(it, MESSAGE_KEY_Cmd);
  uint16_t id = (uint16_t)prv_int(it, MESSAGE_KEY_Id);
  int32_t a = prv_int(it, MESSAGE_KEY_A);
  int32_t b = prv_int(it, MESSAGE_KEY_B);
  int32_t c = prv_int(it, MESSAGE_KEY_C);
  int32_t d = prv_int(it, MESSAGE_KEY_D);
  const char *str = prv_str(it);

  switch (cmd) {
    case P_HELLO:
      s_phone_ready = true;
      g_settings.caps = (uint8_t)a;
      g_settings.phone_free = (uint32_t)b;
      g_settings.phone_used = (uint32_t)c;
      if (d >= g_settings.next_id && d < 0x7fff) g_settings.next_id = d + 1;
      settings_save();
      // A hello sent before the phone's JS was running is acknowledged but lost, so answer
      // every phone hello with ours (the phone only replies to it if it hasn't just said hello).
      prv_send_hello();
      if (g_settings.dirty) comm_send_settings();
      comm_send_list();
      comm_send_rec_state();
      prv_emit(CommEvPhone, 0, NULL);
      break;

    case P_SETTINGS:
      if (g_settings.dirty) {
        comm_send_settings();
        break;
      }
      if (a >= 0 && a < QualityCount) g_settings.quality = a;
      if (b >= 0 && b < TargetCount) g_settings.target = b;
      g_settings.ql_autostart = c ? 1 : 0;
      if (str[0]) strncpy(g_settings.lang, str, LANG_CODE_LEN - 1);
      settings_save();
      prv_emit(CommEvPhone, 0, NULL);
      break;

    case P_CONTROL:
      switch (a) {
        case CtlStart:
          if (recorder_state() == RecIdle) {
            ui_recorder_bring_to_front();
            if (!recorder_start()) {
              comm_send_rec_state();
            }
          } else if (recorder_state() == RecPaused) {
            recorder_resume();
          }
          break;
        case CtlPause: recorder_pause(); break;
        case CtlResume: recorder_resume(); break;
        case CtlStop: recorder_stop_save(); break;
        case CtlDiscard: recorder_discard(); break;
      }
      comm_send_rec_state();
      break;

    case P_LIST_REQ:
      comm_send_list();
      break;

    case P_RENAME: {
      RecMeta *m = storage_find(id);
      if (m) {
        strncpy(m->name, str, REC_NAME_LEN - 1);
        m->name[REC_NAME_LEN - 1] = '\0';
        storage_commit(m);
        prv_emit(CommEvList, id, NULL);
      }
      break;
    }

    case P_DELETE:
      if (id != recorder_current_id() || recorder_state() == RecIdle) {
        storage_delete_local(id);
        comm_send_simple(W_DELETED, id, 0);
        prv_emit(CommEvList, id, NULL);
      }
      break;

    case P_PULL:
      if (s_xfer.active && s_xfer.id == id) break;
      for (int i = 0; i < s_pull_count; i++) {
        if (s_pulls[i] == id) id = 0;
      }
      if (id && s_pull_count < MAX_PULLS) s_pulls[s_pull_count++] = id;
      prv_xfer_start_next();
      prv_pump();
      break;

    case P_PHONE_BEGIN:
      s_seen = 0;
      s_phone_sync = true;
      break;

    case P_PHONE_ITEM:
      prv_apply_phone_item(id, (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint16_t)d, str);
      if (!s_phone_sync) prv_emit(CommEvList, id, NULL);
      break;

    case P_PHONE_END:
      if (s_phone_sync) prv_end_phone_sync();
      prv_emit(CommEvList, 0, NULL);
      break;

    case P_PHONE_GONE:
      prv_phone_gone(id);
      prv_emit(CommEvList, id, NULL);
      break;

    case P_TR_BEGIN:
      if (s_sink && s_sink->begin) s_sink->begin(id, (uint16_t)a, b != 0);
      break;
    case P_TR_SEG:
      if (s_sink && s_sink->segment) s_sink->segment(id, (uint16_t)a, (uint32_t)b, str);
      break;
    case P_TR_END:
      if (s_sink && s_sink->end) s_sink->end(id);
      break;
    case P_TR_NONE:
      if (s_sink && s_sink->fail) s_sink->fail(id, str);
      break;

    case P_PLAY_DATA: {
      Tuple *t = dict_find(it, MESSAGE_KEY_Data);
      if (t && t->type == TUPLE_BYTE_ARRAY) {
        player_on_remote_data(id, (uint16_t)a, t->value->data, t->length);
      }
      break;
    }
    case P_PLAY_ERR:
      player_on_remote_error(id, str);
      break;

    case P_JOB:
      prv_set_job(id, (uint8_t)a, (uint8_t)b, (uint8_t)c);
      if (b == JobDone) {
        if (a == JobTranscribe) {
          RecMeta *m = storage_find(id);
          if (m && !(m->flags & RecTranscript)) {
            m->flags |= RecTranscript;
            storage_commit(m);
          }
        }
        if (recorder_state() == RecIdle) vibes_short_pulse();
      }
      prv_emit(b == JobFailed ? CommEvJobFailed : CommEvJob, id, str);
      break;

    case P_STREAM_STAT:
      recorder_on_stream_stat(id, (uint32_t)a);
      break;

    case P_STORAGE:
      g_settings.phone_free = (uint32_t)b;
      g_settings.phone_used = (uint32_t)c;
      prv_emit(CommEvPhone, 0, NULL);
      break;

    case P_REC_SAVED: {
      RecMeta *m = storage_find(id);
      if (m) {
        m->samples = (uint32_t)b;
        m->lost_ds = (uint16_t)d;
        m->flags = (m->flags & ~RecOpen) | ((uint32_t)c & (RecPhone | RecTranscript | RecUploaded));
        storage_commit(m);
        prv_emit(CommEvList, id, NULL);
      }
      break;
    }
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped %d", reason);
}

static void prv_connection(bool connected) {
  s_phone_ready = false;
  if (connected) {
    prv_send_hello();
  } else {
    s_list_pos = -2;
    if (s_xfer.active) prv_xfer_finish();
    s_pull_count = 0;
  }
  prv_emit(CommEvPhone, 0, NULL);
}

// ---- Lifecycle ----------------------------------------------------------------------------------

void comm_init(void) {
  s_outbox_blocks = MSG_BLOCKS;
  app_message_register_inbox_received(prv_inbox);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(MSG_SIZE, MSG_SIZE);
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = prv_connection,
  });
  if (connection_service_peek_pebble_app_connection()) prv_send_hello();
}

void comm_deinit(void) {
  connection_service_unsubscribe();
  if (s_retry_timer) app_timer_cancel(s_retry_timer);
  s_retry_timer = NULL;
  if (s_xfer.buf) free(s_xfer.buf);
  s_xfer.buf = NULL;
}
