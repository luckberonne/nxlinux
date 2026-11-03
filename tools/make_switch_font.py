#!/usr/bin/env python3
"""Makes the font the host serves as the Switch's shared fonts for a title that draws button icons: a copy of DejaVu Sans with the Nintendo Extended
private-use glyphs (U+E0E0 A, E0E1 B, E0E2 X, E0E3 Y, E0E4 L, E0E5 R, E0E6 ZL, E0E7 ZR, E0E8 SL, E0E9 SR, E0EA D-pad, E0EB..E0EE its four directions,
E0EF +, E0F0 -, E0F1 / E0F2 the sticks) drawn as rings or rounded boxes with their label, and a small ring for the other private-use code points of
U+E000..U+E0FF (the real glyphs are Nintendo's, not in the repo). Needs fontTools (python3 -m venv V; V/bin/pip install fonttools).
  make_switch_font.py [DejaVuSans.ttf] [out.ttf]      the default out is work/run/ssbu/switch_ext.ttf"""
import sys, math
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen

src = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/TTF/DejaVuSans.ttf'
out = sys.argv[2] if len(sys.argv) > 2 else 'work/run/ssbu/switch_ext.ttf'
f = TTFont(src); gs = f.getGlyphSet(); upm = f['head'].unitsPerEm
cmap = f.getBestCmap(); glyf = f['glyf']; hmtx = f['hmtx']; order = f.getGlyphOrder()
adv = upm; cy = int(upm * 0.30)          # the icon sits on the text's middle: centre height

def circle(pen, cx, cyy, r, cw=True, n=16):
    pts = [(cx + r * math.cos(2 * math.pi * k / n), cyy + r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    if cw: pts.reverse()
    pen.moveTo((round(pts[0][0]), round(pts[0][1])))
    for p in pts[1:]: pen.lineTo((round(p[0]), round(p[1])))
    pen.closePath()
def rrect(pen, x0, y0, x1, y1, r, cw=True):
    pts = []
    for (cx, cyy, a0) in ((x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180), (x1 - r, y0 + r, 270)):
        for k in range(5):
            a = math.radians(a0 + 18 * k); pts.append((cx + r * math.cos(a), cyy + r * math.sin(a)))
    if cw: pts.reverse()
    pen.moveTo((round(pts[0][0]), round(pts[0][1])))
    for p in pts[1:]: pen.lineTo((round(p[0]), round(p[1])))
    pen.closePath()
def poly(pen, pts):
    pen.moveTo(pts[0])
    for p in pts[1:]: pen.lineTo(p)
    pen.closePath()

def add(name, glyph):
    glyf[name] = glyph; hmtx[name] = (adv, 0)

# base shapes
p = TTGlyphPen(None); r = int(upm * 0.44); circle(p, adv // 2, cy, r); circle(p, adv // 2, cy, r - int(upm * 0.06), cw=False); add('nx_ring', p.glyph())
p = TTGlyphPen(None); circle(p, adv // 2, cy, int(upm * 0.13)); add('nx_dot', p.glyph())
p = TTGlyphPen(None); rrect(p, int(upm * 0.06), cy - int(upm * 0.30), int(upm * 0.94), cy + int(upm * 0.30), int(upm * 0.16)); rrect(p, int(upm * 0.12), cy - int(upm * 0.24), int(upm * 0.88), cy + int(upm * 0.24), int(upm * 0.11), cw=False); add('nx_box', p.glyph())
p = TTGlyphPen(None); w = int(upm * 0.11); h = int(upm * 0.42)
poly(p, [(adv // 2 - w, cy + h), (adv // 2 + w, cy + h), (adv // 2 + w, cy + w), (adv // 2 + h, cy + w), (adv // 2 + h, cy - w), (adv // 2 + w, cy - w), (adv // 2 + w, cy - h),
         (adv // 2 - w, cy - h), (adv // 2 - w, cy - w), (adv // 2 - h, cy - w), (adv // 2 - h, cy + w), (adv // 2 - w, cy + w)][::-1]); add('nx_cross', p.glyph())

def labelled(base, text, scale):
    """a composite: the base shape and the text's glyphs, scaled, centred on the icon"""
    names = [cmap.get(ord(ch)) for ch in text]
    total = sum(hmtx[n][0] for n in names) * scale
    x = (adv - total) / 2; comps = []
    p = TTGlyphPen(gs)
    p.addComponent(base, (1, 0, 0, 1, 0, 0))
    for n in names:
        p.addComponent(n, (scale, 0, 0, scale, round(x), round(cy - upm * 0.72 * scale * 0.5))); x += hmtx[n][0] * scale
    return p.glyph()

icons = {0xE0E0: ('nx_ring', 'A', 0.55), 0xE0E1: ('nx_ring', 'B', 0.55), 0xE0E2: ('nx_ring', 'X', 0.55), 0xE0E3: ('nx_ring', 'Y', 0.55),
         0xE0E4: ('nx_box', 'L', 0.50), 0xE0E5: ('nx_box', 'R', 0.50), 0xE0E6: ('nx_box', 'ZL', 0.42), 0xE0E7: ('nx_box', 'ZR', 0.42),
         0xE0E8: ('nx_box', 'SL', 0.40), 0xE0E9: ('nx_box', 'SR', 0.40), 0xE0EF: ('nx_ring', '+', 0.60), 0xE0F0: ('nx_ring', '-', 0.60),
         0xE0F1: ('nx_ring', 'L', 0.45), 0xE0F2: ('nx_ring', 'R', 0.45)}
for cp, (base, text, sc) in icons.items(): add('nx_%04x' % cp, labelled(base, text, sc))
for cp, ch in ((0xE0EB, '↑'), (0xE0EC, '↓'), (0xE0ED, '←'), (0xE0EE, '→')): add('nx_%04x' % cp, labelled('nx_cross', ch if cmap.get(ord(ch)) else '', 0.35))
add('nx_e0ea', labelled('nx_cross', '', 1.0))
table = {}
for cp in range(0xE000, 0x10000 if False else 0xE100):
    name = 'nx_%04x' % cp if 'nx_%04x' % cp in glyf.keys() else ('nx_e0ea' if cp == 0xE0EA else 'nx_dot')
    table[cp] = name
order = list(glyf.glyphOrder); f.setGlyphOrder(order)
for t in f['cmap'].tables:
    if t.isUnicode():
        for cp, name in table.items(): t.cmap[cp] = name
f['maxp'].numGlyphs = len(order)
f.save(out); print('wrote', out, len(order), 'glyphs')
