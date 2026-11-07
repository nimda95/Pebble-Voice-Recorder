#pragma once
#include "common.h"
#include "storage.h"

// Message protocol; mirrored in src/pkjs/protocol.js.
// Every message carries Cmd, plus Id / A / B / C / D (ints), Data (bytes) and Str as needed.

// Watch -> phone
enum {
  W_HELLO = 1,        // A=capacity B=used C=features D=inbox blocks Str="version ui-lang"
  W_LIST_BEGIN = 2,   // A=count
  W_LIST_ITEM = 3,    // Id A=created B=samples C=flags|rate<<8|blocks<<16 D=lost_ds Str=name
  W_LIST_END = 4,     // A=used B=capacity C=free
  W_SETTINGS = 5,     // A=quality B=target C=ql_autostart D=gain Str="lang tlang"
  W_REC_STATE = 6,    // Id A=state B=elapsed_ms C=mode D=health|reason<<8
  W_REC_BEGIN = 7,    // Id A=rate_code B=created C=gain  (phone recording)
  W_REC_PAUSE = 8,    // Id
  W_REC_RESUME = 9,   // Id
  W_REC_END = 10,     // Id A=elapsed_ms
  W_REC_DISCARD = 11, // Id
  W_AUDIO = 12,       // Id A=first block Data=blocks
  W_AUDIO_END = 13,   // Id A=blocks B=samples C=rate_code D=created
  W_ACTION = 14,      // Id A=action Str=lang
  W_TR_REQ = 15,      // Id A=0 transcript, 1 translation
  W_PLAY_REQ = 16,    // Id A=first block B=count
  W_DELETED = 17,     // Id                               (watch copy deleted)
  W_SAVED = 18,       // same fields as W_LIST_ITEM       (new watch recording)
  W_PULL_ERR = 19,    // Id A=reason
};

// Phone -> watch
enum {
  P_HELLO = 101,        // A=caps B=phone_free C=phone_used D=highest phone id
  P_SETTINGS = 102,     // A=quality B=target C=ql_autostart D=gain Str="lang tlang"
  P_CONTROL = 103,      // A=control
  P_LIST_REQ = 104,
  P_RENAME = 105,       // Id Str=name
  P_DELETE = 106,       // Id                             (delete the watch copy)
  P_PULL = 107,         // Id                             (send the audio)
  P_PHONE_BEGIN = 108,
  P_PHONE_ITEM = 109,   // Id A=created B=samples C=flags|rate<<8 D=lost_ds Str=name
  P_PHONE_END = 110,
  P_TR_BEGIN = 111,     // Id A=count B=estimated          (transcript or translation)
  P_TR_SEG = 112,       // Id A=index B=start_ms Str=text
  P_TR_END = 113,       // Id
  P_TR_NONE = 114,      // Id Str=reason
  P_PLAY_DATA = 115,    // Id A=first block Data=blocks
  P_PLAY_ERR = 116,     // Id Str=reason
  P_JOB = 117,          // Id A=kind B=status C=percent Str=message
  P_STREAM_STAT = 118,  // Id A=received_ms
  P_STORAGE = 119,      // B=phone_free C=phone_used
  P_REC_SAVED = 120,    // Id A=created B=samples C=flags|rate<<8 D=lost_ds
  P_PHONE_GONE = 121,   // Id                             (phone copy and transcript removed)
};

enum { ActTranscribe = 1, ActUpload = 2, ActSaveToPhone = 3, ActDeletePhone = 4, ActDeleteAll = 5,
       ActTranslate = 6 };
enum { CtlStart = 1, CtlPause = 2, CtlResume = 3, CtlStop = 4, CtlDiscard = 5 };
enum { JobTranscribe = 1, JobUpload = 2, JobSave = 3, JobTranslate = 4 };
enum { JobQueued = 0, JobRunning = 1, JobDone = 2, JobFailed = 3 };

#define FEATURE_SPEAKER 0x01
#define FEATURE_ROUND   0x02

typedef struct {
  uint16_t id;
  uint8_t kind;
  uint8_t status;
  uint8_t pct;
} JobStatus;

typedef enum { CommEvList, CommEvJob, CommEvJobFailed, CommEvPhone } CommEvent;
typedef void (*CommListener)(CommEvent ev, uint16_t id, const char *text);

typedef struct {
  void (*begin)(uint16_t id, uint16_t count, bool estimated);
  void (*segment)(uint16_t id, uint16_t index, uint32_t start_ms, const char *text);
  void (*end)(uint16_t id);
  void (*fail)(uint16_t id, const char *reason);
} TranscriptSink;

void comm_init(void);
void comm_deinit(void);

bool comm_phone_ready(void);
uint16_t comm_inbox_blocks(void);

void comm_send_settings(void);
// Sends the whole watch list (the phone replaces its view of the watch with it).
void comm_send_list(void);
void comm_send_rec_state(void);
void comm_send_item(uint8_t cmd, const RecMeta *m);
void comm_send_simple(uint8_t cmd, uint16_t id, int32_t a);
void comm_send_rec_begin(uint16_t id, uint8_t rate_code, uint32_t created, uint8_t gain);
void comm_send_action(uint16_t id, uint8_t action, const char *lang);
// translation: the translation instead of the transcript
void comm_request_transcript(uint16_t id, bool translation);
void comm_request_play(uint16_t id, uint16_t block, uint16_t count);

const JobStatus *comm_job_for(uint16_t id);

void comm_add_listener(CommListener l);
void comm_remove_listener(CommListener l);
void comm_set_transcript_sink(const TranscriptSink *sink);
