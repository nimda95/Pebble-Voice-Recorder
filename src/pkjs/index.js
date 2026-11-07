// Voice Recorder phone companion: stores recordings on the phone, records straight to the phone,
// runs transcription, translation and WebDAV uploads, and serves the settings page over the live
// config bridge. Messages go to the watch in its language and to the page in the page's.

var P = require('./protocol');
var codec = require('./codec');
var store = require('./store');
var services = require('./services');
var LANGS = require('./languages');
var i18n = require('./i18n');
var E = i18n.error;
var M = i18n.msg;

var watch = {
  connected: false,
  info: null,                 // from W_HELLO
  lang: 'en',                 // the watch app's language
  list: {},                   // id -> watch entry
  listing: null,              // list being received
  recorder: { state: 0, elapsed: 0, id: 0, mode: 0, health: 0, reason: 0, at: 0 }
};
var pageOpen = false;
var pageLang = 'en';

function langName(code) {
  var l = LANGS.filter(function (x) { return x.code === code; })[0];
  return l ? l.name : code;
}

// ---- Messaging to the watch ---------------------------------------------------------------------

var outbox = [];
var sending = false;

function send(msg) {
  return new Promise(function (resolve, reject) {
    outbox.push({ msg: msg, resolve: resolve, reject: reject, tries: 0 });
    pump();
  });
}

function pump() {
  if (sending || !outbox.length) return;
  var item = outbox[0];
  sending = true;
  Pebble.sendAppMessage(item.msg, function () {
    sending = false;
    outbox.shift();
    item.resolve();
    pump();
  }, function () {
    sending = false;
    if (++item.tries >= 4) {
      outbox.shift();
      item.reject(E("The watch didn't answer"));
    }
    setTimeout(pump, 300 * item.tries);
  });
}

function sendQuiet(msg) {
  return send(msg).catch(function () {});
}

function bytesOf(data) {
  if (!data) return new Uint8Array(0);
  if (data instanceof Uint8Array) return data;
  return new Uint8Array(data);
}

function arrayOf(u8) {
  return Array.prototype.slice.call(u8);
}

function caps() {
  var s = store.getSettings();
  var tl = services.translationMode(s);
  return (services.transcriptionReady(s.transcription) ? P.CAP_TRANSCRIBE : 0) |
         (services.webdavReady(s.webdav) ? P.CAP_WEBDAV : 0) |
         (tl === 'any' ? P.CAP_TRANSLATE : tl === 'en' ? P.CAP_TRANSLATE_EN : 0);
}

function settingsMessage(s) {
  return { Cmd: P.P_SETTINGS, A: s.quality, B: s.target, C: s.quickLaunch ? 1 : 0, D: s.gain,
           Str: s.lastLang + ' ' + s.lastTranslateLang };
}

var lastHello = 0;
function sendHello() {
  lastHello = Date.now();
  var u = store.usage();
  var s = store.getSettings();
  sendQuiet({ Cmd: P.P_HELLO, A: caps(), B: u.freeBytes, C: u.usedBytes, D: store.maxId() });
  sendQuiet(settingsMessage(s));
}

var storageTimer = null;
function sendStorage() {
  if (storageTimer) return;
  storageTimer = setTimeout(function () {
    storageTimer = null;
    var u = store.usage();
    sendQuiet({ Cmd: P.P_STORAGE, B: u.freeBytes, C: u.usedBytes });
    pushState();
  }, 500);
}

function phoneFlags(m) {
  return (m.audio ? P.REC_PHONE : 0) | (m.transcript ? P.REC_TRANSCRIPT : 0) | (m.uploaded ? P.REC_UPLOADED : 0) |
         (m.translation ? P.REC_TRANSLATION : 0);
}

function listedOnPhone(m) { return m.audio || m.transcript || m.uploaded || m.translation; }

function sendPhoneItem(m) {
  if (!m) return Promise.resolve();
  if (!listedOnPhone(m)) return sendQuiet({ Cmd: P.P_PHONE_GONE, Id: m.id });
  return sendQuiet({
    Cmd: P.P_PHONE_ITEM, Id: m.id, A: m.created, B: m.samples, C: phoneFlags(m) | ((m.rate || 0) << 8),
    D: m.lost || 0, Str: (m.name || '').slice(0, 23)
  });
}

function sendPhoneList() {
  var items = store.all().filter(listedOnPhone);
  sendQuiet({ Cmd: P.P_PHONE_BEGIN, A: items.length });
  items.forEach(sendPhoneItem);
  sendQuiet({ Cmd: P.P_PHONE_END });
}

// ---- Watch list ---------------------------------------------------------------------------------

function parseItem(p) {
  var c = p.C >>> 0;
  return {
    id: p.Id, created: p.A >>> 0, samples: p.B >>> 0, flags: c & 0xff, rate: (c >> 8) & 0xff,
    blocks: c >>> 16, lost: p.D || 0, name: p.Str || ''
  };
}

// The same id with a different recording time means the watch was reset and reused the id:
// keep the phone's old recording under a new id instead of mixing the two.
function resolveConflict(id, created) {
  var m = store.get(id);
  if (!m || m.open || !created || !m.created || Math.abs(m.created - created) <= 30) return false;
  var moved = store.freePhoneId();
  store.rekey(id, moved);
  console.log('Recording ' + id + ' on the phone is older than the watch\'s; kept as ' + moved);
  return true;
}

function adoptWatchItem(item) {
  if ((item.flags & P.REC_LOCAL) && resolveConflict(item.id, item.created)) sendPhoneList();
  var m = store.get(item.id);
  if (m && item.name && !m.name) {
    m.name = item.name;
    store.save();
  }
}

// ---- Jobs (transcribe, upload, save to phone), one at a time ------------------------------------

var jobs = [];
var current = null;
var KIND_CODE = { transcribe: P.JOB_TRANSCRIBE, upload: P.JOB_UPLOAD, save: P.JOB_SAVE, translate: P.JOB_TRANSLATE };
var RUN = {};  // kind -> function(job), filled in below
var STATUS_CODE = { queued: P.JOB_QUEUED, running: P.JOB_RUNNING, done: P.JOB_DONE, failed: P.JOB_FAILED };

function enqueue(kind, id, opts) {
  var dup = function (j) { return j.kind === kind && j.id === id; };
  if (jobs.some(dup) || (current && dup(current))) return;
  var job = { kind: kind, id: id, opts: opts || {}, status: 'queued', pct: 0, msg: null };
  jobs.push(job);
  report(job);
  runJobs();
}

function report(job) {
  sendQuiet({ Cmd: P.P_JOB, Id: job.id, A: KIND_CODE[job.kind], B: STATUS_CODE[job.status],
              C: job.pct || 0, Str: i18n.text(watch.lang, job.msg).slice(0, 90) });
  push({ type: 'job', job: jobView(job) });
}

function jobView(j) {
  return { kind: j.kind, id: j.id, status: j.status, pct: j.pct, msg: i18n.text(pageLang, j.msg) };
}

function progress(job, pct) {
  if (!job) return;
  pct = Math.max(0, Math.min(99, Math.round(pct)));
  var now = Date.now();
  if (pct === job.pct || now - (job.reportedAt || 0) < 1000) {
    job.pct = pct;
    return;
  }
  job.pct = pct;
  job.reportedAt = now;
  report(job);
}

function runJobs() {
  if (current || !jobs.length) return;
  var job = current = jobs.shift();
  job.status = 'running';
  report(job);
  var run = RUN[job.kind];
  var p;
  try {
    p = run(job);
  } catch (e) {
    p = Promise.reject(e);
  }
  p.then(function (msg) {
    job.status = 'done';
    job.msg = msg || null;
  }, function (err) {
    job.status = 'failed';
    job.msg = err && (err.template || err.message) ? err : M('Something went wrong');
    console.log('Job ' + job.kind + ' ' + job.id + ' failed: ' + i18n.text('en', job.msg));
  }).then(function () {
    job.pct = 100;
    report(job);
    current = null;
    pushRecordings();
    runJobs();
  });
}

// Everything known about a recording, from the watch list and the phone index.
function meta(id) {
  var w = watch.list[id] || {};
  var m = store.get(id) || {};
  return {
    id: id,
    created: m.created || w.created || Math.floor(Date.now() / 1000),
    samples: m.audio ? m.samples : (w.samples || m.samples || 0),
    rate: m.audio ? m.rate : (w.rate !== undefined ? w.rate : (m.rate || 0)),
    name: m.name || w.name || '',
    lost: m.lost || w.lost || 0
  };
}

function ensurePhoneMeta(id) {
  var x = meta(id);
  return store.ensure(id, { created: x.created, name: x.name, lost: x.lost,
                            rate: store.get(id) && store.get(id).audio ? store.get(id).rate : x.rate,
                            samples: store.get(id) && store.get(id).audio ? store.get(id).samples : x.samples });
}

// The recording's audio: the phone copy, or fetched from the watch.
function getAudio(id, job) {
  var m = store.get(id);
  if (m && m.audio) {
    return Promise.resolve({ bytes: store.readAudio(id), rate: m.rate, samples: m.samples, created: m.created });
  }
  var w = watch.list[id];
  if (w && (w.flags & P.REC_LOCAL)) {
    return pull(id, function (pct) { progress(job, pct * (job && job.kind === 'save' ? 1 : 0.4)); });
  }
  return Promise.reject(E(watch.connected ? 'The audio is no longer on the watch or the phone'
                                          : 'Open Voice Recorder on the watch to fetch the audio'));
}

function doSave(job) {
  return pull(job.id, function (pct) { progress(job, pct); }).then(function (a) {
    var x = meta(job.id);
    try {
      store.writeAll(job.id, a.bytes, { created: a.created || x.created, samples: a.samples, rate: a.rate,
                                         name: x.name, lost: x.lost, open: false });
    } catch (e) {
      throw e.quota ? E('Phone storage is full') : (e.message ? e : E('Couldn\'t store the recording'));
    }
    sendPhoneItem(store.get(job.id));
    sendStorage();
    if (job.opts.move) sendQuiet({ Cmd: P.P_DELETE, Id: job.id });
    return M(job.opts.move ? 'Moved to phone' : 'Saved to phone');
  });
}

// Transcribes a recording and stores the transcript. span: the share of the job's progress bar
// this takes (translation runs it first when there's no transcript yet).
function transcribeInto(job, lang, span) {
  var s = store.getSettings();
  return getAudio(job.id, job).then(function (a) {
    var pcm = codec.decodeBlocks(a.bytes, a.samples);
    var rate = codec.rateFor(a.rate);
    var base = job.pct || 0;
    return services.transcribe(pcm, rate, lang, s.transcription, function (pct) {
      progress(job, base + (span - base) * pct / 100);
    }).then(function (r) {
      ensurePhoneMeta(job.id);
      var t = { lang: lang, text: r.text, segments: r.segments, estimated: r.estimated,
                provider: s.transcription.provider, at: Date.now() };
      store.saveTranscript(job.id, t);
      sendPhoneItem(store.get(job.id));
      sendStorage();
      return t;
    });
  });
}

function doTranscribe(job) {
  var lang = job.opts.lang || store.getSettings().lastLang || 'en';
  return transcribeInto(job, lang, 100).then(function (t) {
    return M(t.text ? 'Transcribed' : 'No speech found');
  });
}

// Translates a recording: a chat model translates the transcript (made first if needed), keeping
// its times; Whisper translates the audio into English.
function doTranslate(job) {
  var s = store.getSettings();
  var mode = services.translationMode(s);
  var target = mode === 'en' ? 'en' : (job.opts.lang || s.lastTranslateLang || 'en');
  var done = function (segments, estimated, source) {
    ensurePhoneMeta(job.id);
    store.saveTranslation(job.id, {
      lang: target, source: source || '', segments: segments, estimated: !!estimated,
      text: segments.map(function (x) { return x.text; }).join(' '),
      provider: s.translation.provider, at: Date.now()
    });
    sendPhoneItem(store.get(job.id));
    sendStorage();
    return segments.length ? M('Translated into {lang}', { lang: i18n.language(target) }) : M('No speech found');
  };
  if (mode === 'en') {
    return getAudio(job.id, job).then(function (a) {
      var pcm = codec.decodeBlocks(a.bytes, a.samples);
      return services.translateWhisper(pcm, codec.rateFor(a.rate), s.transcription).then(function (r) {
        return done(r.segments, r.estimated, '');
      });
    });
  }
  if (mode !== 'any') return Promise.reject(E('Set up translation in the phone settings'));
  var existing = store.getTranscript(job.id);
  var transcript;
  if (existing) {
    transcript = Promise.resolve(existing);
  } else if (services.transcriptionReady(s.transcription)) {
    transcript = transcribeInto(job, s.lastLang || 'en', 50);
  } else {
    return Promise.reject(E('Transcribe it first: set up transcription in the phone settings'));
  }
  return transcript.then(function (t) {
    var segs = t.segments && t.segments.length ? t.segments : (t.text ? [{ start: 0, end: 0, text: t.text }] : []);
    if (!segs.length) return done([], t.estimated, t.lang);
    var base = job.pct || 0;
    return services.translateSegments(segs, langName(target), t.lang ? langName(t.lang) : '', s.translation.chat,
      function (pct) { progress(job, base + (100 - base) * pct / 100); })
      .then(function (out) { return done(out, t.estimated, t.lang); });
  });
}

function pad2(n) { return (n < 10 ? '0' : '') + n; }

function stamp(created) {
  var d = new Date(created * 1000);
  return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' ' +
         pad2(d.getHours()) + '.' + pad2(d.getMinutes()) + '.' + pad2(d.getSeconds());
}

function fileBase(x) {
  var name = String(x.name || '').replace(/[\\/:*?"<>|\u0000-\u001f]/g, '').trim();
  return name ? name + ' (' + stamp(x.created) + ')' : 'Recording ' + stamp(x.created);
}

function clock(ms) {
  var s = Math.floor(ms / 1000);
  var h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60;
  return (h ? h + ':' + pad2(m) : m) + ':' + pad2(s % 60);
}

function transcriptText(x, t) {
  var ui = pageLang || watch.lang;
  var lines = [fileBase(x), i18n.t(ui, t.estimated ? 'Language: {lang} (times estimated)' : 'Language: {lang}',
                                   { lang: i18n.language(t.lang) }), ''];
  (t.segments || []).forEach(function (seg) { lines.push('[' + clock(seg.start) + '] ' + seg.text); });
  if (!(t.segments || []).length) lines.push(t.text || '');
  return lines.join('\n') + '\n';
}

function doUpload(job) {
  var s = store.getSettings();
  return getAudio(job.id, job).then(function (a) {
    var x = meta(job.id);
    var pcm = codec.normalize(codec.decodeBlocks(a.bytes, a.samples));
    var base = fileBase(x);
    var files = [{ name: base + '.wav', type: 'audio/wav', bytes: codec.wav(pcm, codec.rateFor(a.rate)) }];
    var t = store.getTranscript(job.id);
    if (t && s.webdav.uploadTranscript) {
      files.push({ name: base + '.txt', type: 'text/plain; charset=utf-8', bytes: codec.utf8(transcriptText(x, t)) });
    }
    var tl = store.getTranslation(job.id);
    if (tl && s.webdav.uploadTranscript) {
      files.push({ name: base + ' (' + String(tl.lang).toUpperCase() + ').txt', type: 'text/plain; charset=utf-8',
                   bytes: codec.utf8(transcriptText(x, tl)) });
    }
    progress(job, 50);
    return services.uploadWebDAV(s.webdav, files).then(function () {
      var m = ensurePhoneMeta(job.id);
      m.uploaded = true;
      store.save();
      sendPhoneItem(m);
      return M('Uploaded');
    });
  });
}

RUN.save = doSave;
RUN.transcribe = doTranscribe;
RUN.translate = doTranslate;
RUN.upload = doUpload;

function autoRules(id, source) {
  var s = store.getSettings();
  if (source === 'watch' && (s.autoSave === 'copy' || s.autoSave === 'move')) {
    enqueue('save', id, { move: s.autoSave === 'move' });
  }
  if (s.autoTranscribe && services.transcriptionReady(s.transcription)) {
    enqueue('transcribe', id, { lang: s.lastLang });
  }
  if (s.autoUpload && services.webdavReady(s.webdav)) enqueue('upload', id);
}

// ---- Fetching audio from the watch --------------------------------------------------------------

var pulls = {};

function pull(id, onProgress) {
  if (pulls[id]) {
    if (onProgress) pulls[id].listeners.push(onProgress);
    return pulls[id].promise;
  }
  var w = watch.list[id];
  var entry = { total: w ? w.blocks : 0, received: 0, chunks: [], listeners: onProgress ? [onProgress] : [] };
  entry.promise = new Promise(function (resolve, reject) {
    entry.resolve = resolve;
    entry.reject = reject;
  });
  pulls[id] = entry;
  armPull(id);
  send({ Cmd: P.P_PULL, Id: id }).catch(function (e) { failPull(id, e); });
  return entry.promise;
}

function armPull(id) {
  var e = pulls[id];
  if (!e) return;
  if (e.timer) clearTimeout(e.timer);
  e.timer = setTimeout(function () {
    failPull(id, E('The watch stopped sending the recording'));
  }, 25000);
}

function failPull(id, err) {
  var e = pulls[id];
  if (!e) return;
  clearTimeout(e.timer);
  delete pulls[id];
  e.reject(err);
}

function onPullChunk(id, at, data) {
  var e = pulls[id];
  if (!e) return;
  e.chunks.push({ at: at, data: data });
  e.received += data.length / codec.BLOCK_BYTES;
  if (e.total) e.listeners.forEach(function (fn) { fn(100 * e.received / e.total); });
  armPull(id);
}

function onPullEnd(id, blocks, samples, rate, created) {
  var e = pulls[id];
  if (!e) return;
  clearTimeout(e.timer);
  delete pulls[id];
  var bytes = new Uint8Array(blocks * codec.BLOCK_BYTES);
  e.chunks.forEach(function (c) {
    if (c.at * codec.BLOCK_BYTES + c.data.length <= bytes.length) bytes.set(c.data, c.at * codec.BLOCK_BYTES);
  });
  e.resolve({ bytes: bytes, samples: samples, rate: rate, created: created });
}

// ---- Recording straight to the phone ------------------------------------------------------------

var stream = null;

function newStream() {
  return { id: 0, rate: 0, gain: 256, inputRate: 16000, created: 0, writer: null, enc: new codec.AdpcmEncoder(),
           dec: new codec.Decimator(), encoded: 0, received: 0, paused: false, full: false,
           pending: [], stopTimer: null, statTimer: null };
}

function streamReceivedMs(s) { return Math.round(s.received * 1000 / s.inputRate); }

function startStats(s) {
  if (s.statTimer) return;
  s.statTimer = setInterval(function () {
    if (s.id && !s.paused) sendQuiet({ Cmd: P.P_STREAM_STAT, Id: s.id, A: streamReceivedMs(s) });
  }, 2000);
}

function stopTimers(s) {
  if (s.stopTimer) clearTimeout(s.stopTimer);
  if (s.statTimer) clearInterval(s.statTimer);
  s.stopTimer = s.statTimer = null;
}

function beginStream(id, rateCode, created, gainStep) {
  if (stream && stream.id === id) return;
  if (stream && stream.id) finishStream(stream, 0);
  var s = stream && !stream.id ? stream : newStream();
  s.id = id;
  s.rate = rateCode;
  s.gain = codec.gainQ8(gainStep);
  s.created = created;
  if (resolveConflict(id, created)) sendPhoneList();
  if (store.get(id)) store.remove(id);
  store.ensure(id, { created: created, rate: rateCode, samples: 0, open: true, name: '' });
  s.writer = new store.AudioWriter(id);
  stream = s;
  var pending = s.pending;
  s.pending = [];
  pending.forEach(function (pcm) { feed(s, pcm); });
  startStats(s);
}

function streamFull(s) {
  if (s.full) return;
  s.full = true;
  console.log('Phone storage full; stopping the recording');
  sendQuiet({ Cmd: P.P_CONTROL, A: P.CTL_STOP });
}

function writeBlock(s, block) {
  if (s.full) return;
  try {
    s.writer.add(block);
    if ((s.writer.blocks & 31) === 0 && store.usage().freeBytes < 4096) streamFull(s);
  } catch (e) {
    streamFull(s);
  }
}

function feed(s, pcm) {
  if (!s.id) {
    if (s.pending.length < 100) s.pending.push(pcm);  // until W_REC_BEGIN arrives (10 s)
    return;
  }
  s.received += pcm.length;
  if (s.full) return;
  var decimate = s.rate === 0 && s.inputRate === 16000;
  var gain = s.gain;
  for (var i = 0; i < pcm.length; i++) {
    var v = gain === 256 ? pcm[i] : codec.applyGain(pcm[i], gain);
    if (decimate) {
      v = s.dec.push(v);
      if (v === null) continue;
    }
    s.encoded++;
    var block = s.enc.push(v);
    if (block) writeBlock(s, block);
  }
}

function finishStream(s, elapsedMs) {
  stopTimers(s);
  if (stream === s) stream = null;
  if (!s.id) return;
  var f = s.enc.flush();
  if (f) writeBlock(s, f.block);
  try { s.writer.finish(); } catch (e) { /* storage full: keep what was written */ }
  var m = store.get(s.id);
  if (!m) return;
  m.open = false;
  m.samples = Math.min(s.encoded, (m.blocks || 0) * codec.BLOCK_SAMPLES);
  var lostMs = elapsedMs ? Math.max(0, elapsedMs - streamReceivedMs(s)) : 0;
  m.lost = lostMs > 300 ? Math.round(lostMs / 100) : 0;
  if (!m.samples) {
    store.remove(s.id);
    sendQuiet({ Cmd: P.P_PHONE_GONE, Id: s.id });
  } else {
    store.save();
    sendQuiet({ Cmd: P.P_REC_SAVED, Id: s.id, A: m.created, B: m.samples, C: phoneFlags(m) | (m.rate << 8),
                D: m.lost });
    autoRules(s.id, 'phone');
  }
  sendStorage();
  pushRecordings();
}

function discardStream(id) {
  if (stream && stream.id === id) {
    stopTimers(stream);
    stream = null;
  }
  if (store.get(id)) store.remove(id);
  sendStorage();
  pushRecordings();
}

Pebble.addEventListener('audiostreamstart', function (e) {
  if (!stream) stream = newStream();
  stream.inputRate = (e && e.sampleRate) || 16000;
  stream.paused = false;
  if (stream.stopTimer) clearTimeout(stream.stopTimer);
  stream.stopTimer = null;
});

Pebble.addEventListener('audiostream', function (e) {
  if (!stream || !e || !e.data) return;
  feed(stream, new Int16Array(e.data));
});

Pebble.addEventListener('audiostreamstop', function () {
  var s = stream;
  if (!s || s.paused) return;
  // A pause or stop message from the watch normally follows; if none does, the phone lost the
  // watch mid-recording, so keep what arrived.
  if (s.stopTimer) clearTimeout(s.stopTimer);
  s.stopTimer = setTimeout(function () { finishStream(s, 0); }, 5000);
});

// Recordings left open when the app was last closed mid-stream.
function recoverOpen() {
  store.all().forEach(function (m) {
    if (!m.open) return;
    m.open = false;
    m.samples = (m.blocks || 0) * codec.BLOCK_SAMPLES;
    if (!m.samples) store.remove(m.id);
  });
  store.save();
}

// ---- Serving the watch --------------------------------------------------------------------------

function servePlay(id, from, count) {
  var m = store.get(id);
  if (!m || !m.audio) {
    sendQuiet({ Cmd: P.P_PLAY_ERR, Id: id, Str: i18n.t(watch.lang, 'This recording isn\'t on the phone.') });
    return;
  }
  var per = (watch.info && watch.info.inboxBlocks) || 8;
  var bytes = store.readBlocks(id, from, count) || new Uint8Array(0);
  var blocks = bytes.length / codec.BLOCK_BYTES;
  for (var i = 0; i < blocks; i += per) {
    var n = Math.min(per, blocks - i);
    sendQuiet({ Cmd: P.P_PLAY_DATA, Id: id, A: from + i,
                Data: arrayOf(bytes.subarray(i * codec.BLOCK_BYTES, (i + n) * codec.BLOCK_BYTES)) });
  }
}

function serveTranscript(id, translation) {
  var t = translation ? store.getTranslation(id) : store.getTranscript(id);
  if (!t) {
    sendQuiet({ Cmd: P.P_TR_NONE, Id: id, Str: i18n.t(watch.lang, translation ? 'No translation yet.' : 'No transcript yet.') });
    return;
  }
  var segs = (t.segments && t.segments.length) ? t.segments : [{ start: 0, text: t.text || '' }];
  segs = segs.slice(0, 160);
  sendQuiet({ Cmd: P.P_TR_BEGIN, Id: id, A: segs.length, B: t.estimated ? 1 : 0 });
  segs.forEach(function (seg, i) {
    sendQuiet({ Cmd: P.P_TR_SEG, Id: id, A: i, B: Math.round(seg.start || 0), Str: String(seg.text || '').slice(0, 480) });
  });
  sendQuiet({ Cmd: P.P_TR_END, Id: id });
}

function deletePhoneCopy(id) {
  if (!store.get(id)) return;
  store.remove(id);
  sendQuiet({ Cmd: P.P_PHONE_GONE, Id: id });
  sendStorage();
}

function onAppMessage(e) {
  var p = e.payload || {};
  var id = p.Id || 0;
  switch (p.Cmd) {
    case P.W_HELLO:
      watch.connected = true;
      var hello = String(p.Str || '').split(' ');
      watch.lang = i18n.lang2(hello[1] || 'en');
      watch.info = { capacity: p.A >>> 0, used: p.B >>> 0, features: p.C || 0, inboxBlocks: p.D || 8,
                     version: hello[0] || '', lang: watch.lang };
      if (Date.now() - lastHello > 3000) sendHello();
      pushState();
      break;
    case P.W_LIST_BEGIN:
      watch.connected = true;
      watch.listing = {};
      break;
    case P.W_LIST_ITEM: {
      var item = parseItem(p);
      adoptWatchItem(item);
      if (watch.listing) watch.listing[item.id] = item;
      else watch.list[item.id] = item;
      break;
    }
    case P.W_LIST_END:
      if (watch.listing) watch.list = watch.listing;
      watch.listing = null;
      if (p.B) {
        watch.info = watch.info || { features: 0, inboxBlocks: 8, version: '' };
        watch.info.used = p.A >>> 0;
        watch.info.capacity = p.B >>> 0;
        watch.info.free = p.C >>> 0;
      }
      pushState();
      sendPhoneList();
      pushRecordings();
      break;
    case P.W_SAVED: {
      var saved = parseItem(p);
      watch.list[saved.id] = saved;
      adoptWatchItem(saved);
      autoRules(saved.id, 'watch');
      if (pageOpen) sendQuiet({ Cmd: P.P_LIST_REQ });
      pushRecordings();
      break;
    }
    case P.W_SETTINGS: {
      var langs = String(p.Str || '').split(' ');
      var cur = store.getSettings();
      store.updateSettings({ quality: p.A || 0, target: p.B || 0, quickLaunch: p.C ? 1 : 0,
                             gain: p.D != null && p.D >= 0 && p.D < P.GAIN_DB.length ? p.D : cur.gain,
                             lastLang: langs[0] || cur.lastLang, lastTranslateLang: langs[1] || cur.lastTranslateLang });
      push({ type: 'settings', settings: store.getSettings() });
      break;
    }
    case P.W_REC_STATE:
      watch.connected = true;
      watch.recorder = { state: p.A || 0, elapsed: p.B || 0, id: id, mode: p.C || 0,
                         health: (p.D || 0) & 0xff, reason: ((p.D || 0) >> 8) & 0xff, at: Date.now() };
      pushState();
      break;
    case P.W_REC_BEGIN:
      beginStream(id, p.A || 0, p.B >>> 0, p.C != null ? p.C : P.GAIN_DEFAULT);
      pushRecordings();
      break;
    case P.W_REC_PAUSE:
      if (stream && stream.id === id) {
        stream.paused = true;
        if (stream.stopTimer) clearTimeout(stream.stopTimer);
        stream.stopTimer = null;
      }
      break;
    case P.W_REC_RESUME:
      if (stream && stream.id === id) stream.paused = false;
      break;
    case P.W_REC_END:
      if (stream && stream.id === id) finishStream(stream, p.A || 0);
      break;
    case P.W_REC_DISCARD:
      discardStream(id);
      break;
    case P.W_AUDIO:
      onPullChunk(id, p.A || 0, bytesOf(p.Data));
      break;
    case P.W_AUDIO_END:
      onPullEnd(id, p.A || 0, p.B >>> 0, p.C || 0, p.D >>> 0);
      break;
    case P.W_PULL_ERR:
      failPull(id, E(p.A === 2 ? 'The watch ran out of memory' : 'The recording is no longer on the watch'));
      break;
    case P.W_ACTION:
      switch (p.A) {
        case P.ACT_TRANSCRIBE:
          if (p.Str) store.updateSettings({ lastLang: p.Str });
          enqueue('transcribe', id, { lang: p.Str || store.getSettings().lastLang });
          break;
        case P.ACT_TRANSLATE:
          if (p.Str && services.translationMode(store.getSettings()) === 'any') {
            store.updateSettings({ lastTranslateLang: p.Str });
          }
          enqueue('translate', id, { lang: p.Str || store.getSettings().lastTranslateLang });
          break;
        case P.ACT_UPLOAD: enqueue('upload', id); break;
        case P.ACT_SAVE: enqueue('save', id); break;
        case P.ACT_DELETE_PHONE:
        case P.ACT_DELETE_ALL:
          delete watch.list[id];
          deletePhoneCopy(id);
          pushRecordings();
          break;
      }
      break;
    case P.W_TR_REQ:
      serveTranscript(id, p.A === 1);
      break;
    case P.W_PLAY_REQ:
      servePlay(id, p.A || 0, p.B || 0);
      break;
    case P.W_DELETED: {
      var w = watch.list[id];
      if (w && !(w.flags & (P.REC_PHONE | P.REC_TRANSCRIPT | P.REC_TRANSLATION))) delete watch.list[id];
      else if (w) { w.flags &= ~P.REC_LOCAL; w.blocks = 0; }
      var m = store.get(id);
      if (m && !m.audio && !m.transcript && !m.translation) store.remove(id);
      if (pageOpen) sendQuiet({ Cmd: P.P_LIST_REQ });
      pushRecordings();
      break;
    }
  }
}

// ---- Settings page (live bridge) ----------------------------------------------------------------

var pushTimer = null;

function push(msg) {
  if (!pageOpen || typeof Pebble.sendConfigMessage !== 'function') return;
  try { Pebble.sendConfigMessage(msg); } catch (e) { /* page gone */ }
}

function pushState() {
  if (!pageOpen || pushTimer) return;
  pushTimer = setTimeout(function () {
    pushTimer = null;
    push({ type: 'state', state: stateView() });
  }, 250);
}

function pushRecordings() {
  push({ type: 'recordings', recordings: recordingsView(), usage: store.usage() });
}

function stateView() {
  return {
    watch: { connected: watch.connected, info: watch.info },
    recorder: watch.recorder,
    usage: store.usage(),
    caps: caps()
  };
}

function recordingsView() {
  var map = {};
  Object.keys(watch.list).forEach(function (k) {
    var w = watch.list[k];
    map[w.id] = {
      id: w.id, created: w.created, samples: w.samples, rate: w.rate, name: w.name, lost: w.lost,
      onWatch: !!(w.flags & P.REC_LOCAL), open: !!(w.flags & P.REC_OPEN), watchBytes: w.blocks * codec.BLOCK_BYTES,
      onPhone: false, phoneBytes: 0, transcript: !!(w.flags & P.REC_TRANSCRIPT), uploaded: !!(w.flags & P.REC_UPLOADED),
      translation: !!(w.flags & P.REC_TRANSLATION)
    };
  });
  store.all().forEach(function (m) {
    var r = map[m.id] || (map[m.id] = { id: m.id, created: m.created, samples: m.samples, rate: m.rate,
                                         name: '', lost: 0, onWatch: false, watchBytes: 0 });
    r.onPhone = !!m.audio;
    r.phoneBytes = (m.blocks || 0) * codec.BLOCK_BYTES;
    r.transcript = !!m.transcript;
    r.translation = !!m.translation;
    if (m.translation) r.xlang = m.xlang;
    r.uploaded = !!m.uploaded;
    r.open = r.open || !!m.open;
    if (m.name) r.name = m.name;
    if (m.audio) { r.samples = m.samples; r.rate = m.rate; }
    if (m.lost) r.lost = m.lost;
    if (m.transcript) {
      var t = store.getTranscript(m.id);
      if (t) r.lang = t.lang;
    }
  });
  if (current) {
    if (map[current.id]) map[current.id].job = jobView(current);
  }
  jobs.forEach(function (j) { if (map[j.id] && !map[j.id].job) map[j.id].job = jobView(j); });
  return Object.keys(map).map(function (k) { return map[k]; })
    .filter(function (r) { return r.onWatch || r.onPhone || r.transcript || r.translation || r.open; })
    .sort(function (a, b) { return a.created - b.created || a.id - b.id; });
}

function snapshot() {
  return {
    ok: true,
    settings: store.getSettings(),
    state: stateView(),
    recordings: recordingsView(),
    languages: LANGS
  };
}

function cleanSettings(next) {
  var s = store.saveSettings(next || {});
  sendHello();
  pushState();
  return s;
}

var CONTROL = { start: P.CTL_START, pause: P.CTL_PAUSE, resume: P.CTL_RESUME, stop: P.CTL_STOP, discard: P.CTL_DISCARD };

function handlePage(msg) {
  switch (msg.type) {
    case 'init':
      pageOpen = true;
      if (msg.lang) pageLang = i18n.lang2(msg.lang);
      if (watch.connected) sendQuiet({ Cmd: P.P_LIST_REQ });
      return Promise.resolve(snapshot());

    case 'settings':
      return Promise.resolve({ ok: true, settings: cleanSettings(msg.settings) });

    case 'control':
      if (!CONTROL[msg.action]) return Promise.reject(new Error('Unknown action'));
      return send({ Cmd: P.P_CONTROL, A: CONTROL[msg.action] }).then(function () { return { ok: true }; }, function () {
        throw E('Open Voice Recorder on the watch first');
      });

    case 'rename': {
      var name = String(msg.name || '').trim().slice(0, 23);
      var m = store.get(msg.id);
      if (m) { m.name = name; store.save(); sendPhoneItem(m); }
      var w = watch.list[msg.id];
      if (w) { w.name = name; sendQuiet({ Cmd: P.P_RENAME, Id: msg.id, Str: name }); }
      pushRecordings();
      return Promise.resolve({ ok: true });
    }

    case 'delete': {
      var id = msg.id;
      if (msg.where === 'watch' || msg.where === 'all') sendQuiet({ Cmd: P.P_DELETE, Id: id });
      if (msg.where === 'phone' || msg.where === 'all') deletePhoneCopy(id);
      if (msg.where === 'all') delete watch.list[id];
      pushRecordings();
      return Promise.resolve({ ok: true });
    }

    case 'save':
      enqueue('save', msg.id);
      return Promise.resolve({ ok: true });

    case 'transcribe':
      if (msg.lang) {
        store.updateSettings({ lastLang: msg.lang });
        sendHello();
      }
      enqueue('transcribe', msg.id, { lang: msg.lang || store.getSettings().lastLang });
      return Promise.resolve({ ok: true });

    case 'translate':
      if (msg.lang) {
        store.updateSettings({ lastTranslateLang: msg.lang });
        sendHello();
      }
      enqueue('translate', msg.id, { lang: msg.lang || store.getSettings().lastTranslateLang });
      return Promise.resolve({ ok: true });

    case 'translation':
      return Promise.resolve({ ok: true, translation: store.getTranslation(msg.id) });

    case 'upload':
      enqueue('upload', msg.id);
      return Promise.resolve({ ok: true });

    case 'audio': {
      var pm = store.get(msg.id);
      if (pm && pm.audio) {
        return Promise.resolve({ ok: true, rate: pm.rate, samples: pm.samples, b64: codec.toBase64(store.readAudio(msg.id)) });
      }
      // Fetch from the watch, answer later with a push (the bridge times out long requests).
      pull(msg.id, function (pct) { push({ type: 'audioProgress', id: msg.id, pct: Math.round(pct) }); }).then(function (a) {
        push({ type: 'audio', id: msg.id, ok: true, rate: a.rate, samples: a.samples, b64: codec.toBase64(a.bytes) });
      }, function (err) {
        push({ type: 'audio', id: msg.id, ok: false, error: i18n.text(pageLang, err) });
      });
      return Promise.resolve({ ok: true, pending: true });
    }

    case 'transcript':
      return Promise.resolve({ ok: true, transcript: store.getTranscript(msg.id) });

    case 'test': {
      var cfg = msg.config || {};
      var p = msg.what === 'openai' ? services.testOpenAI(cfg)
            : msg.what === 'ha' ? services.testHA(cfg)
            : msg.what === 'chat' ? services.testChat(cfg)
            : msg.what === 'webdav' ? services.testWebDAV(cfg)
            : Promise.reject(new Error('Nothing to test'));
      return p.then(function (r) {
        return r.template ? { ok: true, message: i18n.text(pageLang, r) }
                          : { ok: true, message: i18n.text(pageLang, r.message), languages: r.languages };
      });
    }

    case 'refresh':
      if (watch.connected) sendQuiet({ Cmd: P.P_LIST_REQ });
      return Promise.resolve(snapshot());
  }
  return Promise.reject(new Error('Unknown request ' + msg.type));
}

Pebble.addEventListener('configmessage', function (e) {
  var msg = e.data || {};
  handlePage(msg).then(function (r) {
    e.respond(r || { ok: true });
  }, function (err) {
    e.respond({ ok: false, error: err ? i18n.text(pageLang, err) : i18n.t(pageLang, 'Something went wrong') });
  });
});

// The classic settings flow. The Pebble app opens the bundled page (configPage) itself when its
// plugins / permissions preview is on; otherwise it fires this, and the same page opens as a
// data: URL with the settings in its fragment. It can't talk to this script while it's open, so it
// edits settings only and returns them when closed.
// Keeps only one language's page translations, so the data: URL stays small (about half).
function pageForLanguage(page, lang) {
  return page.replace(/(\/\/ I18N-BEGIN\nwindow\.I18N = )([\s\S]*?)(;\n\/\/ I18N-END)/, function (all, head, json, tail) {
    try {
      var all18n = JSON.parse(json), one = {};
      if (all18n[lang]) one[lang] = all18n[lang];
      return head + JSON.stringify(one) + tail;
    } catch (e) {
      return all;
    }
  });
}

Pebble.addEventListener('showConfiguration', function () {
  var phoneLang = typeof navigator !== 'undefined' && navigator.language ? navigator.language : watch.lang;
  var page = pageForLanguage(require('./config_page'), i18n.lang2(phoneLang));
  var data = { classic: true, settings: store.getSettings(), state: stateView(), languages: LANGS };
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(page) + '#' +
                 encodeURIComponent(JSON.stringify(data)));
});

Pebble.addEventListener('webviewclosed', function (e) {
  pageOpen = false;
  // The classic flow (and phone apps without the live bridge): the page returns its settings on close.
  if (e && e.response) {
    // The Pebble app percent-decodes the data once already; decoding it again would mangle any
    // "%" in a password or key. Older apps passed it still encoded, so fall back to that.
    var data = null;
    try { data = JSON.parse(e.response); } catch (err) {
      try { data = JSON.parse(decodeURIComponent(e.response)); } catch (err2) { data = null; }
    }
    if (data && data.settings) cleanSettings(data.settings);
  }
});

// ---- Start --------------------------------------------------------------------------------------

Pebble.addEventListener('appmessage', onAppMessage);

Pebble.addEventListener('ready', function () {
  recoverOpen();
  sendHello();
});
