"""Builds the watch's translations into resource files: resources/strings/<lang>.bin from
src/c/strings_<lang>.def and src/c/lang_names.def. English stays compiled into the app; only the
watch's own language is loaded at run time, which keeps the 64 KB Pebble 2 Duo within its memory.

Run by wscript on every build; can also be run by hand: python3 tools/build_strings.py

File layout (little endian):
  uint16 string_count, uint16 language_count
  uint16 offset[string_count + language_count]   from the file start; 0 = not translated
  NUL-terminated UTF-8 text
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'c')
OUT = os.path.join(ROOT, 'resources', 'strings')
LANGS = ['fr', 'de', 'es', 'it', 'pt']  # UiLang order after English (src/c/i18n.h)

C_STRING = r'"((?:[^"\\]|\\.)*)"'


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def ids():
    out = []
    for line in open(os.path.join(SRC, 'strings.def'), encoding='utf-8'):
        m = re.match(r'\s*S\((S_\w+),', line)
        if m:
            out.append(m.group(1))
    return out


def translations(lang):
    out = {}
    for line in open(os.path.join(SRC, 'strings_%s.def' % lang), encoding='utf-8'):
        m = re.match(r'\s*T\((S_\w+), ' + C_STRING + r'\)', line)
        if m:
            out[m.group(1)] = unescape(m.group(2))
    return out


def language_names():
    rows = []
    for line in open(os.path.join(SRC, 'lang_names.def'), encoding='utf-8'):
        if line.startswith('L('):
            rows.append([unescape(s) for s in re.findall(C_STRING, line)])
    return rows  # [code, en, fr, de, es, it, pt]


def build(lang, order, names):
    table = translations(lang)
    unknown = set(table) - set(order)
    if unknown:
        raise SystemExit('strings_%s.def: unknown ids %s' % (lang, ', '.join(sorted(unknown))))
    column = 2 + LANGS.index(lang)
    texts = [table.get(i) for i in order] + [row[column] for row in names]
    count = len(texts)
    header = 4 + 2 * count
    offsets, data = [], bytearray()
    for t in texts:
        if t is None:
            offsets.append(0)
        else:
            offsets.append(header + len(data))
            data += t.encode('utf-8') + b'\0'
    if header + len(data) > 0xFFFF:
        raise SystemExit('strings_%s: too much text for 16-bit offsets' % lang)
    blob = struct.pack('<HH', len(order), len(names)) + struct.pack('<%dH' % count, *offsets) + bytes(data)
    path = os.path.join(OUT, lang + '.bin')
    old = open(path, 'rb').read() if os.path.exists(path) else None
    if old != blob:  # leave the file alone when nothing changed, so the build stays incremental
        with open(path, 'wb') as f:
            f.write(blob)
    return len(blob), sum(1 for t in texts[:len(order)] if t is None)


def main():
    os.makedirs(OUT, exist_ok=True)
    order, names = ids(), language_names()
    for lang in LANGS:
        size, missing = build(lang, order, names)
        if '-q' not in sys.argv:
            print('strings %s: %d bytes%s' % (lang, size, ', %d missing' % missing if missing else ''))


if __name__ == '__main__':
    main()
