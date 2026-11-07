// Messages the phone side shows on the watch or the settings page, in the watch's language and
// the page's language respectively. Each message is an English template with {name}
// placeholders; translations are keyed by that template, and anything missing stays English.

var NAMES = require('./lang_names');

var DICT = {
  fr: require('./i18n/fr'),
  de: require('./i18n/de'),
  es: require('./i18n/es'),
  it: require('./i18n/it'),
  pt: require('./i18n/pt')
};

// {name} placeholders; a value made by language(code) shows as that language's name in the
// message's language.
function fill(template, vars, lang) {
  return String(template).replace(/\{(\w+)\}/g, function (all, k) {
    var v = vars && vars[k];
    if (v == null) return all;
    if (typeof v === 'object' && v.languageCode) {
      var n = NAMES[v.languageCode];
      return n ? n[lang2(lang)] || n.en : v.languageCode;
    }
    return String(v);
  });
}

function language(code) { return { languageCode: code }; }

function lang2(code) {
  var c = String(code || 'en').slice(0, 2).toLowerCase();
  return DICT[c] ? c : 'en';
}

// A message to show later in either language.
function msg(template, vars) {
  return { template: template, vars: vars || null };
}

// An Error that remembers its template, so it can be shown in another language.
function error(template, vars) {
  var e = new Error(fill(template, vars, 'en'));
  e.template = template;
  e.vars = vars || null;
  return e;
}

// Text of a msg(), an error() or a plain string, in a language.
function text(lang, m) {
  if (m == null) return '';
  if (typeof m === 'string') return translate(lang, m);
  if (m.template) {
    var d = DICT[lang2(lang)];
    return fill((d && d[m.template]) || m.template, m.vars, lang);
  }
  return String(m.message || m);
}

function translate(lang, template, vars) {
  var d = DICT[lang2(lang)];
  return fill((d && d[template]) || template, vars, lang);
}

module.exports = { msg: msg, error: error, text: text, t: translate, lang2: lang2, language: language };
