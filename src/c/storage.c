#include "storage.h"

// Room kept for settings, the delete queue and the settings file's own slack.
#define RESERVE_BYTES 4096
#define MAX_DELQ 40   // 6 bytes each, one persist value

typedef struct {
  RecMeta m;
  uint8_t slot;
} Entry;

typedef struct __attribute__((packed)) {
  uint16_t id;
  uint16_t next;
  uint16_t end;
} DelJob;

static Entry s_entries[MAX_RECS];
static int s_count;
static uint64_t s_slots;
static uint32_t s_capacity;

static DelJob s_delq[MAX_DELQ];
static int s_delq_count;
static AppTimer *s_delete_timer;
static uint8_t s_busy;
static int s_ticks_since_save;

// ---- Helpers ------------------------------------------------------------------------------------

static Entry *prv_entry(uint16_t id) {
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].m.id == id) return &s_entries[i];
  }
  return NULL;
}

static int prv_alloc_slot(void) {
  for (int i = 0; i < MAX_RECS; i++) {
    if (!(s_slots & ((uint64_t)1 << i))) {
      s_slots |= ((uint64_t)1 << i);
      return i;
    }
  }
  return -1;
}

static bool prv_before(const RecMeta *a, const RecMeta *b) {
  return a->created < b->created || (a->created == b->created && a->id < b->id);
}

static int prv_insert(const Entry *e) {
  int pos = s_count;
  while (pos > 0 && prv_before(&e->m, &s_entries[pos - 1].m)) pos--;
  memmove(&s_entries[pos + 1], &s_entries[pos], (s_count - pos) * sizeof(Entry));
  s_entries[pos] = *e;
  s_count++;
  return pos;
}

static void prv_save_delq(void) {
  if (s_delq_count == 0) {
    persist_delete(PKEY_DELETE_QUEUE);
  } else {
    persist_write_data(PKEY_DELETE_QUEUE, s_delq, s_delq_count * sizeof(DelJob));
  }
  s_ticks_since_save = 0;
}

static void prv_delete_tick(void *ctx);

static void prv_kick_deleter(uint32_t delay) {
  if (s_delete_timer || s_delq_count == 0) return;
  s_delete_timer = app_timer_register(delay, prv_delete_tick, NULL);
}

static void prv_run_job_now(DelJob *j) {
  while (j->next < j->end) persist_delete(PKEY_AUDIO(j->id, j->next++));
}

static void prv_queue_delete(uint16_t id, uint16_t blocks) {
  if (blocks == 0) return;
  if (s_delq_count == MAX_DELQ) {
    // Queue full: finish the oldest job right away.
    prv_run_job_now(&s_delq[0]);
    memmove(&s_delq[0], &s_delq[1], (MAX_DELQ - 1) * sizeof(DelJob));
    s_delq_count--;
  }
  s_delq[s_delq_count++] = (DelJob){ .id = id, .next = 0, .end = blocks };
  prv_save_delq();
  prv_kick_deleter(200);
}

static void prv_delete_tick(void *ctx) {
  s_delete_timer = NULL;
  if (s_delq_count == 0) return;
  if (s_busy) {
    s_delete_timer = app_timer_register(1000, prv_delete_tick, NULL);
    return;
  }
  int32_t start = now_ms();
  int done = 0;
  while (s_delq_count > 0 && done < 24) {
    DelJob *j = &s_delq[0];
    if (j->next >= j->end) {
      memmove(&s_delq[0], &s_delq[1], (s_delq_count - 1) * sizeof(DelJob));
      s_delq_count--;
      prv_save_delq();
      continue;
    }
    persist_delete(PKEY_AUDIO(j->id, j->next));
    j->next++;
    done++;
    if (now_ms() - start > 30) break;
  }
  if (++s_ticks_since_save >= 8) prv_save_delq();
  if (s_delq_count > 0) {
    s_delete_timer = app_timer_register(60, prv_delete_tick, NULL);
  }
}

// ---- Lifecycle ----------------------------------------------------------------------------------

void storage_init(void) {
  s_capacity = persist_get_max_size();
  s_count = 0;
  s_slots = 0;

  int n = persist_read_data(PKEY_DELETE_QUEUE, s_delq, sizeof(s_delq));
  s_delq_count = n > 0 ? n / (int)sizeof(DelJob) : 0;

  for (int slot = 0; slot < MAX_RECS; slot++) {
    uint32_t key = PKEY_META_BASE + slot;
    if (!persist_exists(key)) continue;
    Entry e;
    if (persist_read_data(key, &e.m, sizeof(RecMeta)) != (int)sizeof(RecMeta) || e.m.id == 0 ||
        prv_entry(e.m.id)) {
      persist_delete(key);
      continue;
    }
    e.slot = slot;
    s_slots |= ((uint64_t)1 << slot);

    if (e.m.flags & RecOpen) {
      // The app stopped while recording: keep every block that made it to storage.
      if (e.m.flags & RecLocal) {
        uint16_t b = e.m.blocks;
        while (b < MAX_BLOCKS_PER_REC && persist_exists(PKEY_AUDIO(e.m.id, b))) b++;
        e.m.blocks = b;
        if (e.m.samples < (uint32_t)b * ADPCM_BLOCK_SAMPLES) {
          e.m.samples = (uint32_t)b * ADPCM_BLOCK_SAMPLES;
        }
        if (b == 0) e.m.flags &= ~RecLocal;
      }
      e.m.flags &= ~RecOpen;
      if (!(e.m.flags & (RecLocal | RecPhone | RecTranscript))) {
        persist_delete(key);
        s_slots &= ~((uint64_t)1 << slot);
        continue;
      }
      persist_write_data(key, &e.m, sizeof(RecMeta));
    }
    prv_insert(&e);
  }
  prv_kick_deleter(1500);
}

void storage_deinit(void) {
  if (s_delete_timer) {
    app_timer_cancel(s_delete_timer);
    s_delete_timer = NULL;
  }
  prv_save_delq();
}

// ---- Entries ------------------------------------------------------------------------------------

int storage_count(void) { return s_count; }

RecMeta *storage_get(int index) {
  return (index >= 0 && index < s_count) ? &s_entries[index].m : NULL;
}

RecMeta *storage_find(uint16_t id) {
  Entry *e = prv_entry(id);
  return e ? &e->m : NULL;
}

int storage_index_of(uint16_t id) {
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].m.id == id) return i;
  }
  return -1;
}

bool storage_id_in_use(uint16_t id) {
  if (prv_entry(id)) return true;
  for (int i = 0; i < s_delq_count; i++) {
    if (s_delq[i].id == id) return true;
  }
  return false;
}

RecMeta *storage_create(uint16_t id, uint8_t rate_code, uint32_t created, uint8_t flags) {
  if (s_count >= MAX_RECS || prv_entry(id)) return NULL;
  int slot = prv_alloc_slot();
  if (slot < 0) return NULL;
  Entry e;
  memset(&e, 0, sizeof(e));
  e.slot = slot;
  e.m.id = id;
  e.m.rate_code = rate_code;
  e.m.created = created;
  e.m.flags = flags;
  int pos = prv_insert(&e);
  return &s_entries[pos].m;
}

void storage_commit(const RecMeta *m) {
  Entry *e = prv_entry(m->id);
  if (!e) return;
  if (&e->m != m) e->m = *m;
  persist_write_data(PKEY_META_BASE + e->slot, &e->m, sizeof(RecMeta));
}

status_t storage_append_block(RecMeta *m, const uint8_t *block) {
  if (m->blocks >= MAX_BLOCKS_PER_REC) return E_OUT_OF_STORAGE;
  int r = persist_write_data(PKEY_AUDIO(m->id, m->blocks), block, ADPCM_BLOCK_BYTES);
  if (r == ADPCM_BLOCK_BYTES) {
    m->blocks++;
    return S_SUCCESS;
  }
  return r < 0 ? r : E_ERROR;
}

bool storage_read_block(uint16_t id, uint16_t block, uint8_t *out) {
  return persist_read_data(PKEY_AUDIO(id, block), out, ADPCM_BLOCK_BYTES) == ADPCM_BLOCK_BYTES;
}

static void prv_drop_entry(Entry *e) {
  persist_delete(PKEY_META_BASE + e->slot);
  s_slots &= ~((uint64_t)1 << e->slot);
  int i = e - s_entries;
  memmove(&s_entries[i], &s_entries[i + 1], (s_count - i - 1) * sizeof(Entry));
  s_count--;
}

void storage_delete_local(uint16_t id) {
  Entry *e = prv_entry(id);
  if (!e) return;
  if (e->m.flags & RecLocal) {
    prv_queue_delete(id, e->m.blocks);
  }
  e->m.flags &= ~RecLocal;
  e->m.blocks = 0;
  if (!(e->m.flags & (RecPhone | RecTranscript))) {
    prv_drop_entry(e);
  } else {
    storage_commit(&e->m);
  }
}

void storage_remove(uint16_t id) {
  Entry *e = prv_entry(id);
  if (!e) return;
  if (e->m.blocks) prv_queue_delete(id, e->m.blocks);
  prv_drop_entry(e);
}

void storage_delete_all_local(void) {
  uint16_t ids[MAX_RECS];
  int n = 0;
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].m.flags & RecLocal) ids[n++] = s_entries[i].m.id;
  }
  for (int i = 0; i < n; i++) storage_delete_local(ids[i]);
}

// ---- Space --------------------------------------------------------------------------------------

uint32_t storage_capacity(void) { return s_capacity; }

uint32_t storage_used(void) {
  uint32_t used = RESERVE_BYTES;
  for (int i = 0; i < s_count; i++) {
    used += (uint32_t)s_entries[i].m.blocks * BLOCK_COST + sizeof(RecMeta) + PERSIST_KEY_OVERHEAD;
  }
  for (int i = 0; i < s_delq_count; i++) {
    used += (uint32_t)(s_delq[i].end - s_delq[i].next) * BLOCK_COST;
  }
  return used;
}

uint32_t storage_free(void) {
  uint32_t used = storage_used();
  return s_capacity > used ? s_capacity - used : 0;
}

uint32_t storage_seconds_left(uint8_t rate_code) {
  uint32_t blocks = storage_free() / BLOCK_COST;
  return blocks * ADPCM_BLOCK_SAMPLES / rate_for_code(rate_code);
}

int storage_local_count(void) {
  int n = 0;
  for (int i = 0; i < s_count; i++) n += (s_entries[i].m.flags & RecLocal) ? 1 : 0;
  return n;
}

int storage_phone_count(void) {
  int n = 0;
  for (int i = 0; i < s_count; i++) n += (s_entries[i].m.flags & RecPhone) ? 1 : 0;
  return n;
}

uint32_t storage_size_bytes(const RecMeta *m) {
  uint32_t blocks = m->blocks ? m->blocks : (m->samples + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES;
  return blocks * ADPCM_BLOCK_BYTES;
}

uint32_t storage_duration_ms(const RecMeta *m) {
  return (uint32_t)(((uint64_t)m->samples * 1000) / rate_for_code(m->rate_code));
}

void storage_set_busy(uint8_t reason, bool busy) {
  if (busy) s_busy |= reason;
  else s_busy &= ~reason;
  if (!s_busy) prv_kick_deleter(500);
}

bool storage_deleting(void) { return s_delq_count > 0; }
