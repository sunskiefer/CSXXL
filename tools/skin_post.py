#!/usr/bin/env python3
"""CSXXL: after gen_vst.py.
 1. paints the two 2x2 marker blocks into the VOICE page's picture (the touch ribbon's page; the plugin finds the page on the
    display by them, src/xyz_pad.c). The pages are sh_mode_0_<n>.png, shown while pg_<n> is on;
 2. makes the overview tiles and the header buttons (OVERVIEW, <, >) invisible touch areas that tint while pressed,
    placed over the tile / header art.
Usage: skin_post.py <skin dir>."""
import json, os, re, sys
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, "..", "vst")
Y_OFF = 0   # component bounds are in plugin-area pixels (layout y has the 86 px offset added)
MARK_TL = [(23, 7, 91), (91, 7, 23), (7, 91, 23), (23, 91, 7)]
MARK_BR = [(91, 23, 7), (7, 23, 91), (61, 7, 61), (7, 61, 61)]
sk = sys.argv[1]
if os.path.isdir(os.path.join(sk, "Plugin Skins")): sk = os.path.join(sk, "Plugin Skins")
tp = os.path.join(sk, "TUI.json"); tui = json.load(open(tp))
defs = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
pages = [d["value"] for d in defs if d["key"].startswith("CSXXL|")]   # one per Q-Link bank (sub-page); they share the component types
page = pages[0]
dd = {d["key"]: d["value"] for d in defs}
keys = [m.group(1) for m in (re.match(r'\s*\{"([^"]+)",', l) for l in open(os.path.join(ROOT, "build", "params.h"))) if m]

# 1) ribbon markers
n_pg = keys.index("pg_10")
mode = [c for c in page["componentsData"] if c["componentData"].get("name") == "Mode"
        and any(h.endswith("Parameter %d" % n_pg) for h in c["bounds"].get("additionalInvalidatingHandles", []))]
if len(mode) != 1: sys.exit("skin_post: expected the VOICE page picture, found %d" % len(mode))
png = os.path.join(sk, mode[0]["componentData"]["data"]["image"]); im = Image.open(png).convert("RGB"); w, h = im.size
assert (w, h) == (1280, 628), (w, h)
for (x0, y0, cols) in ((0, 0, MARK_TL), (w - 2, h - 2, MARK_BR)):
    for k, col in enumerate(cols): im.putpixel((x0 + k % 2, y0 + k // 2), col)
im.save(png)

# 2) touch areas
tiles = {"go_%d" % t["page"]: (t["x"], t["y"] + Y_OFF, t["w"], t["h"]) for t in json.load(open(os.path.join(ROOT, "overview_tiles.json")))}
navs = {"nav_zoom": (96, 36, 152, 52), "nav_prev": (213, 36, 58, 52), "nav_next": (277, 36, 58, 52)}
rects = dict(tiles)
for k, (cx, cy, ww, hh) in navs.items(): rects[k] = (cx - ww // 2, cy - hh // 2 + Y_OFF, ww, hh)
def param_of(inst):
    for m in inst["handle remapping"]["map"]:
        if m["key"] == "Data" and m["value"].startswith("Parameter "): return keys[int(m["value"].split()[1])]
def tint(ww, hh, rad):
    im2 = Image.new("RGBA", (ww, hh), (0, 0, 0, 0)); ImageDraw.Draw(im2).rounded_rectangle([2, 2, ww - 3, hh - 3], radius=rad, fill=(255, 255, 255, 46)); return im2
done = 0; styled = set()
for inst in [i for pg in pages for i in pg["componentsData"]]:
    k = param_of(inst)
    if k not in rects or not inst["componentData"]["type"].startswith("shTrig_"): continue
    x, y, ww, hh = rects[k]; d = dd[inst["componentData"]["type"]]
    if inst["componentData"]["type"] in styled:
        inst["bounds"]["bounds"] = "%d %d %d %d" % (x, y, ww, hh); done += 1; continue
    styled.add(inst["componentData"]["type"])
    d["componentsData"] = [s for s in d["componentsData"] if s["componentData"]["type"] != "Label"]
    for s in d["componentsData"]:
        s["bounds"]["bounds"] = "0 0 %d %d" % (ww, hh)
        if s["componentData"]["type"] == "Button":
            data = s["componentData"]["data"]
            Image.new("RGBA", (ww, hh), (0, 0, 0, 40 if k.startswith("go_") else 6)).save(os.path.join(sk, data["offImage"]))   # 16 % black: a fully clear picture can be skipped by the host's hit test
            tint(ww, hh, 14 if k.startswith("go_") else 10).save(os.path.join(sk, data["onImage"]))
    inst["bounds"]["bounds"] = "%d %d %d %d" % (x, y, ww, hh); done += 1
if done != len(rects) * len(pages): sys.exit("skin_post: placed %d of %d touch areas" % (done, len(rects)))
json.dump(tui, open(tp, "w"), separators=(",", ":"))
print("skin_post: ribbon markers in page 10, %d touch areas placed in %d Q-Link banks" % (done, len(pages)))
