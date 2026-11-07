"""Writes src/pkjs/config_page.js, the settings page as a JS string, so PebbleKit JS can open it with
Pebble.openURL() as a data: URL. That's the classic settings flow, used when the Pebble app doesn't
open bundled pages (its plugins / permissions preview is off). Run by wscript on every build."""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(ROOT, 'src', 'config', 'index.html'), encoding='utf-8') as f:
    html = f.read()
out = os.path.join(ROOT, 'src', 'pkjs', 'config_page.js')
text = '// Generated from src/config/index.html by tools/embed_config_page.py; do not edit.\n' \
       'module.exports = ' + json.dumps(html) + ';\n'
old = open(out, encoding='utf-8').read() if os.path.exists(out) else None
if old != text:
    with open(out, 'w', encoding='utf-8') as f:
        f.write(text)
