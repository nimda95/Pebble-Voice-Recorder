# Voice Recorder for Pebble

A voice recorder for the new Pebble watches. It uses the watch microphone through the PebbleOS
4.44 microphone API, is driven entirely by the buttons, and is built only from PebbleOS UI
components: status bar, action bar, menu layer, action menu and the standard dialog layout. A phone
companion stores recordings, records straight to the phone, transcribes, and uploads to WebDAV. It
also serves a live settings page inside the Pebble app.

## Watches

| Watch | Platform | Mic | Speaker (playback) |
| --- | --- | --- | --- |
| Pebble Time 2 | emery | yes | yes |
| Pebble 2 Duo | flint | yes | yes |
| Pebble Round 2 | gabbro | yes | no (transcripts only) |

The original Pebble, Pebble Time, Time Round and Pebble 2 are not targets. On those platforms the
SDK compiles the microphone and speaker calls out as stubs, and app storage is 4 KB.

It needs watch firmware with the microphone API (SDK 4.44) and the Core Devices Pebble app. The
settings page uses that app's live config-page bridge.

## Build and install

```sh
pebble build
pebble install --phone <phone-ip>       # or --emulator emery
```

On first use, allow the microphone for Voice Recorder in the Pebble app (the app declares
`WatchMicrophone`). It also declares `Internet` and `LocalNetwork` so it can reach your
transcription and WebDAV servers.

## On the watch

The app opens straight to the recorder. The screen always shows space left and time left, both on
standby and while recording.

| State | Up | Select | Down |
| --- | --- | --- | --- |
| Standby | Past recordings | Record | Settings |
| Recording | Stop & save | Pause | Discard (asks; Up confirms) |
| Paused | Stop & save | Continue | Discard (asks; Up confirms) |

- **Quick Launch:** with *Settings → Quick Launch → Starts recording*, opening the app from Quick
  Launch buzzes once and starts recording. The buzz finishes before the mic opens, so it isn't
  recorded.
- **Back** while recording saves the recording and exits.
- **Interruptions:** a notification or the system taking the mic pauses the recording. It resumes
  when the app is back in front.
- **Recording health** (shown while recording):
  - *Good*
  - *Slow storage*: a storage write stalled long enough to risk overrunning the mic buffer.
  - *Storage low*: under 20 s left.
  - *Dropping audio*: frames arrived fewer than the clock says were due, and still hadn't arrived
    a second later.
  - *Phone lagging* / *Phone silent*: when recording to the phone, the phone reports every 2 s how
    much audio it has. These show when it falls behind or stops reporting.

  Lost time is saved with each recording and shown as a "lost" badge on the phone.
- **Recordings list:** oldest at the top, newest at the bottom, opened on the newest. The title is
  the date and time (or the name you give it on the phone). The subtitle is size and duration,
  prefixed with *Phone* or *Text* when the audio isn't on the watch. It also shows *Transcribing*,
  *Uploading* or *Saving to phone* while a job runs.
- **Select a recording** to open the action menu:
  - Play
  - Transcript
  - Transcribe: pick a language; the last one you used is listed first
  - Upload to WebDAV
  - Save to phone
  - Delete: from the watch, or everywhere

  Transcribe and Upload appear once they are set up on the phone.
- **Transcript view:** one row per timed segment. During playback the highlight follows the audio.
  Up/Down moves between segments (and seeks while playing); Select plays or pauses. Times shown as
  `~0:12` are estimated.
- **Settings:**
  - Quality: Standard 8 kHz, or High 16 kHz
  - Save to: Watch, or Phone
  - Quick Launch behaviour
  - Storage: watch and phone space, time left, counts, delete all on the watch

## Storage

Audio is IMA ADPCM in 256-byte blocks, the WAV block layout, so every block decodes on its own:

| Quality | Rate | Data | About per MB |
| --- | --- | --- | --- |
| Standard | 8 kHz | 4 KB/s | 4 min |
| High | 16 kHz | 8 KB/s | 2 min |

- **Watch:** recordings go into the app's persistent storage, which is 1 MB on firmware 4.44
  (`persist_get_max_size()`); that's about 4 minutes at Standard. Deleting a 256-byte block is slow
  on a full store, so deletes run in the background and pause while you record or play.
- **Phone:** *Save to → Phone* streams the mic to the phone, so length is limited only by phone
  storage. If the phone stops answering, the watch keeps what the phone got and carries on recording
  on the watch, with a double buzz and a notice.
- **Phone storage:** recordings live in the Pebble app's storage for this app. The app doesn't
  publish how big that is, so the settings page has a limit (5 MB by default, about 20 minutes at
  Standard). Raise it if your phone allows more. If the phone does run out, the recording stops and
  is saved.

## Settings page (phone)

Open Voice Recorder on the watch, then open its settings in the Pebble app. The page talks to the
running app live:

- **Recorder card:** record, pause, resume, stop and discard on the watch from the phone. Shows a
  live timer and the health indicator.
- **Recordings:**
  - Play in the page, fetching from the watch if needed.
  - Transcript with timestamps: tap a time to seek; the line being played is highlighted.
  - Save to phone, export `.wav`, transcribe (language defaults to the last used), upload, rename,
    and delete (watch, phone, or everywhere).
- **Settings:**
  - Quality, target and Quick Launch.
  - After each recording: save to phone (Copy, or Move to free the watch), transcribe, and upload.
  - Phone storage limit.
  - Transcription and WebDAV setup, each with a Test button.

Settings save as you change them. Export uses the share sheet or a download where the Pebble app's
web view allows it; if it doesn't, use WebDAV.

## Transcription

- **OpenAI-compatible:** endpoint, API key and model, for example OpenAI `whisper-1`, or
  faster-whisper-server / Speaches, LocalAI, whisper.cpp server or Groq. The app asks for
  `verbose_json` segments, which give real timestamps. Models that only return text (for example
  `gpt-4o-transcribe`) get times spread over the recording, shown with `~`.
- **Wyoming faster-whisper:** enter your Home Assistant address, a long-lived access token and the
  speech-to-text entity (for example `stt.faster_whisper`). A phone can't open Wyoming's raw TCP
  socket, so this goes through Home Assistant's `/api/stt/<entity>` endpoint, which fronts the
  Wyoming integration. That endpoint returns text only, so the app splits the audio at pauses
  (pieces of 3 to 25 s) and transcribes each piece; every line carries its real start time.
- **Language:** you always pick one; the last pick is the default on both the watch and the phone.

## WebDAV

Enter the server address (for Nextcloud:
`https://cloud.example.com/remote.php/dav/files/<user>`), username, password and folder. Uploads
create the folder if needed. Each recording uploads as `<name or "Recording"> (<date time>).wav`,
plus a `.txt` transcript with timestamps if you turn that on.

## Layout

```
src/c/
  main.c          start-up and shutdown (saves a recording in progress)
  recorder.c      local capture, phone streaming, fallback, pause/resume, health
  storage.c       recordings in persistent storage, space accounting, crash recovery, background deletes
  adpcm.c         IMA ADPCM encoder/decoder, 16 -> 8 kHz half-band decimator
  comm.c          AppMessage protocol, reliable outbox, transfers, list sync
  player.c        speaker playback from the watch copy or streamed from the phone
  ui_*.c          recorder, list + action menu, settings, playback/transcript
  dialog.c        message and choice dialogs
src/pkjs/
  index.js        phone companion: watch protocol, phone streaming, jobs, settings-page bridge
  store.js        phone storage (settings, index, chunked audio, transcripts)
  services.js     OpenAI-compatible, Home Assistant STT, WebDAV
  codec.js        ADPCM, WAV, resampling, base64, UTF-8 (no Blob/FormData in PebbleKit JS)
src/config/index.html   the settings page (bundled into the app with "configPage")
tools/make_icons.py     draws the icons in resources/images
tools/make_banner.py    draws the appstore banners from the screenshots
screenshots/<platform>/ emulator screenshots of every screen
store/<platform>/       appstore set: banner.png (720x320) and five screenshots
```

To redraw a banner: `python tools/make_banner.py emery <font dir> store/emery/banner.png`. The font
directory needs `BarlowCondensed-Bold.ttf` and `Barlow-Medium.ttf` from Google Fonts.

The message protocol is listed in `src/c/comm.h`, mirrored in `src/pkjs/protocol.js`.

## Tested

Tested in the SDK 4.44 emulator:

- Recording on emery, flint and gabbro with a test tone fed into the mic. It decodes back at the
  right pitch, both from the watch copy and when recorded straight to the phone.
- Pause, discard and save.
- Watch → phone transfer.
- Phone → watch streaming for playback.
- Transcription (OpenAI-compatible and Home Assistant) and WebDAV against a mock server.
- The settings page, driven against the emulator's phone runtime.

Not covered:

- **Speaker timing.** The emulator's speaker accepts the first half-second and never drains, so
  playback timing and the transcript follow-along need checking on a real watch.
- **Real-hardware storage speed.** On the emulator, writes stayed fast enough at 16 kHz with the
  store 60% full, and no mic frames were lost.
- **The Core app's web view.** Downloads from the settings page, and storage size, depend on it.
