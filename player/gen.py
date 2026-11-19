#!/usr/bin/env python3
"""Peanut-GB's sound (minigb_apu) and its core's header into gen/: the sound at 24 kHz (half the GamePad's 48: half the work), and with one
function more, to save and load its state."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
LIBWUP = os.environ.get('LIBWUP') or next((p for p in (os.path.join(HERE, '..', '..', 'libwup'), os.path.join(HERE, '..', '..', 'wup-homebrew', 'libwup')) if os.path.isdir(p)), None)
if not LIBWUP: sys.exit('libwup not found: set LIBWUP to wup-homebrew/libwup')
sys.path.insert(0, os.path.join(LIBWUP, 'tools'))
import deps
STATE = '''
/* (added by gen.py) the whole sound state in one piece, for save states and rewind: load 0 = out of here, 1 = into here */
#include <string.h>
unsigned audio_state(uint8_t *p, int load)
{
	struct { void *at; unsigned n; } parts[] = { { audio_mem, sizeof audio_mem }, { chans, sizeof chans }, { &vol_l, sizeof vol_l }, { &vol_r, sizeof vol_r } };
	unsigned total = 0;
	for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++) {
		if (p && load) memcpy(parts[i].at, p + total, parts[i].n);
		else if (p) memcpy(p + total, parts[i].at, parts[i].n);
		total += parts[i].n;
	}
	return total;
}
'''


def core(t):
    """the core, a little faster on the GamePad's ARM9: the game and its battery memory are read where they lie (main.c's
    WUP_ROM_READ...: Peanut-GB calls back through function pointers for every byte, 7 % of a frame), and the memory
    access functions may be inlined (static)"""
    n = {}
    for a, b in (('gb->gb_rom_read(gb,', 'WUP_ROM_READ(gb,'), ('gb->gb_cart_ram_read(gb,', 'WUP_RAM_READ(gb,'), ('gb->gb_cart_ram_write(gb,', 'WUP_RAM_WRITE(gb,'),
                 ('\nuint8_t __gb_read(struct gb_s *gb, uint16_t addr)', '\nstatic uint8_t __gb_read(struct gb_s *gb, uint16_t addr)'),
                 ('\nvoid __gb_write(struct gb_s *gb, uint_fast16_t addr, uint8_t val)', '\nstatic void __gb_write(struct gb_s *gb, uint_fast16_t addr, uint8_t val)')):
        n[a] = t.count(a); t = t.replace(a, b)
    assert n['gb->gb_rom_read(gb,'] >= 10 and n['gb->gb_cart_ram_read(gb,'] >= 1 and n['gb->gb_cart_ram_write(gb,'] >= 1, n
    return t


deps.fetch('peanut-gb'); src = deps.DEPS['peanut-gb'][0]; out = os.path.join(HERE, 'gen'); os.makedirs(out, exist_ok=True)
h = open(os.path.join(src, 'examples', 'sdl2', 'minigb_apu', 'minigb_apu.h')).read()
assert '#define AUDIO_SAMPLE_RATE\t32768' in h
files = {'minigb_apu.h': h.replace('#define AUDIO_SAMPLE_RATE\t32768', '#define AUDIO_SAMPLE_RATE\t24000'),   # (main.c plays each sample twice: 48 kHz)
         'minigb_apu.c': open(os.path.join(src, 'examples', 'sdl2', 'minigb_apu', 'minigb_apu.c')).read() + STATE,
         'peanut_gb.h': core(open(os.path.join(src, 'peanut_gb.h')).read())}
for n, t in files.items():
    p = os.path.join(out, n)
    if not os.path.exists(p) or open(p).read() != t: open(p, 'w').write(t)
