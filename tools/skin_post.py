#!/usr/bin/env python3
"""CSXXL: after gen_vst.py, paint the two 2x2 marker blocks into the VOICE page background only (the touch ribbon's page).
The plugin finds the page on the display by them (src/xyz_pad.c, see the XYZ pad recipe). Usage: skin_post.py <skin dir>."""
import json, os, sys
from PIL import Image
MARK_TL = [(23, 7, 91), (91, 7, 23), (7, 91, 23), (23, 91, 7)]
MARK_BR = [(91, 23, 7), (7, 23, 91), (61, 7, 61), (7, 61, 61)]
sk = sys.argv[1]
if os.path.isdir(os.path.join(sk, "Plugin Skins")): sk = os.path.join(sk, "Plugin Skins")
tui = json.load(open(os.path.join(sk, "TUI.json")))
done = 0
for d in tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
    if d["key"] != "VOICE|VOICE": continue
    for c in d["value"].get("componentsData", []):
        cd = c["componentData"]
        if cd.get("name") == "Background" and cd.get("type") == "Image":
            png = os.path.join(sk, cd["data"]["image"]); im = Image.open(png).convert("RGB"); w, h = im.size
            assert (w, h) == (1280, 628), (w, h)
            for (x0, y0, cols) in ((0, 0, MARK_TL), (w - 2, h - 2, MARK_BR)):
                for k, col in enumerate(cols): im.putpixel((x0 + k % 2, y0 + k // 2), col)
            im.save(png); done += 1
if done != 1: sys.exit("skin_post: expected the VOICE background, found %d" % done)
print("skin_post: ribbon markers written into the VOICE page background")
