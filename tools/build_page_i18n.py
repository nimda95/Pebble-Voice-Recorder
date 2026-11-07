"""Inlines the settings page's translations (src/config/i18n/<lang>.json, each mapping English text
to its translation) into src/config/index.html, which must stay a single file for the Pebble app.
Run: python3 tools/build_page_i18n.py"""
import glob
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, 'src', 'config', 'index.html')

langs = {}
for path in sorted(glob.glob(os.path.join(ROOT, 'src', 'config', 'i18n', '*.json'))):
    with open(path, encoding='utf-8') as f:
        langs[os.path.splitext(os.path.basename(path))[0]] = json.load(f)

block = '// I18N-BEGIN\nwindow.I18N = ' + json.dumps(langs, ensure_ascii=False, indent=1, sort_keys=False) + ';\n// I18N-END'
with open(PAGE, encoding='utf-8') as f:
    html = f.read()
html, n = re.subn(r'// I18N-BEGIN\n.*?// I18N-END', lambda m: block, html, flags=re.S)
if n != 1:
    raise SystemExit('I18N markers not found in index.html')
with open(PAGE, 'w', encoding='utf-8') as f:
    f.write(html)
print('Inlined %s (%d strings each at most)' % (', '.join(langs), max([len(v) for v in langs.values()] or [0])))
