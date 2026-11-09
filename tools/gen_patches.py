#!/usr/bin/env python3
"""CSXXL factory patches: 64 written recipes (8 families x 8) x 12 fixed variations = 768 slots. Writes src/patches.h and docs/PATCHES.md.
Values are slider positions 0..100. A recipe lists the changes to the neutral layer for line A and line B ('b' = changes on top of A for line B);
'g' holds the global ones (chorus, reverb, detune, ring mod, crush). Nothing is random."""
import os
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SAW, RAMP1, RAMP2, SQ, PEAK, OCT, D1, D2, NOISE = 5, 17, 28, 39, 50, 61, 72, 83, 100
SLOT = dict(wav="OSC_WAV", wlv="OSC_WLV", plv="OSC_PLV", wid="OSC_WID", pw="OSC_PW", rte="OSC_RTE", sub="OSC_SUB",
            lf="LPF_FRQ", lres="LPF_RES", la="LPF_ATT", ld="LPF_DEC", ls="LPF_SUS", lr="LPF_REL", lm="LPF_MOD",
            hf="HPF_FRQ", hres="HPF_RES", ha="HPF_ATT", hd="HPF_DEC", hs="HPF_SUS", hr="HPF_REL", hm="HPF_MOD",
            sine="AMP_SINE", flt="AMP_FLT", aa="AMP_ATT", ad="AMP_DEC", as_="AMP_SUS", ar="AMP_REL", lvl="AMP_LVL",
            il="CS80_IL", al="CS80_AL", ibr="CS80_IBR", abr="CS80_ABR", ilv="CS80_ILV", alv="CS80_ALV",
            vib="VIB_VIB", vrt="VIB_VRT", viblfo="VIB_LFO", glid="PERF_GLID", gtp="PERF_GTP", mon="PERF_MON",
            arp="ARP_SRC", tmp="ARP_TMP", typ="ARP_TYP", hld="ARP_HLD", oct="ARP_OCT", avel="ARP_VEL", gat="ARP_GAT",
            echolvl="ECHO_LVL", echotim="ECHO_TIM", echorep="ECHO_REP", echodly="ECHO_DLY",
            lforte="LFO_RTE", lfosyn="LFO_SYN", lfowav="LFO_WAV", lfomde="LFO_MDE", lfodst="LFO_DST", lfoatt="LFO_ATT", lforel="LFO_REL",
            tlaf="TOUCH_LAF", thaf="TOUCH_HAF", tlve="TOUCH_LVE", thve="TOUCH_HVE", tkey="TOUCH_KEY", tmix="TOUCH_MIX",
            slo="VIB_SLO", shi="VIB_SHI", tun="PERF_TUN", oc="PERF_OCT")
GLB = dict(det="p6_master_det", chorus="p8_chorus_on", chrte="p8_chorus_rte", chamt="p8_chorus_amt", rvmix="p8_reverb_mix", rvsize="p8_reverb_size",
           rvdmp="p8_reverb_dmp", rm="p9_levels_rm", rmab="p8_ring_mod_ab", rmven="p8_ring_mod_ven", rmpit="p8_ring_mod_pit", bc="p9_levels_bitc", bcbit="p8_crush_bit", bcsrc="p8_crush_src",
           bri="p6_master_bri", rsn="p6_master_rsn", mix="p6_master_mix")
# neutral layer (everything a recipe does not mention)
BASE = dict(wav=SAW, wlv=80, plv=40, wid=40, pw=50, rte=25, sub=10, lf=45, lres=15, la=10, ld=40, ls=60, lr=30, lm=40, hf=10, hres=0, ha=5, hd=30, hs=100, hr=20, hm=0,
            sine=0, flt=80, aa=8, ad=40, as_=80, ar=30, lvl=80, il=20, al=85, ibr=40, abr=65, vib=25, vrt=40, glid=0, mon=0, arp=0, tmp=50, typ=0, hld=0, oct=0, avel=100, gat=60,
            echolvl=0, echotim=40, echorep=35, lforte=30, lfowav=0, lfomde=0, lfodst=17, tlaf=0, thaf=0, tlve=0)
R = {}   # family -> list of recipes
R["BRASS"] = [
 ("Blade brass: slow-swelling saw+pulse, filter opens after the key, aftertouch bite", dict(wlv=90, plv=50, lf=36, lm=62, la=30, ld=45, ls=60, aa=22, as_=90, ar=35, hf=12, lres=22, ibr=30, abr=78, vib=35, tlaf=55), dict(wid=48)),
 ("Fanfare: fast attack, bright", dict(wlv=95, plv=45, lf=46, lm=70, la=8, ld=35, ls=55, aa=8, as_=88, ar=25, ibr=45, abr=85, lres=25), dict(wid=50)),
 ("Soft horn: square, dark and round", dict(wav=SQ, wlv=85, plv=30, lf=30, lm=40, la=22, aa=28, as_=88, ar=40, lres=8, abr=60), dict(wid=45)),
 ("Sforzando: hard bite that settles", dict(wlv=92, plv=55, lf=34, lm=80, la=4, ld=28, ls=38, aa=6, as_=82, ar=22, lres=30, abr=82, tlve=40), dict(wid=45)),
 ("Trombone: mono, slide, slow attack", dict(wlv=90, plv=70, wid=45, lf=26, lm=55, la=26, aa=25, as_=88, ar=30, mon=100, glid=8, lres=18), dict(wid=52)),
 ("Trumpet: pulse-heavy, resonant, vibrato late", dict(wlv=80, plv=75, wid=30, lf=50, lm=65, la=12, aa=10, as_=85, ar=22, lres=32, vib=45, viblfo=0, lfomde=50, lforte=48, lfoatt=60), dict(wid=36)),
 ("Brass section: two lines, one wide and bright", dict(wlv=90, plv=50, lf=38, lm=58, la=22, aa=18, as_=88, ar=30), dict(wav=RAMP1, plv=65, wid=58, lf=42)),
 ("Octave brass: octaves wave, big and hollow", dict(wav=OCT, wlv=85, plv=40, lf=40, lm=55, la=18, aa=14, as_=86, ar=28, lres=20), dict(wid=50)),
]
R["STRINGS"] = [
 ("Warm strings: slow, wide, soft filter", dict(wav=D1 - 50, wlv=70, plv=65, wid=35, lf=55, lres=8, lm=15, la=40, aa=48, ad=50, as_=92, ar=55, hf=25, vib=45, ibr=35, abr=60), dict(wid=45, lf=52)),
 ("Bright strings: high-pass thin top", dict(wlv=75, plv=60, lf=64, lm=18, la=30, aa=36, as_=90, ar=45, hf=38, hres=10, abr=72, vib=50), dict(wid=50)),
 ("Slow swell strings", dict(wlv=70, plv=60, lf=48, lm=30, la=60, ld=60, aa=70, as_=95, ar=62, hf=22, vib=40), dict(wid=48)),
 ("Pizzicato: short, plucked, no sustain", dict(wlv=85, plv=30, lf=40, lm=55, la=2, ld=18, ls=5, aa=2, ad=22, as_=8, ar=20, lres=22), dict(wid=42)),
 ("Solo violin: mono, vibrato by aftertouch", dict(wlv=80, plv=40, lf=52, lm=35, aa=24, as_=90, ar=40, mon=100, glid=10, vib=20, thaf=0, tlaf=30, lfomde=0, lfodst=0, lforte=52), dict(wid=40)),
 ("Cello-like: low, dark, round", dict(wav=SQ, wlv=80, plv=55, lf=32, lm=30, la=30, aa=30, as_=90, ar=45, lres=12), dict(wid=45)),
 ("String machine: ramp plus pulse, chorus", dict(wav=RAMP1, wlv=70, plv=60, lf=50, lm=10, la=20, aa=22, as_=95, ar=40, hf=20), dict(wid=55, lf=54), ),
 ("Tremolo strings: LFO on amplitude", dict(wlv=75, plv=55, lf=52, lm=20, la=35, aa=40, as_=92, ar=45, lforte=62, lfodst=40, lfomde=70, vib=0, viblfo=40), dict(wid=48)),
]
R["PAD"] = [
 ("Tears pad: slow filter, soft attack, big release", dict(wav=PEAK, plv=60, wid=55, lf=40, lm=25, la=55, ld=60, aa=70, ad=60, as_=85, ar=75, il=10, al=70, vib=20), dict(wid=62, lf=44)),
 ("Glass pad: high-passed, resonant shimmer", dict(wlv=60, plv=55, lf=62, lres=40, lm=20, la=40, aa=55, as_=88, ar=70, hf=36, hres=25, vib=15), dict(wid=62)),
 ("Dark choir-ish pad: square+pulse, low filter", dict(wav=SQ, wlv=75, plv=70, wid=50, lf=30, lm=28, la=45, aa=60, as_=90, ar=68, lres=14), dict(wid=58)),
 ("Warm wash: no filter movement, very slow", dict(wav=PEAK, wlv=70, plv=50, lf=46, lm=0, aa=82, as_=92, ar=85, sine=20), dict(wid=56)),
 ("Moving pad: LFO on the filter", dict(wlv=70, plv=60, lf=42, lm=15, la=50, aa=60, as_=88, ar=70, lforte=18, lfodst=25, lfomde=70, viblfo=45, vib=10), dict(wid=60, lforte=24)),
 ("Sweeping pad: filter attack long", dict(wlv=75, plv=55, lf=26, lm=70, la=75, ld=70, ls=75, aa=55, as_=92, ar=65, lres=20), dict(wid=55)),
 ("Sine bed pad: sub sine under a soft saw", dict(wlv=55, plv=30, lf=36, lm=18, aa=65, as_=95, ar=75, sine=65, sub=50), dict(wid=50)),
 ("Pulse-width pad: wide PWM motion", dict(wav=SQ, wlv=40, plv=85, wid=50, rte=18, pw=70, lf=48, lm=20, aa=58, as_=90, ar=66), dict(wid=62, rte=26)),
]
R["BASS"] = [
 ("Sub bass: sine + sub, short filter", dict(wlv=60, plv=20, sub=75, sine=70, lf=20, lres=22, lm=45, la=2, ld=28, ls=25, aa=2, ad=30, as_=75, ar=12, mon=100, glid=6), dict()),
 ("Saw bass: bright pluck", dict(wlv=95, plv=30, lf=28, lres=30, lm=60, la=2, ld=24, ls=15, aa=2, ad=26, as_=70, ar=10, mon=100, glid=8), dict()),
 ("Square bass: round", dict(wav=SQ, wlv=90, plv=20, lf=24, lres=18, lm=40, la=2, ld=30, ls=35, aa=2, as_=75, ar=14, mon=100), dict()),
 ("Resonant acid-ish bass: high resonance, fast env", dict(wlv=95, plv=0, lf=18, lres=62, lm=72, la=2, ld=20, ls=10, aa=2, ad=22, as_=70, ar=8, mon=100, glid=12), dict()),
 ("Fat unison bass: two lines, detuned", dict(wlv=92, plv=40, wid=58, lf=26, lres=24, lm=45, la=2, ld=26, ls=30, aa=2, as_=76, ar=12, mon=100), dict(wid=66)),
 ("Pulse bass: PWM", dict(wav=SQ, wlv=30, plv=90, wid=45, rte=30, lf=26, lres=20, lm=42, la=2, ld=28, ls=30, aa=2, as_=75, ar=12, mon=100), dict()),
 ("Slide bass: long portamento", dict(wlv=92, plv=35, sub=40, lf=24, lres=25, lm=45, la=3, ld=30, ls=40, aa=3, as_=78, ar=14, mon=100, glid=30), dict()),
 ("Organ-ish bass: peak wave, no filter env", dict(wav=PEAK, wlv=90, plv=0, sub=60, lf=34, lm=5, la=1, aa=2, as_=92, ar=10, mon=100), dict()),
]
R["LEAD"] = [
 ("Deckard lead: saw+pulse, mono, ribbon-friendly vibrato", dict(wlv=80, plv=60, wid=30, lf=46, lres=32, lm=36, aa=8, as_=86, ar=30, mon=100, glid=22, vib=50, vrt=50, tlaf=45), dict(wid=36)),
 ("Whistle lead: sine-heavy, soft", dict(wlv=20, plv=20, sine=85, lf=40, lm=10, aa=10, as_=90, ar=30, mon=100, glid=18, vib=60, vrt=48), dict()),
 ("Square lead: hollow", dict(wav=SQ, wlv=85, plv=45, lf=44, lres=28, lm=40, aa=6, as_=88, ar=26, mon=100, glid=15), dict(wid=38)),
 ("Screamer: high resonance, hard filter env", dict(wlv=90, plv=50, lf=40, lres=55, lm=70, la=4, ld=40, ls=60, aa=4, as_=90, ar=24, mon=100, glid=10, tlaf=60), dict(wid=36)),
 ("Soft flute lead: breath noise in the attack", dict(wlv=50, plv=30, sine=40, lf=42, lm=18, la=15, aa=18, as_=88, ar=34, mon=100, glid=14, vib=45), dict(wav=NOISE, wlv=22, plv=0, lres=40, lf=55, aa=14, as_=40, ar=20, mon=0, glid=0)),
 ("Sync-ish lead: octaves wave", dict(wav=OCT, wlv=85, plv=40, lf=48, lres=30, lm=45, aa=6, as_=88, ar=26, mon=100, glid=12), dict(wid=42)),
 ("Slow-vibrato lead: LFO fades in", dict(wlv=80, plv=55, lf=46, lres=28, lm=34, aa=10, as_=88, ar=30, mon=100, glid=20, vib=0, viblfo=55, lforte=38, lfodst=0, lfomde=34, lfoatt=65), dict(wid=36)),
 ("Dual lead: line B an octave-ish detune up", dict(wlv=80, plv=60, lf=46, lres=30, lm=36, aa=8, as_=86, ar=30, mon=100, glid=20, vib=40), dict(wid=62, lf=50)),
]
R["ENSEMBLE"] = [
 ("Brass over strings: A strings, B brass", dict(wav=D1 - 50, wlv=70, plv=65, lf=55, la=40, aa=50, as_=92, ar=55, hf=25), dict(wav=SAW, wlv=90, plv=50, lf=38, lm=55, la=25, aa=18, as_=88, ar=30, hf=10, wid=48)),
 ("Strings and choir pad", dict(wlv=70, plv=60, lf=52, la=38, aa=46, as_=92, ar=56, hf=22), dict(wav=SQ, wlv=70, plv=70, lf=32, lm=30, la=45, aa=60, as_=90, ar=66, wid=58)),
 ("Split-brass ensemble", dict(wlv=92, plv=50, lf=40, lm=60, la=20, aa=14, as_=88, ar=30), dict(wav=PEAK, wlv=80, plv=60, lf=34, lm=30, la=28, aa=30, as_=90, ar=40, wid=58)),
 ("Organ and strings", dict(wav=PEAK, wlv=90, plv=40, lf=42, lm=5, aa=3, as_=95, ar=14, sub=55), dict(wlv=70, plv=60, lf=54, la=36, aa=44, as_=92, ar=50, hf=20, wid=48)),
 ("Pad and bell", dict(wav=PEAK, wlv=70, plv=55, lf=40, la=50, aa=64, as_=88, ar=70), dict(wav=D2, wlv=60, plv=50, lf=60, lres=30, lm=40, la=2, ld=44, ls=10, aa=2, ad=48, as_=12, ar=50, wid=40)),
 ("Wide brass choir", dict(wlv=92, plv=55, wid=50, lf=36, lm=60, la=26, aa=20, as_=90, ar=32), dict(wlv=92, plv=55, wid=64, lf=40, lm=60, la=26, aa=20, as_=90, ar=32)),
 ("Strings with airy noise", dict(wlv=72, plv=62, lf=54, la=36, aa=44, as_=92, ar=52, hf=22), dict(wav=NOISE, wlv=40, plv=0, lres=36, lf=62, lm=20, la=40, aa=42, as_=70, ar=55, hf=40)),
 ("Layered pad and lead", dict(wav=PEAK, plv=60, lf=42, la=55, aa=68, as_=88, ar=70, wid=56), dict(wlv=80, plv=55, lf=48, lres=30, lm=36, aa=8, as_=86, ar=30, mon=100, glid=18, vib=45)),
]
R["FX"] = [
 ("Wind: white noise, resonant band sweeping", dict(wav=NOISE, wlv=70, plv=0, lf=38, lres=58, lm=45, la=55, ld=60, ls=60, aa=40, as_=85, ar=70, hf=24, lforte=14, lfodst=17, lfomde=70, viblfo=50, echolvl=25), dict(lf=44, lforte=19)),
 ("Surf: noise with slow swell and fall", dict(wav=NOISE, wlv=75, plv=0, lf=30, lres=22, lm=60, la=70, ld=72, ls=20, aa=60, ad=68, as_=30, ar=80, hf=14), dict(lf=34)),
 ("Whoosh: fast noise sweep", dict(wav=NOISE, wlv=70, plv=0, lf=16, lres=62, lm=90, la=40, ld=48, ls=5, aa=10, ad=50, as_=20, ar=55, hres=18, hf=16), dict(lm=80)),
 ("Laser: pitch-ish blip via filter, ring mod", dict(wav=D1, wlv=70, plv=60, lf=60, lres=60, lm=80, la=1, ld=18, ls=0, aa=2, ad=20, as_=5, ar=24, echolvl=40, echotim=24), dict(wid=70)),
 ("Dark drone: low square pair, slow movement", dict(wav=SQ, wlv=60, plv=70, wid=60, lf=24, lres=25, lm=20, la=70, aa=75, as_=95, ar=85, lforte=8, lfodst=17, lfomde=70, viblfo=50, sine=40), dict(wid=70, lf=27)),
 ("Sample and hold: random filter steps", dict(wav=SAW, wlv=70, plv=40, lf=40, lres=45, lm=10, aa=12, as_=88, ar=40, lforte=40, lfowav=100, lfodst=17, lfomde=70, viblfo=70, slo=20, shi=85), dict(wid=50)),
 ("Bit-crushed texture: noise plus crush", dict(wav=NOISE, wlv=50, plv=40, lf=50, lres=30, lm=30, la=30, aa=20, as_=80, ar=50), dict(wid=50), ),
 ("Chime FX: ring modulated bell", dict(wav=PEAK, wlv=70, plv=50, lf=64, lres=40, lm=50, la=2, ld=42, ls=8, aa=2, ad=45, as_=10, ar=55, echolvl=35), dict(wid=48)),
]
R["ARP"] = [
 ("Up arp: 1/8 saw pluck", dict(wlv=85, plv=45, lf=40, lres=25, lm=55, la=2, ld=24, ls=15, aa=2, ad=24, as_=50, ar=18, arp=50, tmp=50, typ=0, oct=33), dict(wid=50)),
 ("Up-down arp: bright, 2 octaves", dict(wlv=80, plv=50, lf=46, lres=30, lm=60, la=2, ld=22, ls=12, aa=2, ad=22, as_=45, ar=16, arp=50, tmp=50, typ=40, oct=33), dict(wid=52)),
 ("Down arp: dark pluck", dict(wav=SQ, wlv=85, plv=35, lf=32, lres=22, lm=50, la=2, ld=26, ls=10, aa=2, ad=26, as_=45, ar=18, arp=50, tmp=50, typ=20, oct=0), dict(wid=48)),
 ("Fast arp: 1/16", dict(wlv=85, plv=40, lf=42, lres=28, lm=55, la=2, ld=18, ls=10, aa=2, ad=18, as_=40, ar=12, arp=50, tmp=62, typ=0, oct=33, avel=100), dict(wid=50)),
 ("Echo arp: arp plus tempo echo", dict(wlv=85, plv=45, lf=40, lres=25, lm=55, la=2, ld=24, ls=15, aa=2, ad=24, as_=50, ar=18, arp=50, tmp=50, typ=0, oct=33, echolvl=45, echotim=40, echorep=50), dict(wid=50)),
 ("Hold arp: latched chord with pad underneath", dict(wlv=85, plv=45, lf=42, lres=25, lm=55, la=2, ld=24, ls=15, aa=2, ad=24, as_=50, ar=18, arp=50, tmp=50, typ=40, oct=33, hld=100), dict(arp=0, wav=PEAK, wlv=70, plv=55, lf=40, lm=20, la=50, aa=65, ad=60, as_=88, ar=72, wid=56)),
 ("Keys and arp: A plays keys, B arpeggiates (use keysplit)", dict(wlv=85, plv=45, lf=44, lm=50, aa=4, as_=80, ar=24), dict(arp=50, tmp=50, typ=0, oct=33, ld=24, ls=15, as_=50, ar=18, wid=52)),
 ("Random-ish arp: up-and-down 2", dict(wlv=85, plv=45, lf=42, lres=26, lm=55, la=2, ld=24, ls=15, aa=2, ad=24, as_=50, ar=18, arp=50, tmp=50, typ=80, oct=66), dict(wid=50)),
]
FAM = list(R)
G_BASE = {"BRASS": {}, "STRINGS": dict(chorus=100, chrte=30, chamt=45), "PAD": dict(chorus=100, chrte=22, chamt=55, rvmix=40, rvsize=70), "BASS": {}, "LEAD": dict(rvmix=20),
          "ENSEMBLE": dict(chorus=100, chrte=26, chamt=45, rvmix=25), "FX": dict(rvmix=40, rvsize=75), "ARP": dict(rvmix=18)}
EXTRA_G = {("FX", 3): dict(rm=45, rmab=30, rmven=60), ("FX", 5): {}, ("FX", 6): dict(bc=40, bcbit=55, bcsrc=66), ("FX", 7): dict(rm=40, rmab=60, rmven=50, rmpit=70)}
names_ops = ["as written", "brighter", "darker", "slower attack and release", "short and plucked", "wide: detune, chorus, more width", "echo and reverb",
             "expression: vibrato from the LFO, aftertouch on the filter", "sub octave under line B", "noise breath layer on line A", "mono with glide", "ring mod / crush colour"]
out = ["/* generated by tools/gen_patches.py - do not edit */", "#pragma once"]
def pairs(d):
    return ", ".join("{L_%s, %g}" % (SLOT[k], v) for k, v in d.items()) + (", " if d else "") + "{-1, 0}"
def gpairs(d):
    return ", ".join('{E_%s, %g}' % (GLB[k], v) for k, v in d.items()) + (", " if d else "") + "{-1, 0}"
out.append("typedef struct { short slot; float v; } PatchKV;\ntypedef struct { PatchKV a[40], b[40], g[16]; } Recipe;")
out.append("static const PatchKV BASE_KV[] = {%s};" % pairs(BASE))
out.append("static const Recipe RECIPES[%d] = {" % (len(FAM) * 8))
md = ["# CSXXL factory patches", "", "768 slots = 64 written recipes (8 families x 8) x 12 fixed variations. Slot n: family = n mod 8, recipe = (n/8) mod 8, variation = n/64 (0..11). Names in the plugin stay numbers.", ""]
for f in FAM:
    md.append("## " + f)
    for i, rec in enumerate(R[f]):
        name, a, *rest = rec; b = rest[0] if rest else {}
        A = dict(a); B = dict(a); B.update(b)
        if "wid" not in b: B["wid"] = min(100, A.get("wid", BASE["wid"]) + 8)
        g = dict(G_BASE[f]); g.update(EXTRA_G.get((f, i), {}))
        if len(a) > 40 or len(B) > 40: raise SystemExit("recipe too long: " + name)
        out.append("  { {%s}, {%s}, {%s} }, /* %s %d */" % (pairs(A), pairs(B), gpairs(g), f, i))
        md.append("%d. %s" % (i + 1, name))
    md.append("")
out.append("};")
out.append("static const char *const PATCH_VARIATION[12] = {%s};" % ", ".join('"%s"' % n for n in names_ops))
md.append("## Variations (applied to every recipe)"); md += ["%d. %s" % (i, n) for i, n in enumerate(names_ops)]
open(os.path.join(R_ := os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."), "src", "patches.h"), "w").write("\n".join(out) + "\n")
os.makedirs(os.path.join(R_, "docs"), exist_ok=True); open(os.path.join(R_, "docs", "PATCHES.md"), "w").write("\n".join(md) + "\n")
print("patches:", len(FAM) * 8, "recipes")
