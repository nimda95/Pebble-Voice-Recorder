#pragma once
#include "common.h"

#define MAX_RECS 48
#define REC_NAME_LEN 24

enum {
  RecLocal      = 0x01,  // audio blocks are on the watch
  RecPhone      = 0x02,  // audio is stored on the phone
  RecOpen       = 0x04,  // still being recorded (cleared on save; recovered after a crash)
  RecTranscript = 0x08,  // the phone has a transcript
  RecUploaded   = 0x10,  // uploaded to WebDAV
};

typedef struct __attribute__((packed)) {
  uint16_t id;
  uint8_t flags;
  uint8_t rate_code;     // 0 = 8 kHz, 1 = 16 kHz
  uint32_t created;      // UTC seconds
  uint32_t samples;
  uint16_t blocks;       // blocks stored on the watch
  uint16_t lost_ds;      // audio lost while recording, tenths of a second
  char name[REC_NAME_LEN];
} RecMeta;

void storage_init(void);
void storage_deinit(void);

int storage_count(void);
// Sorted oldest first. Pointers stay valid only until the next create/remove.
RecMeta *storage_get(int index);
RecMeta *storage_find(uint16_t id);
int storage_index_of(uint16_t id);
bool storage_id_in_use(uint16_t id);

RecMeta *storage_create(uint16_t id, uint8_t rate_code, uint32_t created, uint8_t flags);
void storage_commit(const RecMeta *m);
// Appends one block to a local recording. Returns the persist status.
status_t storage_append_block(RecMeta *m, const uint8_t *block);
bool storage_read_block(uint16_t id, uint16_t block, uint8_t *out);

// Frees the watch copy (blocks are deleted in the background). The entry stays when another
// copy (phone audio or transcript) exists.
void storage_delete_local(uint16_t id);
// Drops the entry and any watch audio.
void storage_remove(uint16_t id);
// Drops every watch copy.
void storage_delete_all_local(void);

uint32_t storage_capacity(void);
uint32_t storage_used(void);
uint32_t storage_free(void);
uint32_t storage_seconds_left(uint8_t rate_code);
int storage_local_count(void);
int storage_phone_count(void);
uint32_t storage_size_bytes(const RecMeta *m);
uint32_t storage_duration_ms(const RecMeta *m);

// Background deletion yields while any of these is going on.
enum { BusyRecording = 0x01, BusyPlaying = 0x02, BusyTransfer = 0x04 };
void storage_set_busy(uint8_t reason, bool busy);
bool storage_deleting(void);
