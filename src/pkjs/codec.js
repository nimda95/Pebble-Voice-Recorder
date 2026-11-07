// Audio and byte helpers. PebbleKit JS has no Blob, FormData or TextEncoder, so everything here
// works on plain typed arrays.

var BLOCK_BYTES = 256;
var BLOCK_SAMPLES = 505;

var STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
  449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
  9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
];
var INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

function clamp16(v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
function clampIndex(i) { return i < 0 ? 0 : (i > 88 ? 88 : i); }

// ---- IMA ADPCM (WAV block layout, mono, 256-byte blocks) -----------------------------------------

function decodeBlocks(bytes, totalSamples) {
  var blocks = Math.floor(bytes.length / BLOCK_BYTES);
  var count = blocks * BLOCK_SAMPLES;
  if (typeof totalSamples === 'number' && totalSamples < count) count = totalSamples;
  var out = new Int16Array(count);
  var o = 0;
  for (var b = 0; b < blocks && o < count; b++) {
    var base = b * BLOCK_BYTES;
    var pred = (bytes[base] | (bytes[base + 1] << 8));
    if (pred & 0x8000) pred -= 0x10000;
    var index = clampIndex(bytes[base + 2]);
    out[o++] = pred;
    for (var k = 0; k < BLOCK_SAMPLES - 1 && o < count; k++) {
      var byte = bytes[base + 4 + (k >> 1)];
      var nib = (k & 1) ? (byte >> 4) : (byte & 0x0f);
      var step = STEP_TABLE[index];
      var delta = step >> 3;
      if (nib & 4) delta += step;
      if (nib & 2) delta += step >> 1;
      if (nib & 1) delta += step >> 2;
      pred = clamp16((nib & 8) ? pred - delta : pred + delta);
      index = clampIndex(index + INDEX_TABLE[nib]);
      out[o++] = pred;
    }
  }
  return out;
}

function AdpcmEncoder() {
  this.predictor = 0;
  this.index = 0;
  this.pos = 0;
  this.block = new Uint8Array(BLOCK_BYTES);
  this.samples = 0;  // real samples encoded, for the saved length
}

AdpcmEncoder.prototype._encode = function (sample) {
  var step = STEP_TABLE[this.index];
  var diff = sample - this.predictor;
  var nib = 0;
  if (diff < 0) { nib = 8; diff = -diff; }
  var delta = step >> 3;
  if (diff >= step) { nib |= 4; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { nib |= 2; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { nib |= 1; delta += step; }
  this.predictor = clamp16((nib & 8) ? this.predictor - delta : this.predictor + delta);
  this.index = clampIndex(this.index + INDEX_TABLE[nib]);
  return nib;
};

// Returns a finished block (a copy) or null.
AdpcmEncoder.prototype.push = function (sample) {
  var b = this.block;
  if (this.pos === 0) {
    this.predictor = sample;
    b[0] = sample & 0xff;
    b[1] = (sample >> 8) & 0xff;
    b[2] = this.index;
    b[3] = 0;
    this.pos = 1;
    return null;
  }
  var nib = this._encode(sample);
  var k = this.pos - 1;
  var at = 4 + (k >> 1);
  if (k & 1) b[at] |= nib << 4; else b[at] = nib;
  if (++this.pos === BLOCK_SAMPLES) {
    this.pos = 0;
    return new Uint8Array(b);
  }
  return null;
};

// Completes the partial block; returns {block, real} or null.
AdpcmEncoder.prototype.flush = function () {
  if (this.pos === 0) return null;
  var real = this.pos;
  var hold = this.predictor;
  var done = null;
  while (!done) done = this.push(hold);
  return { block: done, real: real };
};

// 16 kHz -> 8 kHz, the same half-band filter the watch uses.
function Decimator() {
  this.x = [0, 0, 0, 0, 0, 0, 0];
  this.phase = 0;
}

Decimator.prototype.push = function (v) {
  var x = this.x;
  x.shift();
  x.push(v);
  this.phase ^= 1;
  if (this.phase) return null;
  return clamp16((-x[0] + 9 * x[2] + 16 * x[3] + 9 * x[4] - x[6]) >> 5);
};

// Linear upsampling by an integer factor (8 kHz -> 16 kHz for services that only take 16 kHz).
function upsample(pcm, factor) {
  if (factor === 1) return pcm;
  var out = new Int16Array(pcm.length * factor);
  for (var i = 0; i < pcm.length; i++) {
    var a = pcm[i];
    var b = i + 1 < pcm.length ? pcm[i + 1] : a;
    for (var k = 0; k < factor; k++) out[i * factor + k] = a + ((b - a) * k) / factor;
  }
  return out;
}

// Scales quiet recordings up (the watch mic is far from the mouth). Peak to ~-1 dBFS, at most 8x.
function normalize(pcm) {
  var peak = 1;
  for (var i = 0; i < pcm.length; i++) {
    var v = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (v > peak) peak = v;
  }
  var gain = Math.min(8, 29000 / peak);
  if (gain <= 1.05) return pcm;
  var out = new Int16Array(pcm.length);
  for (var j = 0; j < pcm.length; j++) out[j] = clamp16(Math.round(pcm[j] * gain));
  return out;
}

// ---- WAV ----------------------------------------------------------------------------------------

function pcmBytes(pcm) {
  var out = new Uint8Array(pcm.length * 2);
  for (var i = 0; i < pcm.length; i++) {
    out[2 * i] = pcm[i] & 0xff;
    out[2 * i + 1] = (pcm[i] >> 8) & 0xff;
  }
  return out;
}

function wav(pcm, rate) {
  var data = pcm.length * 2;
  var out = new Uint8Array(44 + data);
  var p = 0;
  function str(s) { for (var i = 0; i < s.length; i++) out[p++] = s.charCodeAt(i); }
  function u32(v) { out[p++] = v & 0xff; out[p++] = (v >> 8) & 0xff; out[p++] = (v >> 16) & 0xff; out[p++] = (v >>> 24) & 0xff; }
  function u16(v) { out[p++] = v & 0xff; out[p++] = (v >> 8) & 0xff; }
  str('RIFF'); u32(36 + data); str('WAVE');
  str('fmt '); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  str('data'); u32(data);
  out.set(pcmBytes(pcm), 44);
  return out;
}

// ---- Bytes --------------------------------------------------------------------------------------

var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
var B64_LOOKUP = (function () {
  var t = new Uint8Array(128);
  for (var i = 0; i < B64.length; i++) t[B64.charCodeAt(i)] = i;
  return t;
})();

function toBase64(bytes) {
  var parts = [];
  var chunk = [];
  for (var i = 0; i < bytes.length; i += 3) {
    var a = bytes[i], b = bytes[i + 1], c = bytes[i + 2];
    var n = (a << 16) | ((b || 0) << 8) | (c || 0);
    chunk.push(B64[(n >> 18) & 63], B64[(n >> 12) & 63],
               i + 1 < bytes.length ? B64[(n >> 6) & 63] : '=',
               i + 2 < bytes.length ? B64[n & 63] : '=');
    if (chunk.length >= 8192) {
      parts.push(chunk.join(''));
      chunk = [];
    }
  }
  parts.push(chunk.join(''));
  return parts.join('');
}

function fromBase64(str) {
  var clean = str.replace(/[^A-Za-z0-9+/]/g, '');
  var len = Math.floor(clean.length * 3 / 4);
  var out = new Uint8Array(len);
  var o = 0;
  for (var i = 0; i < clean.length; i += 4) {
    var n = (B64_LOOKUP[clean.charCodeAt(i)] << 18) | (B64_LOOKUP[clean.charCodeAt(i + 1)] << 12) |
            ((B64_LOOKUP[clean.charCodeAt(i + 2)] || 0) << 6) | (B64_LOOKUP[clean.charCodeAt(i + 3)] || 0);
    if (o < len) out[o++] = (n >> 16) & 0xff;
    if (o < len) out[o++] = (n >> 8) & 0xff;
    if (o < len) out[o++] = n & 0xff;
  }
  return out;
}

function utf8(str) {
  var out = [];
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      var d = str.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) {
        c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
        i++;
      }
    }
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
    else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
  }
  return new Uint8Array(out);
}

function fromUtf8(bytes) {
  var s = '';
  for (var i = 0; i < bytes.length;) {
    var c = bytes[i++];
    if (c >= 0xf0) c = ((c & 7) << 18) | ((bytes[i++] & 63) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
    else if (c >= 0xe0) c = ((c & 15) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
    else if (c >= 0xc0) c = ((c & 31) << 6) | (bytes[i++] & 63);
    s += String.fromCharCode.apply(null, c >= 0x10000
      ? [0xd800 + ((c - 0x10000) >> 10), 0xdc00 + ((c - 0x10000) & 1023)] : [c]);
  }
  return s;
}

function concat(arrays) {
  var len = 0;
  for (var i = 0; i < arrays.length; i++) len += arrays[i].length;
  var out = new Uint8Array(len);
  var p = 0;
  for (var j = 0; j < arrays.length; j++) {
    out.set(arrays[j], p);
    p += arrays[j].length;
  }
  return out;
}

module.exports = {
  BLOCK_BYTES: BLOCK_BYTES,
  BLOCK_SAMPLES: BLOCK_SAMPLES,
  decodeBlocks: decodeBlocks,
  AdpcmEncoder: AdpcmEncoder,
  Decimator: Decimator,
  upsample: upsample,
  normalize: normalize,
  pcmBytes: pcmBytes,
  wav: wav,
  toBase64: toBase64,
  fromBase64: fromBase64,
  utf8: utf8,
  fromUtf8: fromUtf8,
  concat: concat,
  rateFor: function (code) { return code ? 16000 : 8000; }
};
