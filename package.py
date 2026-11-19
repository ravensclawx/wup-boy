#!/usr/bin/env python3
"""Build WUP Boy and pack it as the App Store takes it: build/wup_boy.zip holding apps/wup_boy/wup_boy.wup and
apps/wup_boy/player.wup (unzip it onto a GamePad over File Sharing to install by hand).
    LIBWUP=/path/to/wup-homebrew/libwup python3 package.py"""
import os, re, subprocess, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
LIBWUP = os.environ.get('LIBWUP') or next((p for p in (os.path.join(HERE, '..', 'libwup'), os.path.join(HERE, '..', 'wup-homebrew', 'libwup')) if os.path.isdir(p)), None)
if not LIBWUP: sys.exit('libwup not found: set LIBWUP to wup-homebrew/libwup')
LIBWUP = os.path.abspath(LIBWUP)


def built(d):
    r = subprocess.run([sys.executable, os.path.join(LIBWUP, 'tools', 'wupbuild.py'), d], capture_output=True, text=True, cwd=HERE, env=dict(os.environ, LIBWUP=LIBWUP))
    m = re.search(r'(\S+\.wup) \(', r.stdout)
    if r.returncode or not m: sys.exit('%s did not build:\n%s' % (d, r.stdout[-2000:] + r.stderr[-2000:]))
    return open(m.group(1), 'rb').read()


out = os.path.join(HERE, 'build', 'wup_boy.zip'); os.makedirs(os.path.dirname(out), exist_ok=True)
with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
    for name, d in (('apps/wup_boy/wup_boy.wup', HERE), ('apps/wup_boy/player.wup', os.path.join(HERE, 'player'))):
        z.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), built(d))
print(out)
