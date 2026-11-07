#include "player.h"
#include "storage.h"
#include "comm.h"
#include "adpcm.h"
#include "settings.h"
#include "i18n.h"

#if defined(PBL_SPEAKER)

#define FILL_INTERVAL_MS 40
#if BIG_SCREEN
  #define CACHE_BLOCKS 32
#else
  #define CACHE_BLOCKS 12
#endif
#define REQUEST_TIMEOUT_MS 5000

static struct {
  PlayerState state;
  uint16_t id;
  bool remote;
  uint8_t rate_code;
  uint16_t total_blocks;
  uint32_t total_samples;

  // Decode position
  uint16_t next_block;
  int16_t pcm[ADPCM_BLOCK_SAMPLES];
  uint16_t pcm_len;
  uint16_t pcm_off;
  bool stream_open;
  bool all_written;

  // Clock: where playback started and when
  uint32_t base_ms;
  int32_t started_at;
  uint32_t written_samples;  // since base

  // Phone copy: blocks [cache_base, cache_base + cache_count)
  uint8_t *cache;
  uint16_t cache_base;
  uint16_t cache_count;
  bool requesting;
  uint16_t req_end;
  int32_t requested_at;

  AppTimer *timer;
  PlayerListener listener;
  const char *error;
} s;

static void prv_notify(void) {
  if (s.listener) s.listener();
}

static void prv_cancel_timer(void) {
  if (s.timer) {
    app_timer_cancel(s.timer);
    s.timer = NULL;
  }
}

static uint32_t prv_samples_to_ms(uint32_t samples) {
  return (uint32_t)(((uint64_t)samples * 1000) / rate_for_code(s.rate_code));
}

static void prv_request_more(void) {
  if (!s.remote || s.requesting) return;
  uint16_t want_from = s.cache_base + s.cache_count;
  if (want_from >= s.total_blocks) return;
  uint16_t room = CACHE_BLOCKS - s.cache_count;
  if (room < comm_inbox_blocks() && s.cache_count > 0) return;
  uint16_t count = room;
  if (count > s.total_blocks - want_from) count = s.total_blocks - want_from;
  if (count == 0) return;
  s.requesting = true;
  s.req_end = want_from + count;
  s.requested_at = now_ms();
  comm_request_play(s.id, want_from, count);
}

// Loads block `index` into pcm[]. Returns false when it isn't available yet.
static bool prv_load_block(uint16_t index) {
  uint8_t raw[ADPCM_BLOCK_BYTES];
  if (!s.remote) {
    if (!storage_read_block(s.id, index, raw)) memset(raw, 0, sizeof(raw));
  } else {
    if (index < s.cache_base || index >= s.cache_base + s.cache_count) {
      if (index < s.cache_base || index > s.cache_base + s.cache_count) {
        // Seeked away from the cached range.
        s.cache_base = index;
        s.cache_count = 0;
        s.requesting = false;
      }
      prv_request_more();
      return false;
    }
    memcpy(raw, s.cache + (index - s.cache_base) * ADPCM_BLOCK_BYTES, ADPCM_BLOCK_BYTES);
    // Drop what has been played once half the cache is behind us.
    uint16_t behind = index - s.cache_base;
    if (behind >= CACHE_BLOCKS / 2) {
      memmove(s.cache, s.cache + behind * ADPCM_BLOCK_BYTES,
              (s.cache_count - behind) * ADPCM_BLOCK_BYTES);
      s.cache_base += behind;
      s.cache_count -= behind;
    }
    prv_request_more();
  }
  adpcm_decode_block(raw, s.pcm);
  uint32_t first = (uint32_t)index * ADPCM_BLOCK_SAMPLES;
  uint32_t len = ADPCM_BLOCK_SAMPLES;
  if (first + len > s.total_samples) len = s.total_samples > first ? s.total_samples - first : 0;
  s.pcm_len = (uint16_t)len;
  return true;
}

static void prv_fill(void *ctx);

static void prv_schedule(uint32_t ms) {
  prv_cancel_timer();
  s.timer = app_timer_register(ms, prv_fill, NULL);
}

static bool prv_open_stream(void) {
  if (s.stream_open) return true;
  SpeakerPcmFormat fmt = s.rate_code ? SpeakerPcmFormat_16kHz_16bit : SpeakerPcmFormat_8kHz_16bit;
  if (!speaker_stream_open(fmt, g_settings.volume)) {
    s.error = tr(S_P_BUSY);
    return false;
  }
  s.stream_open = true;
  return true;
}

static void prv_fill(void *ctx) {
  s.timer = NULL;
  if (s.state != PlayerPlaying && s.state != PlayerBuffering) return;
  if (s.remote && s.requesting && now_ms() - s.requested_at > REQUEST_TIMEOUT_MS) {
    s.requesting = false;
    prv_request_more();
  }
  for (int guard = 0; guard < 8 && !s.all_written; guard++) {
    if (s.pcm_off >= s.pcm_len) {
      if (s.next_block >= s.total_blocks) {
        s.all_written = true;
        break;
      }
      if (!prv_load_block(s.next_block)) {
        if (s.state != PlayerBuffering) {
          s.state = PlayerBuffering;
          prv_notify();
        }
        prv_schedule(FILL_INTERVAL_MS * 2);
        return;
      }
      s.next_block++;
      s.pcm_off = 0;
    }
    if (s.state == PlayerBuffering) {
      s.state = PlayerPlaying;
      s.base_ms = player_position_ms();
      s.started_at = now_ms();
      s.written_samples = 0;
      prv_notify();
    }
    if (!prv_open_stream()) {
      s.state = PlayerPaused;
      prv_notify();
      return;
    }
    uint32_t bytes = (uint32_t)(s.pcm_len - s.pcm_off) * sizeof(int16_t);
    uint32_t written = speaker_stream_write(&s.pcm[s.pcm_off], bytes);
    s.pcm_off += written / sizeof(int16_t);
    s.written_samples += written / sizeof(int16_t);
    if (written < bytes) break;  // the speaker's buffer is full
  }
  if (s.all_written && s.stream_open) {
    speaker_stream_close();  // plays out what is buffered, then calls the finish callback
    return;
  }
  prv_schedule(FILL_INTERVAL_MS);
}

static void prv_finished(SpeakerFinishReason reason, void *ctx) {
  // Only follows our own speaker_stop(), which already cleared stream_open. It can arrive after a
  // seek or a quick pause/play has opened the next stream, so it must not touch that one.
  if (reason == SpeakerFinishReasonStopped) return;
  s.stream_open = false;
  if (s.state != PlayerPlaying && s.state != PlayerBuffering) return;
  if (reason == SpeakerFinishReasonDone && s.all_written) {
    prv_cancel_timer();
    s.state = PlayerEnded;
    s.base_ms = player_duration_ms();
    prv_notify();
  } else if (reason == SpeakerFinishReasonPreempted || reason == SpeakerFinishReasonError) {
    s.base_ms = player_position_ms();
    prv_cancel_timer();
    s.state = PlayerPaused;
    prv_notify();
  }
  // Ran dry while waiting for the phone: prv_fill reopens the stream when data comes.
}

static void prv_position_at(uint32_t ms) {
  uint32_t dur = player_duration_ms();
  if (ms > dur) ms = dur;
  uint32_t sample = (uint32_t)(((uint64_t)ms * rate_for_code(s.rate_code)) / 1000);
  s.next_block = sample / ADPCM_BLOCK_SAMPLES;
  s.pcm_len = 0;
  s.pcm_off = 0;
  s.all_written = false;
  s.base_ms = ms;
  s.written_samples = 0;
  if (s.next_block < s.total_blocks && prv_load_block(s.next_block)) {
    s.next_block++;
    s.pcm_off = sample % ADPCM_BLOCK_SAMPLES;
  }
}

bool player_available(void) { return true; }

bool player_muted(void) { return speaker_is_muted(); }

void player_set_volume(uint8_t volume) {
  if (s.stream_open) speaker_set_volume(volume);
}

bool player_open(uint16_t id) {
  player_close();
  s.error = NULL;
  RecMeta *m = storage_find(id);
  if (!m || !(m->flags & (RecLocal | RecPhone))) {
    s.error = tr(S_P_NO_AUDIO);
    return false;
  }
  s.id = id;
  s.remote = !(m->flags & RecLocal);
  s.rate_code = m->rate_code;
  s.total_samples = m->samples;
  s.total_blocks = s.remote ? (m->samples + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES : m->blocks;
  if (s.remote) {
    if (!comm_phone_ready()) {
      s.error = tr(S_P_CONNECT);
      return false;
    }
    s.cache = malloc(CACHE_BLOCKS * ADPCM_BLOCK_BYTES);
    if (!s.cache) {
      s.error = tr(S_P_MEMORY);
      return false;
    }
    s.cache_base = 0;
    s.cache_count = 0;
    s.requesting = false;
  }
  speaker_set_finish_callback(prv_finished, NULL);
  s.state = PlayerStopped;
  prv_position_at(0);
  storage_set_busy(BusyPlaying, true);
  return true;
}

void player_close(void) {
  prv_cancel_timer();
  if (s.stream_open) speaker_stop();
  s.stream_open = false;
  speaker_set_finish_callback(NULL, NULL);
  if (s.cache) free(s.cache);
  s.cache = NULL;
  storage_set_busy(BusyPlaying, false);
  s.state = PlayerStopped;
  s.id = 0;
}

void player_play(void) {
  if (!s.id) return;
  if (s.state == PlayerPlaying || s.state == PlayerBuffering) return;
  if (s.state == PlayerEnded) prv_position_at(0);
  else prv_position_at(s.base_ms);
  s.started_at = now_ms();
  s.written_samples = 0;
  s.state = PlayerPlaying;
  prv_notify();
  prv_fill(NULL);
}

void player_pause(void) {
  if (s.state != PlayerPlaying && s.state != PlayerBuffering) return;
  s.base_ms = player_position_ms();
  prv_cancel_timer();
  if (s.stream_open) speaker_stop();
  s.stream_open = false;
  s.state = PlayerPaused;
  prv_notify();
}

void player_toggle(void) {
  if (s.state == PlayerPlaying || s.state == PlayerBuffering) player_pause();
  else player_play();
}

void player_seek_ms(uint32_t ms) {
  bool playing = s.state == PlayerPlaying || s.state == PlayerBuffering;
  if (playing) {
    prv_cancel_timer();
    if (s.stream_open) speaker_stop();
    s.stream_open = false;
  }
  prv_position_at(ms);
  if (playing) {
    s.started_at = now_ms();
    s.state = PlayerPlaying;
    prv_fill(NULL);
  } else if (s.state == PlayerEnded) {
    s.state = PlayerPaused;
  }
  prv_notify();
}

void player_skip_ms(int32_t delta) {
  int32_t pos = (int32_t)player_position_ms() + delta;
  player_seek_ms(pos < 0 ? 0 : (uint32_t)pos);
}

uint32_t player_position_ms(void) {
  if (s.state == PlayerPlaying) {
    uint32_t wall = (uint32_t)(now_ms() - s.started_at);
    uint32_t written = prv_samples_to_ms(s.written_samples);
    uint32_t pos = s.base_ms + (wall < written ? wall : written);
    uint32_t dur = player_duration_ms();
    return pos > dur ? dur : pos;
  }
  return s.base_ms;
}

uint32_t player_duration_ms(void) { return prv_samples_to_ms(s.total_samples); }
PlayerState player_state(void) { return s.state; }
const char *player_error(void) { return s.error; }
void player_set_listener(PlayerListener listener) { s.listener = listener; }

void player_on_remote_data(uint16_t id, uint16_t block, const uint8_t *data, uint16_t len) {
  if (!s.cache || id != s.id) return;
  if (block != s.cache_base + s.cache_count) {
    // Left over from before a seek. Ask again unless a request for the right range is pending.
    if (!s.requesting) prv_request_more();
    return;
  }
  uint16_t count = len / ADPCM_BLOCK_BYTES;
  if (count > CACHE_BLOCKS - s.cache_count) count = CACHE_BLOCKS - s.cache_count;
  memcpy(s.cache + s.cache_count * ADPCM_BLOCK_BYTES, data, count * ADPCM_BLOCK_BYTES);
  s.cache_count += count;
  if (s.cache_base + s.cache_count >= s.req_end) s.requesting = false;
  if (s.state == PlayerBuffering && !s.timer) prv_fill(NULL);
}

void player_on_remote_error(uint16_t id, const char *reason) {
  if (id != s.id) return;
  s.error = reason && reason[0] ? reason : tr(S_P_PHONE_FAILED);
  player_pause();
  prv_notify();
}

#else  // no speaker

bool player_available(void) { return false; }
bool player_muted(void) { return false; }
void player_set_volume(uint8_t volume) {}
bool player_open(uint16_t id) { return false; }
void player_close(void) {}
void player_play(void) {}
void player_pause(void) {}
void player_toggle(void) {}
void player_seek_ms(uint32_t ms) {}
void player_skip_ms(int32_t delta_ms) {}
uint32_t player_position_ms(void) { return 0; }
uint32_t player_duration_ms(void) { return 0; }
PlayerState player_state(void) { return PlayerStopped; }
const char *player_error(void) { return tr(S_NO_SPEAKER); }
void player_set_listener(PlayerListener listener) {}
void player_on_remote_data(uint16_t id, uint16_t block, const uint8_t *data, uint16_t len) {}
void player_on_remote_error(uint16_t id, const char *reason) {}

#endif
