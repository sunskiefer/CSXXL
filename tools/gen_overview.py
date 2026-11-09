#!/usr/bin/env python3
"""CSXXL overview page art: the real header of the panel pages, and one tile per page showing that page in miniature.
Writes vst/art/overview.png and vst/overview_tiles.json (tile rectangles, art coordinates). Usage: gen_overview.py <colour dir>"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = sys.argv[1]; v = os.path.join(root, "vst")
FONT = os.path.join(os.environ.get("MPC_VST", "/home/claude/sunskiefer/mpc-vst-plugins"), "tools/html_art/fonts/TitilliumWeb-SemiBold.ttf")
m = json.load(open(os.path.join(v, "control_map.json")))
W, H, HDR = 1280, 628, 70
ref = Image.open(os.path.join(v, "art", "page6.png")).convert("RGB")
im = Image.new("RGB", (W, H), (22, 22, 24))
im.paste(ref.crop((0, 0, W, HDR)), (0, 0))
d = ImageDraw.Draw(im)
d.rectangle([322, 8, 690, 62], fill=(0, 0, 0))          # replaces the "n/11 NAME" title of a page
d.text((330, 35), "TAP A PAGE", font=ImageFont.truetype(FONT, 38), fill=(255, 255, 255), anchor="lm")
tiles = []; cols, rows, tw, th, gx, gy, x0, y0 = 4, 3, 300, 170, 16, 14, 16, 82
f26 = ImageFont.truetype(FONT, 26)
for k, p in enumerate(m["pages"]):
    n = p["index"]; c, r = k % cols, k // cols
    x, y = x0 + c * (tw + gx), y0 + r * (th + gy)
    d.rounded_rectangle([x, y, x + tw - 1, y + th - 1], radius=10, fill=(12, 12, 12), outline=(140, 140, 140), width=2)
    pg = os.path.join(src, "p%d" % n, "page_caps.png")
    if not os.path.exists(pg): pg = os.path.join(v, "art", "page%d.png" % n)
    body = Image.open(pg).convert("RGB").crop((0, HDR, W, H))
    thumb = body.resize((tw - 12, int((tw - 12) * body.height / body.width)), Image.LANCZOS)
    im.paste(thumb, (x + 6, y + 6))
    d.text((x + 12, y + th - 20), "%d  %s" % (n, p["name"]), font=f26, fill=(240, 240, 240), anchor="lm")
    tiles.append({"page": n, "x": x, "y": y, "w": tw, "h": th})
im.save(os.path.join(v, "art", "overview.png"), optimize=True)
json.dump(tiles, open(os.path.join(v, "overview_tiles.json"), "w"), indent=1)
print("overview:", len(tiles), "tiles")
