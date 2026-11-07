// Phone-side storage in the Pebble app's localStorage: settings, the recordings index, audio
// (IMA ADPCM blocks, base64, in chunks) and transcripts.

var codec = require('./codec');

var SETTINGS_KEY = 'vr.settings';
var INDEX_KEY = 'vr.index';
var CHUNK_BLOCKS = 32;  // 8 KB of audio per key

var DEFAULTS = {
  quality: 0,            // 0 standard (8 kHz), 1 high (16 kHz)
  target: 0,             // 0 watch, 1 phone
  quickLaunch: 1,        // start recording when opened with Quick Launch
  lastLang: 'en',
  autoSave: 'off',       // 'off' | 'copy' | 'move'
  autoTranscribe: false,
  autoUpload: false,
  phoneBudgetMB: 5,
  transcription: {
    provider: 'none',    // 'none' | 'openai' | 'ha'
    openai: { baseUrl: 'https://api.openai.com/v1', apiKey: '', model: 'whisper-1' },
    ha: { url: '', token: '', entity: 'stt.faster_whisper', languages: null }
  },
  webdav: { url: '', username: '', password: '', folder: 'Voice Recorder', uploadTranscript: true }
};

function clone(o) { return JSON.parse(JSON.stringify(o)); }

function merge(base, extra) {
  var out = clone(base);
  if (!extra || typeof extra !== 'object') return out;
  Object.keys(extra).forEach(function (k) {
    if (out[k] && typeof out[k] === 'object' && !Array.isArray(out[k]) && extra[k] && typeof extra[k] === 'object') {
      out[k] = merge(out[k], extra[k]);
    } else if (extra[k] !== undefined) {
      out[k] = extra[k];
    }
  });
  return out;
}

function readJSON(key, fallback) {
  try {
    var raw = localStorage.getItem(key);
    return raw ? JSON.parse(raw) : fallback;
  } catch (e) {
    return fallback;
  }
}

var settings = merge(DEFAULTS, readJSON(SETTINGS_KEY, {}));
var index = readJSON(INDEX_KEY, {});  // id -> meta

function saveIndex() {
  localStorage.setItem(INDEX_KEY, JSON.stringify(index));
}

function audioKey(id, n) { return 'vr.a.' + id + '.' + n; }
function transcriptKey(id) { return 'vr.t.' + id; }

function isQuota(e) {
  return e && (e.name === 'QuotaExceededError' || e.code === 22 || /quota|full|exceed/i.test(String(e.message || e)));
}

// ---- Settings -----------------------------------------------------------------------------------

function getSettings() { return settings; }

function saveSettings(next) {
  settings = merge(DEFAULTS, next);
  localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  return settings;
}

function updateSettings(patch) {
  return saveSettings(merge(settings, patch));
}

// ---- Usage --------------------------------------------------------------------------------------

function usedChars() {
  var n = 0;
  Object.keys(index).forEach(function (id) { n += (index[id].chars || 0) + (index[id].tchars || 0); });
  return n;
}

function budgetChars() {
  return Math.max(1, Number(settings.phoneBudgetMB) || DEFAULTS.phoneBudgetMB) * 1024 * 1024;
}

// What the watch shows: ADPCM bytes still storable (base64 costs 4 chars per 3 bytes).
function usage() {
  var used = usedChars();
  var budget = budgetChars();
  var free = Math.max(0, budget - used);
  return { usedChars: used, budgetChars: budget, freeBytes: Math.floor(free * 3 / 4), usedBytes: Math.floor(used * 3 / 4) };
}

// ---- Recordings ---------------------------------------------------------------------------------

function get(id) { return index[id] || null; }

function all() {
  return Object.keys(index).map(function (k) { return index[k]; })
    .sort(function (a, b) { return a.created - b.created || a.id - b.id; });
}

function put(meta) {
  index[meta.id] = meta;
  saveIndex();
  return meta;
}

function ensure(id, fields) {
  var m = index[id] || { id: id, created: Math.floor(Date.now() / 1000), samples: 0, rate: 0, name: '',
                         audio: false, transcript: false, uploaded: false, blocks: 0, chars: 0, tchars: 0, lost: 0 };
  Object.keys(fields || {}).forEach(function (k) { m[k] = fields[k]; });
  index[id] = m;
  return m;
}

function maxId() {
  var max = 0;
  Object.keys(index).forEach(function (k) { if (+k < 0x8000 && +k > max) max = +k; });
  return max;
}

// Moves a recording to another id (used when a reinstalled watch reuses an id).
function rekey(oldId, newId) {
  var m = index[oldId];
  if (!m || index[newId]) return false;
  var chunks = Math.ceil((m.blocks || 0) / CHUNK_BLOCKS) + 1;
  for (var n = 0; n < chunks; n++) {
    var v = localStorage.getItem(audioKey(oldId, n));
    if (v !== null) {
      localStorage.setItem(audioKey(newId, n), v);
      localStorage.removeItem(audioKey(oldId, n));
    }
  }
  var t = localStorage.getItem(transcriptKey(oldId));
  if (t !== null) {
    localStorage.setItem(transcriptKey(newId), t);
    localStorage.removeItem(transcriptKey(oldId));
  }
  m.id = newId;
  index[newId] = m;
  delete index[oldId];
  saveIndex();
  return true;
}

// Ids from 0x8000 up are never handed out by the watch.
function freePhoneId() {
  var id = 0x8000;
  while (index[id]) id++;
  return id;
}

function deleteAudio(id) {
  var m = index[id];
  if (!m) return;
  var chunks = Math.ceil((m.blocks || 0) / CHUNK_BLOCKS) + 1;
  for (var n = 0; n < chunks; n++) localStorage.removeItem(audioKey(id, n));
  m.audio = false;
  m.blocks = 0;
  m.chars = 0;
  saveIndex();
}

function remove(id) {
  deleteAudio(id);
  localStorage.removeItem(transcriptKey(id));
  delete index[id];
  saveIndex();
}

// Writes ADPCM blocks as they arrive. Full chunks are written once; the open chunk is rewritten
// every few blocks so little is lost if the app is closed mid-recording.
function AudioWriter(id) {
  this.id = id;
  this.blocks = 0;
  this.pending = [];
  this.sinceSave = 0;
  this.chunkChars = {};
  var m = ensure(id, {});
  m.blocks = 0;
  m.chars = 0;
  m.audio = true;
}

AudioWriter.prototype._writeChunk = function (n, blocks) {
  var b64 = codec.toBase64(codec.concat(blocks));
  localStorage.setItem(audioKey(this.id, n), b64);  // may throw on quota
  var m = index[this.id];
  m.chars = (m.chars || 0) - (this.chunkChars[n] || 0) + b64.length;
  this.chunkChars[n] = b64.length;
};

AudioWriter.prototype.add = function (block) {
  this.pending.push(block);
  this.blocks++;
  var m = index[this.id];
  m.blocks = this.blocks;
  var chunk = Math.floor((this.blocks - 1) / CHUNK_BLOCKS);
  if (this.pending.length === CHUNK_BLOCKS) {
    this._writeChunk(chunk, this.pending);
    this.pending = [];
    this.sinceSave = 0;
    saveIndex();
  } else if (++this.sinceSave >= 8) {
    this._writeChunk(chunk, this.pending);
    this.sinceSave = 0;
    saveIndex();
  }
};

AudioWriter.prototype.finish = function () {
  if (this.pending.length) {
    this._writeChunk(Math.floor((this.blocks - 1) / CHUNK_BLOCKS), this.pending);
    this.pending = [];
  }
  saveIndex();
};

// Stores a whole recording at once. Throws {quota: true} when it doesn't fit.
function writeAll(id, bytes, fields) {
  var blocks = Math.floor(bytes.length / codec.BLOCK_BYTES);
  var estimate = Math.ceil(blocks * codec.BLOCK_BYTES / 3) * 4;
  var current = index[id] ? (index[id].chars || 0) : 0;
  if (usedChars() - current + estimate > budgetChars()) {
    var err = new Error('Phone storage is full');
    err.quota = true;
    throw err;
  }
  deleteAudio(id);
  var m = ensure(id, fields);
  var w = new AudioWriter(id);
  try {
    for (var b = 0; b < blocks; b++) {
      w.add(bytes.subarray(b * codec.BLOCK_BYTES, (b + 1) * codec.BLOCK_BYTES));
    }
    w.finish();
  } catch (e) {
    deleteAudio(id);
    if (isQuota(e)) e.quota = true;
    throw e;
  }
  m.audio = true;
  saveIndex();
  return m;
}

function readBlocks(id, from, count) {
  var m = index[id];
  if (!m || !m.audio) return null;
  var end = Math.min(m.blocks, from + count);
  var parts = [];
  var cached = {};
  for (var b = from; b < end; b++) {
    var n = Math.floor(b / CHUNK_BLOCKS);
    if (!cached[n]) {
      var raw = localStorage.getItem(audioKey(id, n));
      cached[n] = raw ? codec.fromBase64(raw) : new Uint8Array(0);
    }
    var off = (b % CHUNK_BLOCKS) * codec.BLOCK_BYTES;
    var block = cached[n].subarray(off, off + codec.BLOCK_BYTES);
    if (block.length < codec.BLOCK_BYTES) block = new Uint8Array(codec.BLOCK_BYTES);
    parts.push(block);
  }
  return codec.concat(parts);
}

function readAudio(id) {
  var m = index[id];
  return m && m.audio ? readBlocks(id, 0, m.blocks) : null;
}

// ---- Transcripts --------------------------------------------------------------------------------

function saveTranscript(id, t) {
  var json = JSON.stringify(t);
  localStorage.setItem(transcriptKey(id), json);
  var m = ensure(id, {});
  m.transcript = true;
  m.tchars = json.length;
  saveIndex();
}

function getTranscript(id) {
  return readJSON(transcriptKey(id), null);
}

function deleteTranscript(id) {
  localStorage.removeItem(transcriptKey(id));
  var m = index[id];
  if (m) {
    m.transcript = false;
    m.tchars = 0;
    saveIndex();
  }
}

module.exports = {
  DEFAULTS: DEFAULTS,
  getSettings: getSettings,
  saveSettings: saveSettings,
  updateSettings: updateSettings,
  usage: usage,
  get: get,
  all: all,
  put: put,
  ensure: ensure,
  save: saveIndex,
  maxId: maxId,
  remove: remove,
  rekey: rekey,
  freePhoneId: freePhoneId,
  deleteAudio: deleteAudio,
  AudioWriter: AudioWriter,
  writeAll: writeAll,
  readBlocks: readBlocks,
  readAudio: readAudio,
  saveTranscript: saveTranscript,
  getTranscript: getTranscript,
  deleteTranscript: deleteTranscript,
  isQuota: isQuota
};
