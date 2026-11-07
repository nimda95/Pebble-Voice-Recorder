// Transcription (OpenAI-compatible endpoint, or Wyoming faster-whisper through Home Assistant)
// and WebDAV uploads.

var codec = require('./codec');

// ---- HTTP ---------------------------------------------------------------------------------------

function request(opts) {
  return new Promise(function (resolve, reject) {
    var xhr = new XMLHttpRequest();
    var done = false;
    function finish(fn, value) {
      if (done) return;
      done = true;
      fn(value);
    }
    try {
      xhr.open(opts.method || 'GET', opts.url, true);
    } catch (e) {
      reject(new Error('Bad URL: ' + opts.url));
      return;
    }
    Object.keys(opts.headers || {}).forEach(function (h) { xhr.setRequestHeader(h, opts.headers[h]); });
    xhr.timeout = opts.timeout || 60000;
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      if (xhr.status === 0) {
        finish(reject, new Error("Couldn't reach " + host(opts.url)));
        return;
      }
      var json = null;
      try { json = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
      finish(resolve, { status: xhr.status, text: xhr.responseText || '', json: json });
    };
    xhr.ontimeout = function () { finish(reject, new Error(host(opts.url) + ' took too long to answer')); };
    xhr.onerror = function () { finish(reject, new Error("Couldn't reach " + host(opts.url))); };
    xhr.send(opts.body === undefined ? null : opts.body);
  });
}

function host(url) {
  var m = /^[a-z]+:\/\/([^/?#]+)/i.exec(url || '');
  return m ? m[1] : 'the server';
}

function trimSlash(s) { return String(s || '').trim().replace(/\/+$/, ''); }

function errorText(res, fallback) {
  var j = res.json;
  var msg = j && (j.error && (j.error.message || j.error) || j.message || j.detail);
  if (typeof msg === 'object') msg = JSON.stringify(msg);
  return (msg ? String(msg) : fallback || ('HTTP ' + res.status)).slice(0, 160);
}

function multipart(fields, file) {
  var boundary = '----pebblevr' + Date.now().toString(16) + Math.floor(Math.random() * 1e9).toString(16);
  var parts = [];
  fields.forEach(function (f) {
    parts.push(codec.utf8('--' + boundary + '\r\nContent-Disposition: form-data; name="' + f[0] +
                          '"\r\n\r\n' + f[1] + '\r\n'));
  });
  parts.push(codec.utf8('--' + boundary + '\r\nContent-Disposition: form-data; name="file"; filename="' +
                        file.name + '"\r\nContent-Type: ' + file.type + '\r\n\r\n'));
  parts.push(file.bytes);
  parts.push(codec.utf8('\r\n--' + boundary + '--\r\n'));
  return { body: codec.concat(parts), type: 'multipart/form-data; boundary=' + boundary };
}

// ---- Segments -----------------------------------------------------------------------------------

// Spreads text over the duration by length, sentence by sentence, when the service gave no times.
function estimateSegments(text, durationMs) {
  var clean = String(text || '').trim();
  if (!clean) return [];
  var parts = clean.match(/[^.!?。！？؟]+[.!?。！？؟]*["')\]]*\s*/g) || [clean];
  var total = 0;
  parts.forEach(function (p) { total += p.length; });
  var t = 0;
  return parts.map(function (p) {
    var start = Math.round(durationMs * t / total);
    t += p.length;
    return { start: start, end: Math.round(durationMs * t / total), text: p.trim() };
  }).filter(function (s) { return s.text; });
}

// Joins very short segments so the watch shows readable rows.
function tidySegments(segs) {
  var out = [];
  segs.forEach(function (s) {
    var text = String(s.text || '').trim();
    if (!text) return;
    var last = out[out.length - 1];
    if (last && (last.text.length < 25 || s.start - last.start < 1500) && last.text.length + text.length < 180) {
      last.text += ' ' + text;
      last.end = s.end;
    } else {
      out.push({ start: Math.max(0, Math.round(s.start)), end: Math.round(s.end), text: text });
    }
  });
  return out;
}

// Cuts audio at pauses: pieces of 3 to 25 seconds, silent pieces dropped.
function splitAtPauses(pcm, rate) {
  var frame = Math.floor(rate / 50);
  var frames = Math.floor(pcm.length / frame);
  if (frames < 2) return [{ start: 0, end: pcm.length }];
  var energy = new Float32Array(frames);
  for (var f = 0; f < frames; f++) {
    var sum = 0;
    for (var i = f * frame; i < (f + 1) * frame; i++) sum += pcm[i] * pcm[i];
    energy[f] = Math.sqrt(sum / frame);
  }
  var sorted = Array.prototype.slice.call(energy).sort(function (a, b) { return a - b; });
  var noise = sorted[Math.floor(sorted.length * 0.2)];
  var thresh = Math.max(noise * 2.5, 250);
  var minFrames = 150, maxFrames = 1250, pauseFrames = 18;
  var cuts = [0];
  var start = 0, run = 0;
  for (f = 0; f < frames; f++) {
    run = energy[f] < thresh ? run + 1 : 0;
    var len = f - start;
    if (run >= pauseFrames && len >= minFrames) {
      start = f - Math.floor(run / 2);
      cuts.push(start);
      run = 0;
    } else if (len >= maxFrames) {
      var best = f, bestE = Infinity;
      for (var k = Math.max(start + minFrames, f - 250); k <= f; k++) {
        if (energy[k] < bestE) { bestE = energy[k]; best = k; }
      }
      start = best;
      cuts.push(start);
      run = 0;
    }
  }
  var pieces = [];
  for (var c = 0; c < cuts.length; c++) {
    var a = cuts[c], b = c + 1 < cuts.length ? cuts[c + 1] : frames;
    var loud = false;
    for (var x = a; x < b && !loud; x++) loud = energy[x] >= thresh;
    if (loud) pieces.push({ start: a * frame, end: c + 1 < cuts.length ? b * frame : pcm.length });
  }
  return pieces.length ? pieces : [{ start: 0, end: pcm.length }];
}

// ---- OpenAI-compatible --------------------------------------------------------------------------

function openaiEndpoint(cfg) {
  var base = trimSlash(cfg.baseUrl || 'https://api.openai.com/v1');
  return /\/audio\/transcriptions$/.test(base) ? base : base + '/audio/transcriptions';
}

function transcribeOpenAI(pcm, rate, lang, cfg) {
  var bytes = codec.wav(codec.normalize(pcm), rate);
  var durationMs = Math.round(pcm.length * 1000 / rate);
  function attempt(format) {
    var fields = [['model', cfg.model || 'whisper-1'], ['response_format', format], ['temperature', '0']];
    if (lang) fields.push(['language', lang]);
    if (format === 'verbose_json') fields.push(['timestamp_granularities[]', 'segment']);
    var mp = multipart(fields, { name: 'recording.wav', type: 'audio/wav', bytes: bytes });
    var headers = { 'Content-Type': mp.type };
    if (cfg.apiKey) headers.Authorization = 'Bearer ' + cfg.apiKey;
    return request({ method: 'POST', url: openaiEndpoint(cfg), headers: headers, body: mp.body, timeout: 180000 });
  }
  return attempt('verbose_json').then(function (res) {
    // Some models (e.g. gpt-4o-transcribe) only return plain JSON.
    if (res.status === 400 && /response_format|verbose_json|timestamp/i.test(res.text)) return attempt('json');
    return res;
  }).then(function (res) {
    if (res.status === 401 || res.status === 403) throw new Error('The API key was refused');
    if (res.status === 404) throw new Error('No transcription endpoint at ' + openaiEndpoint(cfg));
    if (res.status < 200 || res.status >= 300) throw new Error(errorText(res));
    var j = res.json || { text: res.text };
    var text = String(j.text || '').trim();
    if (Array.isArray(j.segments) && j.segments.length) {
      return {
        text: text,
        estimated: false,
        segments: tidySegments(j.segments.map(function (s) {
          return { start: (+s.start || 0) * 1000, end: (+s.end || 0) * 1000, text: s.text };
        }))
      };
    }
    return { text: text, estimated: true, segments: tidySegments(estimateSegments(text, durationMs)) };
  });
}

function testOpenAI(cfg) {
  var url = trimSlash(cfg.baseUrl || 'https://api.openai.com/v1').replace(/\/audio\/transcriptions$/, '') + '/models';
  var headers = {};
  if (cfg.apiKey) headers.Authorization = 'Bearer ' + cfg.apiKey;
  return request({ url: url, headers: headers, timeout: 15000 }).then(function (res) {
    if (res.status === 401 || res.status === 403) throw new Error('The API key was refused');
    if (res.status >= 200 && res.status < 300) {
      var models = res.json && Array.isArray(res.json.data) ? res.json.data.map(function (m) { return m.id; }) : [];
      var model = cfg.model || 'whisper-1';
      if (models.length && models.indexOf(model) < 0) {
        return 'Connected, but "' + model + '" isn\'t in the server\'s model list';
      }
      return 'Connected';
    }
    if (res.status === 404) return 'Server reachable (it has no model list to check)';
    throw new Error(errorText(res));
  });
}

// ---- Wyoming faster-whisper through Home Assistant ----------------------------------------------
// The phone can't open the raw TCP socket Wyoming uses, so this goes through Home Assistant's
// speech-to-text API, which fronts the Wyoming integration: POST /api/stt/<entity>.

function haBase(cfg) { return trimSlash(cfg.url); }
function haHeaders(cfg) { return { Authorization: 'Bearer ' + String(cfg.token || '').trim() }; }

function haInfo(cfg) {
  return request({ url: haBase(cfg) + '/api/stt/' + cfg.entity, headers: haHeaders(cfg), timeout: 15000 })
    .then(function (res) {
      if (res.status === 401) throw new Error('Home Assistant refused the token');
      if (res.status === 404) throw new Error('No speech-to-text entity "' + cfg.entity + '"');
      if (res.status < 200 || res.status >= 300 || !res.json) throw new Error(errorText(res));
      return res.json;
    });
}

function haLanguage(lang, languages) {
  if (!languages || !languages.length) return lang;
  var lower = languages.map(function (l) { return String(l).toLowerCase(); });
  var i = lower.indexOf(lang.toLowerCase());
  if (i >= 0) return languages[i];
  for (var k = 0; k < lower.length; k++) {
    if (lower[k].indexOf(lang.toLowerCase() + '-') === 0 || lower[k].indexOf(lang.toLowerCase() + '_') === 0) {
      return languages[k];
    }
  }
  throw new Error('The speech-to-text engine in Home Assistant doesn\'t support "' + lang + '"');
}

function transcribeHA(pcm, rate, lang, cfg, onProgress) {
  var pcm16 = codec.normalize(codec.upsample(pcm, 16000 / rate));
  var languagesPromise = cfg.languages && cfg.languages.length
    ? Promise.resolve(cfg.languages)
    : haInfo(cfg).then(function (info) { return info.languages || []; });
  return languagesPromise.then(function (languages) {
    var language = haLanguage(lang, languages);
    var pieces = splitAtPauses(pcm16, 16000);
    var segments = [];
    var texts = [];
    var chain = Promise.resolve();
    pieces.forEach(function (piece, i) {
      chain = chain.then(function () {
        if (onProgress) onProgress(Math.round(100 * i / pieces.length));
        return request({
          method: 'POST',
          url: haBase(cfg) + '/api/stt/' + cfg.entity,
          headers: {
            Authorization: haHeaders(cfg).Authorization,
            'X-Speech-Content': 'format=wav; codec=pcm; sample_rate=16000; bit_rate=16; channel=1; language=' + language,
            'Content-Type': 'application/octet-stream'
          },
          body: codec.pcmBytes(pcm16.subarray(piece.start, piece.end)),
          timeout: 120000
        }).then(function (res) {
          if (res.status === 401) throw new Error('Home Assistant refused the token');
          if (res.status === 415) throw new Error('The speech-to-text engine rejected the audio format or language');
          if (res.status < 200 || res.status >= 300) throw new Error(errorText(res));
          var text = res.json && res.json.text ? String(res.json.text).trim() : '';
          if (res.json && res.json.result === 'error' && !text) return;
          if (text) {
            texts.push(text);
            segments.push({ start: piece.start / 16, end: piece.end / 16, text: text });
          }
        });
      });
    });
    return chain.then(function () {
      return { text: texts.join(' '), estimated: false, segments: tidySegments(segments) };
    });
  });
}

function testHA(cfg) {
  return haInfo(cfg).then(function (info) {
    var n = (info.languages || []).length;
    return { message: 'Connected' + (n ? ', ' + n + ' languages' : ''), languages: info.languages || [] };
  });
}

// ---- WebDAV -------------------------------------------------------------------------------------

function davAuth(cfg) {
  var h = {};
  if (cfg.username || cfg.password) {
    h.Authorization = 'Basic ' + codec.toBase64(codec.utf8((cfg.username || '') + ':' + (cfg.password || '')));
  }
  return h;
}

function davFolderParts(cfg) {
  return String(cfg.folder || '').split('/').map(function (s) { return s.trim(); }).filter(Boolean);
}

function davUrl(cfg, name) {
  var path = davFolderParts(cfg).map(encodeURIComponent);
  if (name) path.push(encodeURIComponent(name));
  return trimSlash(cfg.url) + '/' + path.join('/');
}

function davEnsureFolder(cfg) {
  var parts = davFolderParts(cfg);
  var chain = Promise.resolve();
  parts.forEach(function (p, i) {
    chain = chain.then(function () {
      var url = trimSlash(cfg.url) + '/' + parts.slice(0, i + 1).map(encodeURIComponent).join('/') + '/';
      return request({ method: 'MKCOL', url: url, headers: davAuth(cfg), timeout: 20000 }).then(function (res) {
        if (res.status === 401) throw new Error('The WebDAV server refused the username or password');
        // 201 made, 405 already there, 301/302 some servers redirect existing folders
        if ([200, 201, 204, 301, 302, 405].indexOf(res.status) < 0) {
          throw new Error('Couldn\'t create the folder "' + p + '" (HTTP ' + res.status + ')');
        }
      });
    });
  });
  return chain;
}

function davPut(cfg, name, bytes, type) {
  var headers = davAuth(cfg);
  headers['Content-Type'] = type;
  return request({ method: 'PUT', url: davUrl(cfg, name), headers: headers, body: bytes, timeout: 180000 })
    .then(function (res) {
      if (res.status === 401) throw new Error('The WebDAV server refused the username or password');
      if (res.status === 507) throw new Error('The WebDAV server is out of space');
      if (res.status < 200 || res.status >= 300) throw new Error('Upload failed (HTTP ' + res.status + ')');
    });
}

function uploadWebDAV(cfg, files) {
  return davEnsureFolder(cfg).then(function () {
    var chain = Promise.resolve();
    files.forEach(function (f) {
      chain = chain.then(function () { return davPut(cfg, f.name, f.bytes, f.type); });
    });
    return chain;
  });
}

function testWebDAV(cfg) {
  var headers = davAuth(cfg);
  headers.Depth = '0';
  return request({ method: 'PROPFIND', url: trimSlash(cfg.url) + '/', headers: headers, timeout: 15000 })
    .then(function (res) {
      if (res.status === 401) throw new Error('The server refused the username or password');
      if (res.status === 207 || res.status === 200) return 'Connected';
      if (res.status === 404) throw new Error('No WebDAV folder at that address');
      throw new Error('The server answered HTTP ' + res.status);
    });
}

// ---- Configured? --------------------------------------------------------------------------------

function transcriptionReady(t) {
  if (!t) return false;
  if (t.provider === 'openai') return !!(t.openai && trimSlash(t.openai.baseUrl) && t.openai.model);
  if (t.provider === 'ha') return !!(t.ha && trimSlash(t.ha.url) && t.ha.token && t.ha.entity);
  return false;
}

function webdavReady(w) {
  return !!(w && /^https?:\/\//i.test(trimSlash(w.url)));
}

function transcribe(pcm, rate, lang, t, onProgress) {
  if (t.provider === 'openai') return transcribeOpenAI(pcm, rate, lang, t.openai);
  if (t.provider === 'ha') return transcribeHA(pcm, rate, lang, t.ha, onProgress);
  return Promise.reject(new Error('Set up transcription in the phone settings'));
}

module.exports = {
  transcribe: transcribe,
  testOpenAI: testOpenAI,
  testHA: testHA,
  uploadWebDAV: uploadWebDAV,
  testWebDAV: testWebDAV,
  transcriptionReady: transcriptionReady,
  webdavReady: webdavReady,
  estimateSegments: estimateSegments,
  splitAtPauses: splitAtPauses
};
