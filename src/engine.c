/* CSXXL engine: CS-80-style two-layer polyphonic synth for the Force / MPC OS (mpc_engine contract, 44.1 kHz, int16 stereo).
 * Written for this plugin (not a port). Voices follow the CS-80 signal path: saw/pulse/sub/sine oscillators -> HPF -> 2x LPF
 * -> VCA, each layer (I = "a", II = "b") with its own level/brilliance envelopes, 8 voices per layer.
 * Parameters arrive as strings keyed by params_gen.h; all are 0..100 (buttons 0/1).
 * No allocation in render(). File I/O only on the preset save trigger and at create(). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "params_gen.h"

#define SR 44100.0f
#define NV 8
#define CHUNK 16
#define NFACTORY 768
#define NSLOTS 1536
#define PI_F 3.14159265f

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float expmap(float x, float lo, float hi) { return lo * powf(hi / lo, clampf(x, 0.f, 1.f)); }
static inline float flush(float x) { return fabsf(x) < 1e-18f ? 0.f : x; }

/* ---------------------------------------------------------------- filter */
typedef struct { float ic1, ic2; } Svf;
typedef struct { float a1, a2, a3, k; } SvfC;
static void svf_c(SvfC *c, float fc, float k) {
    fc = clampf(fc, 20.f, 0.45f * SR);
    float g = tanf(PI_F * fc / SR);
    c->k = k; c->a1 = 1.f / (1.f + g * (g + k)); c->a2 = g * c->a1; c->a3 = g * c->a2;
}
static inline void svf_run(Svf *s, const SvfC *c, float x, float *lp, float *hp) {
    float v3 = x - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = flush(2.f * v1 - s->ic1); s->ic2 = flush(2.f * v2 - s->ic2);
    *lp = v2; *hp = x - c->k * v1 - v2;
}

/* ---------------------------------------------------------------- envelope */
typedef struct { int stage; float v; } Env;   /* 0 idle 1 attack 2 decay/sustain 3 release */
static inline void env_on(Env *e) { e->stage = 1; }
static inline void env_off(Env *e) { if (e->stage) e->stage = 3; }
static float env_step(Env *e, float att, float dec, float sus, float rel, int n) {
    float c;
    switch (e->stage) {
    case 1: c = 1.f - expf(-(float)n / (fmaxf(att, 0.0005f) * SR) * 3.f); e->v += (1.15f - e->v) * c;
            if (e->v >= 1.f) { e->v = 1.f; e->stage = 2; } break;
    case 2: c = 1.f - expf(-(float)n / (fmaxf(dec, 0.002f) * SR) * 3.f); e->v += (sus - e->v) * c; break;
    case 3: c = 1.f - expf(-(float)n / (fmaxf(rel, 0.002f) * SR) * 3.f); e->v += (0.f - e->v) * c;
            if (e->v < 0.0004f) { e->v = 0.f; e->stage = 0; } break;
    default: e->v = 0.f; break;
    }
    return e->v;
}

/* ---------------------------------------------------------------- oscillators */
static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}
static inline float osc_saw(float ph, float dt) { return 2.f * ph - 1.f - blep(ph, dt); }
static inline float osc_pulse(float ph, float dt, float w) {
    float v = ph < w ? 1.f : -1.f;
    float p2 = ph - w; if (p2 < 0.f) p2 += 1.f;
    return v + blep(ph, dt) - blep(p2, dt);
}
static inline float wrap1(float x) { return x - floorf(x); }

/* ---------------------------------------------------------------- state */
typedef struct {
    int active, note, gate, held_by_pedal;
    float vel, pitch, tpitch;       /* semitones (MIDI note units), glided */
    float ph1, ph2, phs, phsin, pwm_ph, vib_ph, lfo_ph, snh_ph, snh_val, lfo_gain;
    Env ea, ef, eh;
    Svf h, l1, l2;
    uint32_t order;
    float dly;                      /* line B gate delay (s) before the envelopes start */
} Voice;

typedef struct {
    Voice v[NV];
    int held[16], nheld;            /* keys held by the player (arp / mono) */
    int arp_notes[16], narp, arp_i, arp_dir, arp_note_on;
    int arp_latched;
    double arp_t; float arp_gate_t;
    float echo[44100]; int echo_w;
    float tilt_lp;
} Layer;

typedef struct {
    float P[N_PARAMS];
    Layer L[2];
    uint32_t order;
    float bend, mw, at, ribbon_prev;
    int pedal;
    float rm_env, rm_stage_on, rm_note;
    int rm_stage; float rm_ph;
    float sub_ph, sub_snh; float sub_snh_t;
    float chor[4096]; int chor_w; float chor_ph;
    /* reverb */
    float cb[2][4][1700]; int ci[2][4]; float cd[2][4];
    float ab[2][2][600]; int ai[2][2];
    float outg;
    float user[NSLOTS][N_PARAMS]; unsigned char user_set[NSLOTS];
    char data_dir[256];
    uint32_t rng;
} Inst;

static const int COMB_LEN[2][4] = {{1116, 1188, 1277, 1356}, {1139, 1211, 1300, 1379}};
static const int AP_LEN[2][2] = {{556, 441}, {579, 464}};

static inline float rnd01(Inst *I) { I->rng = I->rng * 1664525u + 1013904223u; return (float)(I->rng >> 8) / 16777216.f; }

/* ---------------------------------------------------------------- param access */
static inline float lp(const Inst *I, int ly, int slot) {
    int i = LIDX[ly][slot];
    return i < 0 ? 0.f : I->P[i] * 0.01f;
}
#define G(name) (I->P[E_##name] * 0.01f)

/* ---------------------------------------------------------------- factory patches (own, generated from recipes) */
static void lset(float *out, int ly, int slot, float v) {
    int i = LIDX[ly][slot];
    if (i >= 0) out[i] = clampf(v, 0.f, 100.f);
}
static float vary(uint32_t *s, float base, float amt) {
    *s = *s * 1664525u + 1013904223u;
    float r = (float)((*s >> 8) & 0xFFFF) / 65535.f * 2.f - 1.f;
    return clampf(base + r * amt, 0.f, 100.f);
}
static void patch_gen(int n, float *out) {
    for (int i = 0; i < N_PARAMS; i++) out[i] = PDEF[i];
    uint32_t s = 0x9E3779B9u * (uint32_t)(n + 1) + 12345u;
    int cat = n % 8, var = (n / 8) % 16, bank = n / 128;
    float vamt = 4.f + (float)var * 1.2f;
    for (int ly = 0; ly < 2; ly++) {
        /* defaults of a "neutral saw" layer */
        float wav = 10, wlv = 80, plv = 40, wid = 40, pw = 50, rte = 25, sub = 10;
        float lf = 45, lres = 15, la = 10, ld = 40, ls = 60, lr = 30, lm = 40;
        float hf = 10, hres = 0, ha = 5, hd = 30, hs = 100, hr = 20, hm = 0;
        float aa = 8, ad = 40, as = 80, ar = 30, lvl = 80, sine = 0, flt = 80;
        float il = 20, al = 85, ibr = 40, abr = 65, vib = 30, vrt = 40;
        float glid = 0, mon = 0, arp = 0, tmp = 50, typ = 0;
        switch (cat) {
        case 0: /* brass */    wlv = 90; plv = 50; lf = 35; lm = 55; la = 25; ld = 45; ls = 55; aa = 18; ad = 40; as = 85; ar = 25; ibr = 30; abr = 75; break;
        case 1: /* strings */  wav = 30; wlv = 70; plv = 65; wid = 35; lf = 55; lres = 8; lm = 15; la = 40; aa = 45 + var; as = 90; ar = 50; hf = 25; vib = 45; break;
        case 2: /* pad */      wav = 40; plv = 60; wid = 55; lf = 40; lm = 25; la = 55; ld = 60; aa = 70; ad = 60; as = 85; ar = 75; il = 10; al = 70; vib = 20; break;
        case 3: /* bass */     wlv = 90; plv = 30; sub = 75; lf = 22; lres = 28; lm = 45; la = 3; ld = 30; ls = 25; aa = 3; ad = 30; as = 70; ar = 12; mon = 100; glid = 8; break;
        case 4: /* lead */     wlv = 80; plv = 60; wid = 30; lf = 45; lres = 35; lm = 35; aa = 8; as = 85; ar = 28; mon = 100; glid = 22; vib = 55; vrt = 50; break;
        case 5: /* ensemble */ if (ly == 0) { wav = 30; plv = 65; aa = 50; ar = 55; lf = 55; } else { wlv = 90; la = 25; lf = 38; lm = 55; aa = 18; } break;
        case 6: /* fx */       wav = 70; wlv = 60; plv = 70; wid = 70; lf = 30; lres = 55; lm = 60; la = 20; ld = 55; ls = 30; aa = 10; ad = 55; as = 50; ar = 55; sine = 40; break;
        default: /* arp */     wlv = 85; plv = 45; lf = 40; lres = 25; lm = 50; la = 3; ld = 25; ls = 20; aa = 3; ad = 25; as = 60; ar = 20; arp = 50; tmp = 55; typ = 20; break;
        }
        if (ly == 1) { wid += 8; lf += (var & 1) ? 3 : -3; }
        lset(out, ly, L_OSC_WAV, vary(&s, wav, vamt)); lset(out, ly, L_OSC_WLV, vary(&s, wlv, vamt * 0.5f));
        lset(out, ly, L_OSC_PLV, vary(&s, plv, vamt)); lset(out, ly, L_OSC_WID, vary(&s, wid, vamt));
        lset(out, ly, L_OSC_PW, vary(&s, pw, vamt)); lset(out, ly, L_OSC_RTE, vary(&s, rte, vamt));
        lset(out, ly, L_OSC_SUB, vary(&s, sub, vamt * 0.5f));
        lset(out, ly, L_LPF_FRQ, vary(&s, lf, vamt)); lset(out, ly, L_LPF_RES, vary(&s, lres, vamt));
        lset(out, ly, L_LPF_ATT, vary(&s, la, vamt)); lset(out, ly, L_LPF_DEC, vary(&s, ld, vamt));
        lset(out, ly, L_LPF_SUS, vary(&s, ls, vamt)); lset(out, ly, L_LPF_REL, vary(&s, lr, vamt));
        lset(out, ly, L_LPF_MOD, vary(&s, lm, vamt));
        lset(out, ly, L_HPF_FRQ, vary(&s, hf, vamt * 0.6f)); lset(out, ly, L_HPF_RES, hres);
        lset(out, ly, L_HPF_ATT, ha); lset(out, ly, L_HPF_DEC, hd); lset(out, ly, L_HPF_SUS, hs);
        lset(out, ly, L_HPF_REL, hr); lset(out, ly, L_HPF_MOD, hm);
        lset(out, ly, L_AMP_SINE, sine); lset(out, ly, L_AMP_FLT, flt);
        lset(out, ly, L_AMP_ATT, vary(&s, aa, vamt)); lset(out, ly, L_AMP_DEC, vary(&s, ad, vamt));
        lset(out, ly, L_AMP_SUS, vary(&s, as, vamt * 0.6f)); lset(out, ly, L_AMP_REL, vary(&s, ar, vamt));
        lset(out, ly, L_AMP_LVL, 80);
        lset(out, ly, L_CS80_IL, il); lset(out, ly, L_CS80_AL, al);
        lset(out, ly, L_CS80_IBR, vary(&s, ibr, vamt)); lset(out, ly, L_CS80_ABR, vary(&s, abr, vamt));
        lset(out, ly, L_VIB_VIB, vary(&s, vib, vamt)); lset(out, ly, L_VIB_VRT, vrt);
        lset(out, ly, L_PERF_GLID, glid); lset(out, ly, L_PERF_MON, mon);
        lset(out, ly, L_ARP_SRC, arp); lset(out, ly, L_ARP_TMP, tmp); lset(out, ly, L_ARP_TYP, typ);
        lset(out, ly, L_ECHO_LVL, cat == 6 ? 35.f : 0.f); lset(out, ly, L_ECHO_TIM, vary(&s, 40, 20)); lset(out, ly, L_ECHO_REP, 35);
    }
    out[E_p6_master_det] = vary(&s, 52, 4); out[E_p8_chorus_on] = (cat == 1 || cat == 2 || cat == 5) ? 100.f : 0.f;
    out[E_p8_reverb_mix] = (cat == 2 || cat == 6) ? 40.f : 22.f;
    if (cat == 6) { out[E_p9_levels_rm] = 45; out[E_p8_ring_mod_ab] = 30; }
    (void)bank;
}

/* ---------------------------------------------------------------- preset helpers */
static void preset_load(Inst *I, int n) {
    n = (int)clampf((float)n, 0.f, (float)(NSLOTS - 1));
    float tmp[N_PARAMS];
    const float *src;
    if (I->user_set[n]) src = I->user[n];
    else if (n < NFACTORY) { patch_gen(n, tmp); src = tmp; }
    else { for (int i = 0; i < N_PARAMS; i++) tmp[i] = PDEF[i]; src = tmp; }
    for (int i = 0; i < N_PARAMS; i++) if (!PBTN[i] && i != E_p11_preset_bnk && i != E_p11_preset_pat) I->P[i] = src[i];
}
static int preset_index(const Inst *I) {
    int b = (int)(I->P[E_p11_preset_bnk] * 0.01f * 12.f); if (b > 11) b = 11;
    int p = (int)(I->P[E_p11_preset_pat] * 1.27f + 0.5f); if (p > 127) p = 127;
    return b * 128 + p;
}
static void user_file(const Inst *I, char *path, size_t n) { snprintf(path, n, "%s/csxxl_user_presets.bin", I->data_dir[0] ? I->data_dir : "."); }
static void user_save_file(Inst *I) {
    char path[300]; user_file(I, path, sizeof path);
    char tmp[310]; snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    uint32_t magic = 0x43535831u, np = N_PARAMS; fwrite(&magic, 4, 1, f); fwrite(&np, 4, 1, f);
    for (uint32_t i = 0; i < NSLOTS; i++) if (I->user_set[i]) { fwrite(&i, 4, 1, f); fwrite(I->user[i], sizeof(float), N_PARAMS, f); }
    fclose(f); rename(tmp, path);
}
static void user_load_file(Inst *I) {
    char path[300]; user_file(I, path, sizeof path);
    FILE *f = fopen(path, "rb"); if (!f) return;
    uint32_t magic = 0, np = 0;
    if (fread(&magic, 4, 1, f) == 1 && fread(&np, 4, 1, f) == 1 && magic == 0x43535831u && np == N_PARAMS) {
        uint32_t i;
        while (fread(&i, 4, 1, f) == 1 && i < NSLOTS && fread(I->user[i], sizeof(float), N_PARAMS, f) == (size_t)N_PARAMS) I->user_set[i] = 1;
    }
    fclose(f);
}

/* ---------------------------------------------------------------- notes */
static float note_shift(const Inst *I, int ly) {
    float oct = roundf((lp(I, ly, L_PERF_OCT) - 0.5f) * 4.f) * 12.f;
    float tun = (lp(I, ly, L_PERF_TUN) - 0.5f) * 2.f;           /* +-1 semitone */
    float crs = roundf((G(p6_master_crs) - 0.5f) * 24.f);
    float fine = (G(p6_master_fine) - 0.5f) * 1.0f;
    float det = (G(p6_master_det) - 0.5f) * 0.6f * (ly ? 1.f : -1.f);
    float rib = (G(p10_ribbon_rib) - 0.5f) * 2.f * (G(p10_ribbon_rng) * 12.f);
    return oct + tun + crs + fine + det + rib;
}
static void voice_on(Inst *I, int ly, int note, float vel, int retrig_env, int mono) {
    Layer *L = &I->L[ly]; Voice *v = 0;
    if (mono) v = &L->v[0];
    else {
        for (int i = 0; i < NV; i++) if (!L->v[i].active) { v = &L->v[i]; break; }
        if (!v) { uint32_t best = 0xFFFFFFFFu; for (int i = 0; i < NV; i++) { uint32_t o = L->v[i].order + (L->v[i].gate ? 0x40000000u : 0u); if (o < best) { best = o; v = &L->v[i]; } } }
    }
    int was = v->active && v->gate;
    v->note = note; v->vel = vel; v->gate = 1; v->held_by_pedal = 0; v->order = ++I->order;
    v->tpitch = (float)note;
    float gt = lp(I, ly, L_PERF_GTP) * 100.f; float glid = lp(I, ly, L_PERF_GLID);
    int gl_on = glid > 0.01f && gt < 66.f && !(gt >= 33.f && !was);
    if (!v->active || !gl_on) v->pitch = (float)note;
    if (!v->active) { v->ph1 = v->ph2 = v->phs = v->phsin = 0.f; v->ph1 = rnd01(I); v->ph2 = rnd01(I); v->lfo_gain = 0.f; v->ea.v = v->ef.v = v->eh.v = 0.f; memset(&v->h, 0, sizeof(Svf)); memset(&v->l1, 0, sizeof(Svf)); memset(&v->l2, 0, sizeof(Svf)); }
    v->active = 1;
    v->dly = (ly == 1 && (retrig_env || !was)) ? lp(I, ly, L_ECHO_DLY) * 1.5f : 0.f;
    if (retrig_env || !was) { env_on(&v->ea); env_on(&v->ef); env_on(&v->eh); }
    I->rm_stage = 1; I->rm_note = (float)note;
}
static void voice_off(Inst *I, int ly, int note) {
    Layer *L = &I->L[ly];
    for (int i = 0; i < NV; i++) {
        Voice *v = &L->v[i];
        if (v->active && v->gate && v->note == note) {
            if (I->pedal && lp(I, ly, L_PERF_FT) >= 0.5f) { v->held_by_pedal = 1; }
            else { v->gate = 0; env_off(&v->ea); env_off(&v->ef); env_off(&v->eh); }
        }
    }
}
static void held_add(Layer *L, int n) { for (int i = 0; i < L->nheld; i++) if (L->held[i] == n) return; if (L->nheld < 16) L->held[L->nheld++] = n; }
static void held_del(Layer *L, int n) { for (int i = 0; i < L->nheld; i++) if (L->held[i] == n) { for (int j = i; j < L->nheld - 1; j++) L->held[j] = L->held[j + 1]; L->nheld--; return; } }

static int arp_on(const Inst *I, int ly) { return lp(I, ly, L_ARP_SRC) >= 0.25f; }
static void layer_key(Inst *I, int ly, int note, int vel, int on) {
    Layer *L = &I->L[ly];
    float fv = vel / 127.f;
    if (arp_on(I, ly)) {
        if (on) {
            if (L->arp_latched && L->nheld == 0) { L->narp = 0; }
            L->arp_latched = 0; held_add(L, note);
            int found = 0; for (int i = 0; i < L->narp; i++) if (L->arp_notes[i] == note) found = 1;
            if (!found && L->narp < 16) L->arp_notes[L->narp++] = note;
        } else {
            held_del(L, note);
            if (lp(I, ly, L_ARP_HLD) >= 0.5f) { if (L->nheld == 0) L->arp_latched = 1; }
            else { for (int i = 0; i < L->narp; i++) if (L->arp_notes[i] == note) { for (int j = i; j < L->narp - 1; j++) L->arp_notes[j] = L->arp_notes[j + 1]; L->narp--; break; } }
        }
        return;
    }
    int mono = lp(I, ly, L_PERF_MON) >= 0.5f;
    if (on) { held_add(L, note); voice_on(I, ly, note, fv, 1, mono); }
    else {
        held_del(L, note);
        if (mono && L->nheld > 0) voice_on(I, ly, L->held[L->nheld - 1], 0.8f, lp(I, ly, L_PERF_GTP) < 0.33f, 1);
        else voice_off(I, ly, note);
    }
}
static void all_off(Inst *I) {
    for (int ly = 0; ly < 2; ly++) { Layer *L = &I->L[ly]; L->nheld = 0; L->narp = 0; L->arp_note_on = -1;
        for (int i = 0; i < NV; i++) { Voice *v = &L->v[i]; v->gate = 0; v->held_by_pedal = 0; env_off(&v->ea); env_off(&v->ef); env_off(&v->eh); } }
}
static void key_event(Inst *I, int note, int vel, int on) {
    int split = G(p7_split_son) >= 0.5f; int sp = 24 + (int)(G(p7_split_spl) * 72.f);
    int a = 1, b = 1;
    if (split) { a = note < sp; b = note >= sp; }
    if (a) layer_key(I, 0, note, vel, on);
    if (b) layer_key(I, 1, note, vel, on);
}

/* ---------------------------------------------------------------- engine API */
static void *eng_create(const char *data_dir) {
    Inst *I = (Inst *)calloc(1, sizeof(Inst));
    if (!I) return 0;
    for (int i = 0; i < N_PARAMS; i++) I->P[i] = PDEF[i];
    I->rng = 0x1234567u; I->mw = 0.f; I->at = 0.f;
    for (int ly = 0; ly < 2; ly++) I->L[ly].arp_note_on = -1, I->L[ly].arp_dir = 1;
    for (int c = 0; c < 2; c++) for (int k = 0; k < 4; k++) I->cd[c][k] = 0.f;
    if (data_dir) snprintf(I->data_dir, sizeof I->data_dir, "%s", data_dir);
    user_load_file(I);
    return I;
}
static void eng_destroy(void *p) { free(p); }

static int key_idx(const char *key) {
    for (int i = 0; i < N_PARAMS; i++) if (!strcmp(PKEY[i], key)) return i;
    return -1;
}
static void eng_set(void *p, const char *key, const char *val) {
    Inst *I = (Inst *)p; int i = key_idx(key); if (i < 0) return;
    float v = (float)atof(val);
    if (PBTN[i]) {
        if (v < 0.5f) return;
        if (i == E_p11_load) preset_load(I, preset_index(I));
        else if (i == E_p11_save) { int n = preset_index(I); memcpy(I->user[n], I->P, sizeof(I->P)); I->user_set[n] = 1; user_save_file(I); }
        else if (i == E_p11_init) { for (int k = 0; k < N_PARAMS; k++) if (!PBTN[k] && k != E_p11_preset_bnk && k != E_p11_preset_pat) I->P[k] = PDEF[k]; }
        return;
    }
    I->P[i] = clampf(v, 0.f, 100.f);
}
static int eng_get(void *p, const char *key, char *buf, int len) {
    Inst *I = (Inst *)p; int i = key_idx(key); if (i < 0 || len < 6) return 0;
    return snprintf(buf, (size_t)len, "%d", (int)(PBTN[i] ? 0.f : I->P[i] + 0.5f));
}
static void eng_midi(void *p, const uint8_t *m, int n) {
    Inst *I = (Inst *)p; if (n < 2) return;
    int st = m[0] & 0xF0;
    if (st == 0x90 && n >= 3) key_event(I, m[1], m[2], m[2] > 0);
    else if (st == 0x80 && n >= 3) key_event(I, m[1], 0, 0);
    else if (st == 0xE0 && n >= 3) I->bend = ((m[2] << 7 | m[1]) - 8192) / 8192.f;
    else if (st == 0xD0) I->at = m[1] / 127.f;
    else if (st == 0xA0 && n >= 3) I->at = m[2] / 127.f;
    else if (st == 0xB0 && n >= 3) {
        if (m[1] == 1) I->mw = m[2] / 127.f;
        else if (m[1] == 64) {
            int on = m[2] >= 64; I->pedal = on;
            if (!on) for (int ly = 0; ly < 2; ly++) for (int i = 0; i < NV; i++) { Voice *v = &I->L[ly].v[i]; if (v->held_by_pedal) { v->held_by_pedal = 0; v->gate = 0; env_off(&v->ea); env_off(&v->ef); env_off(&v->eh); } }
        } else if (m[1] == 123 || m[1] == 120) all_off(I);
    }
}

/* ---------------------------------------------------------------- arp */
static void arp_run(Inst *I, int ly, int frames) {
    Layer *L = &I->L[ly];
    if (!arp_on(I, ly)) { if (L->arp_note_on >= 0) { voice_off(I, ly, L->arp_note_on); L->arp_note_on = -1; } return; }
    float bpm = 40.f + lp(I, ly, L_ARP_TMP) * 200.f;
    float cl = (ly ? G(p7_split_ckb) : G(p7_split_cka));
    float div = cl <= 0.25f ? 1.f : cl <= 0.5f ? 2.f : cl <= 0.75f ? 4.f : 0.5f;   /* 16th, 8th, quarter, 32nd */
    double step = (60.0 / bpm) / 4.0 * div * SR;
    L->arp_t += frames;
    double gate = step * (0.1 + 0.85 * lp(I, ly, L_ARP_GAT));
    if (L->arp_note_on >= 0 && L->arp_t >= gate) { voice_off(I, ly, L->arp_note_on); L->arp_note_on = -1; }
    if (L->arp_t >= step) {
        L->arp_t -= step; if (L->arp_t > step) L->arp_t = 0;
        if (L->arp_note_on >= 0) { voice_off(I, ly, L->arp_note_on); L->arp_note_on = -1; }
        if (L->narp > 0) {
            int octs = 1 + (int)(lp(I, ly, L_ARP_OCT) * 3.99f);
            int total = L->narp * octs; if (total < 1) total = 1;
            float typ = lp(I, ly, L_ARP_TYP);
            int idx;
            if (typ < 0.2f) { L->arp_i = (L->arp_i + 1) % total; idx = L->arp_i; }
            else if (typ < 0.4f) { L->arp_i = (L->arp_i + total - 1) % total; idx = L->arp_i; }
            else if (typ < 0.6f) { if (total == 1) idx = 0; else { L->arp_i += L->arp_dir; if (L->arp_i >= total - 1) { L->arp_i = total - 1; L->arp_dir = -1; } else if (L->arp_i <= 0) { L->arp_i = 0; L->arp_dir = 1; } idx = L->arp_i; } }
            else if (typ < 0.8f) { idx = (int)(rnd01(I) * total); if (idx >= total) idx = total - 1; }
            else { L->arp_i = (L->arp_i + 1) % total; idx = L->arp_i; }
            if (idx >= total) idx = 0;
            int note = L->arp_notes[idx % L->narp] + 12 * (idx / L->narp);
            float vel = lp(I, ly, L_ARP_VEL) >= 0.5f ? 0.8f : 0.7f;
            if (note > 127) note = 127;
            L->arp_note_on = note;
            voice_on(I, ly, note, vel, 1, lp(I, ly, L_PERF_MON) >= 0.5f);
        }
    }
}

/* ---------------------------------------------------------------- voice render */
typedef struct {
    float wav, wlv, plv, wid, pw, rte, sub;
    float lf, lres, la, ld, ls, lr, lm;
    float hf, hres, ha, hd, hs, hr, hm;
    float sine, flt, aa, ad, as, ar, lvl;
    float lrte, lwav, lmde, ldst, latt, lrel;
    float vlfo, vvib, vvrt, vslo, vshi;
    float t_lve, t_hve, t_laf, t_haf, t_mix, t_key;
    float il, al, ibr, abr, ilv, alv;
    float glid, shift, bend_semi;
    float k_brl, k_brh, k_lvl, k_lvh;
    float rel_mul;
} LP;

static void load_lp(const Inst *I, int ly, LP *q) {
#define R(f, s) q->f = lp(I, ly, s)
    R(wav, L_OSC_WAV); R(wlv, L_OSC_WLV); R(plv, L_OSC_PLV); R(wid, L_OSC_WID); R(pw, L_OSC_PW); R(rte, L_OSC_RTE); R(sub, L_OSC_SUB);
    R(lf, L_LPF_FRQ); R(lres, L_LPF_RES); R(la, L_LPF_ATT); R(ld, L_LPF_DEC); R(ls, L_LPF_SUS); R(lr, L_LPF_REL); R(lm, L_LPF_MOD);
    R(hf, L_HPF_FRQ); R(hres, L_HPF_RES); R(ha, L_HPF_ATT); R(hd, L_HPF_DEC); R(hs, L_HPF_SUS); R(hr, L_HPF_REL); R(hm, L_HPF_MOD);
    R(sine, L_AMP_SINE); R(flt, L_AMP_FLT); R(aa, L_AMP_ATT); R(ad, L_AMP_DEC); R(as, L_AMP_SUS); R(ar, L_AMP_REL); R(lvl, L_AMP_LVL);
    R(lrte, L_LFO_RTE); R(lwav, L_LFO_WAV); R(lmde, L_LFO_MDE); R(ldst, L_LFO_DST); R(latt, L_LFO_ATT); R(lrel, L_LFO_REL);
    R(vlfo, L_VIB_LFO); R(vvib, L_VIB_VIB); R(vvrt, L_VIB_VRT); R(vslo, L_VIB_SLO); R(vshi, L_VIB_SHI);
    R(t_lve, L_TOUCH_LVE); R(t_hve, L_TOUCH_HVE); R(t_laf, L_TOUCH_LAF); R(t_haf, L_TOUCH_HAF); R(t_mix, L_TOUCH_MIX); R(t_key, L_TOUCH_KEY);
    R(il, L_CS80_IL); R(al, L_CS80_AL); R(ibr, L_CS80_IBR); R(abr, L_CS80_ABR); R(ilv, L_CS80_ILV); R(alv, L_CS80_ALV);
    R(glid, L_PERF_GLID);
#undef R
    q->shift = note_shift(I, ly);
    q->k_brl = G(p6_keys_brl); q->k_brh = G(p6_keys_brh); q->k_lvl = G(p6_keys_lvl); q->k_lvh = G(p6_keys_lvh);
    q->rel_mul = 0.5f + G(p10_sustain_sln) * 1.5f;
}

static float lfo_wave(float ph, int shape, float snh) {
    if (shape == 0) return sinf(2.f * PI_F * ph);
    if (shape == 1) return 4.f * fabsf(ph - 0.5f) - 1.f;
    if (shape == 2) return 2.f * ph - 1.f;
    if (shape == 3) return ph < 0.5f ? 1.f : -1.f;
    return snh;
}

static void render_layer(Inst *I, int ly, float *bus, int frames, float sub_lfo, float sub_vco, float sub_vcf, float sub_vca) {
    Layer *L = &I->L[ly]; LP q; load_lp(I, ly, &q);
    int lshape = (int)(q.lwav * 4.99f);
    float lrate = expmap(q.lrte, 0.05f, 20.f);
    float vibrate = expmap(q.vvrt, 0.5f, 10.f);
    float snhrate = expmap(q.vslo, 0.5f, 20.f);
    int dst = (int)(q.ldst * 3.99f), mde = (int)(q.lmde * 2.99f);
    float modbar = G(p7_mod_bar_bar);
    int mb_dst = (int)(G(p7_mod_bar_dst) * 3.99f);
    float bri_master = (G(p6_master_bri) - 0.5f) * 6.f;
    float rsn = (G(p6_master_rsn) - 0.5f) * 0.5f;
    float bendsemi = (float)(int)(lp(I, ly, L_PERF_BND) * 12.f + 0.5f);
    float ribpitch = 0.f; (void)ribpitch;
    float glid_tau = expmap(q.glid, 0.005f, 2.f);
    for (int vi = 0; vi < NV; vi++) {
        Voice *v = &L->v[vi]; if (!v->active) continue;
        for (int off = 0; off < frames; off += CHUNK) {
            int n = frames - off < CHUNK ? frames - off : CHUNK;
            if (v->dly > 0.f) { v->dly -= (float)n / SR; continue; } /* line B DELAY: start of the envelope is delayed */
            /* envelopes */
            float rm = q.rel_mul;
            float ea = env_step(&v->ea, expmap(q.aa, 0.002f, 6.f), expmap(q.ad, 0.01f, 8.f), q.as, expmap(q.ar, 0.01f, 10.f) * rm, n);
            float ef = env_step(&v->ef, expmap(q.la, 0.002f, 6.f), expmap(q.ld, 0.01f, 8.f), q.ls, expmap(q.lr, 0.01f, 10.f) * rm, n);
            float eh = env_step(&v->eh, expmap(q.ha, 0.002f, 6.f), expmap(q.hd, 0.01f, 8.f), q.hs, expmap(q.hr, 0.01f, 10.f) * rm, n);
            if (v->ea.stage == 0) { v->active = 0; break; }
            /* glide */
            float gc = 1.f - expf(-(float)n / (glid_tau * SR));
            if (q.glid > 0.01f) v->pitch += (v->tpitch - v->pitch) * gc; else v->pitch = v->tpitch;
            /* lfo */
            v->lfo_ph = wrap1(v->lfo_ph + lrate * n / SR);
            float lfo = lfo_wave(v->lfo_ph, lshape, v->snh_val);
            float fadet = expmap(q.latt, 0.01f, 4.f);
            if (v->gate) v->lfo_gain = fminf(1.f, v->lfo_gain + (float)n / (fadet * SR));
            else v->lfo_gain = fmaxf(0.f, v->lfo_gain - (float)n / (expmap(q.lrel, 0.01f, 4.f) * SR));
            float srcdepth = mde == 0 ? 1.f : (mde == 1 ? I->mw : I->at);
            float ld = q.vlfo * srcdepth * v->lfo_gain;
            float mbm = I->mw * modbar;
            float lfo_to_p = 0, lfo_to_f = 0, lfo_to_a = 0, lfo_to_pw = 0;
            float lv = lfo * ld;
            if (dst == 0) lfo_to_p += lv * 2.f; else if (dst == 1) lfo_to_f += lv * 3.f; else if (dst == 2) lfo_to_a += lv; else lfo_to_pw += lv * 0.4f;
            float mv = (G(p7_mod_bar_mde) >= 0.5f ? sub_lfo : lfo) * mbm;
            if (mb_dst == 0) lfo_to_p += mv * 2.f; else if (mb_dst == 1) lfo_to_f += mv * 3.f; else if (mb_dst == 2) lfo_to_a += mv; else lfo_to_pw += mv * 0.4f;
            v->vib_ph = wrap1(v->vib_ph + vibrate * n / SR);
            float vib = sinf(2.f * PI_F * v->vib_ph) * q.vvib * 0.5f;
            v->snh_ph += snhrate * n / SR; if (v->snh_ph >= 1.f) { v->snh_ph -= 1.f; v->snh_val = rnd01(I) * 2.f - 1.f; }
            float snh_f = v->snh_val * q.vshi * 2.f;
            /* pitch (semitones) */
            float pit = v->pitch + 0.03f * sinf((float)(vi * 5 + ly * 2) * 1.7f) + q.shift + I->bend * bendsemi + lfo_to_p + vib + sub_vco;
            float f1 = 440.f * exp2f((pit - 69.f) / 12.f);
            float f2 = f1 * exp2f((q.wid - 0.5f) * 0.12f);
            float sinmul = q.sub < 0.34f ? 1.f : (q.sub < 0.67f ? 0.5f : 0.25f); /* SINE / SUB -1 oct / SUB -2 oct */
            float dt1 = clampf(f1 / SR, 1e-5f, 0.45f), dt2 = clampf(f2 / SR, 1e-5f, 0.45f);
            v->pwm_ph = wrap1(v->pwm_ph + expmap(q.rte, 0.1f, 8.f) * n / SR);
            float pw = clampf(0.5f + q.wid * 0.4f + 0.2f * sinf(2.f * PI_F * v->pwm_ph) * q.pw + lfo_to_pw, 0.05f, 0.95f);
            /* brilliance / filter */
            float keyt = clampf(((float)v->note - 36.f) / 60.f, 0.f, 1.f);
            float bri = lerpf(q.ibr, q.abr, ea) - 0.5f;
            float kbri = (lerpf(q.k_brl, q.k_brh, keyt) - 0.5f) * 3.f;
            float vel = v->vel;
            float lcut_oct = q.lm * 6.f * ef + bri * 6.f + kbri + bri_master + (v->note - 60.f) / 12.f * q.t_key
                           + vel * q.t_lve * 3.f + I->at * q.t_laf * 3.f + lfo_to_f + snh_f + sub_vcf;
            float vdr = sinf((float)(vi * 7 + ly * 3) * 2.399f); /* per-voice-card trimmer drift */
            float lcut = expmap(q.lf, 40.f, 16000.f) * exp2f(lcut_oct + 0.04f * vdr);
            float hcut = expmap(q.hf, 20.f, 8000.f) * exp2f(q.hm * 3.f * eh + vel * q.t_hve * 3.f + I->at * q.t_haf * 3.f);
            float res = clampf(q.lres + rsn, 0.f, 1.f);
            SvfC hc, c1;
            svf_c(&hc, hcut, 1.4f - q.hres * 1.2f);
            svf_c(&c1, lcut, 2.f * (1.f - 0.82f * res));
            /* amplitude */
            float lvlkey = (lerpf(q.k_lvl, q.k_lvh, keyt) - 0.5f) * 1.2f;
            float ilvel = 1.f - q.ilv * 0.5f * (1.f - vel), alvel = 1.f - q.alv * 0.5f * (1.f - vel);
            float g = lerpf(q.il * ilvel, q.al * alvel, ea) * 1.6f * q.lvl * (1.f + lvlkey);
            g *= 1.f + lfo_to_a * 0.5f + sub_vca;
            if (g < 0.f) g = 0.f;
            /* audio */
            for (int s = 0; s < n; s++) {
                v->ph1 += dt1; if (v->ph1 >= 1.f) v->ph1 -= 1.f;
                v->ph2 += dt2; if (v->ph2 >= 1.f) v->ph2 -= 1.f;
                v->phsin += dt1 * sinmul; if (v->phsin >= 1.f) v->phsin -= 1.f;
                int wi = (int)(q.wav * 8.999f); float o1;
                float sawv = osc_saw(v->ph1, dt1);
                switch (wi) {
                case 0: o1 = sawv; break;                                   /* sawtooth */
                case 1: o1 = -sawv; break;                                  /* ramp 1 (falling) */
                case 2: { float r = 1.f - v->ph1; o1 = 2.f * r * r - 1.f; } break; /* ramp 2 (curved) */
                case 3: o1 = osc_pulse(v->ph1, dt1, 0.5f); break;           /* square */
                case 4: o1 = 4.f * fabsf(v->ph1 - 0.5f) - 1.f; break;       /* peak (triangle) */
                case 5: o1 = 0.6f * sawv + 0.4f * (2.f * wrap1(v->ph1 * 2.f) - 1.f); break; /* octaves */
                case 6: o1 = floorf(sawv * 4.f + 4.f) / 4.f - 1.f; break;   /* digital 1 (stepped saw) */
                case 7: o1 = floorf((4.f * fabsf(v->ph1 - 0.5f) - 1.f) * 3.f + 3.f) / 3.f - 1.f; break; /* digital 2 */
                default: o1 = rnd01(I) * 2.f - 1.f; break;                  /* white noise */
                }
                o1 *= q.wlv;
                float o2 = osc_pulse(v->ph2, dt2, pw) * q.plv;
                float x = (o1 + o2) * 0.5f;
                float lo, hi, tmp;
                svf_run(&v->h, &hc, x, &tmp, &hi);
                svf_run(&v->l1, &c1, x, &lo, &tmp);
                float ser, tmp2; svf_run(&v->l2, &c1, hi, &ser, &tmp2); /* LP of the HP output (series path) */
                float mixv = q.t_mix, filt;
                if (mixv < 0.5f) filt = lerpf(lo, ser, mixv * 2.f); else filt = lerpf(ser, hi, (mixv - 0.5f) * 2.f);
                float sn = sinf(2.f * PI_F * v->phsin) + 0.04f * sinf(6.f * PI_F * v->phsin);
                sn *= q.sine * 0.4f;
                bus[off + s] += (filt * q.flt * 1.3f + sn) * g;
            }
        }
    }
}

/* ---------------------------------------------------------------- render */
static void eng_render(void *p, int16_t *out, int frames) {
    Inst *I = (Inst *)p;
    if (frames > 128) frames = 128;
    float A[128], B[128], X[128];
    memset(A, 0, sizeof A); memset(B, 0, sizeof B);
    arp_run(I, 0, frames); arp_run(I, 1, frames);
    /* sub oscillator (global second LFO) */
    float srate = expmap(G(p7_sub_osc_spd) + 0.f, 0.05f, 30.f) * (1.f + I->at * G(p7_sub_touch_spd) * 3.f);
    int sshape = (int)(G(p7_sub_osc_fnc) * 4.99f);
    I->sub_ph = wrap1(I->sub_ph + srate * frames / SR);
    I->sub_snh_t += srate * frames / SR; if (I->sub_snh_t >= 1.f) { I->sub_snh_t -= 1.f; I->sub_snh = rnd01(I) * 2.f - 1.f; }
    float sl = lfo_wave(I->sub_ph, sshape, I->sub_snh);
    float s_vco = sl * (G(p7_sub_osc_vco) + I->at * G(p7_sub_touch_vco)) * 2.f + I->at * (G(p7_sub_touch_bnd) - 0.5f) * 4.f;
    float s_vcf = sl * (G(p7_sub_osc_vcf) + I->at * G(p7_sub_touch_vcf)) * 3.f;
    float s_vca = sl * G(p7_sub_osc_vca) * 0.5f;
    render_layer(I, 0, A, frames, sl, s_vco, s_vcf, s_vca);
    render_layer(I, 1, B, frames, sl, s_vco, s_vcf, s_vca);
    /* per-layer: tone, echo */
    float *bus[2] = {A, B};
    for (int ly = 0; ly < 2; ly++) {
        Layer *L = &I->L[ly]; float *b = bus[ly];
        float tn = (ly ? G(p10_tone_tnb) : G(p10_tone_tna));
        float echo_lvl = lp(I, ly, L_ECHO_LVL), rep = lp(I, ly, L_ECHO_REP) * 0.85f;
        int dl = (int)(expmap(lp(I, ly, L_ECHO_TIM), 0.02f, 0.9f) * SR);
        if (dl > 44099) dl = 44099; if (dl < 64) dl = 64;
        float tilt = tn * 100.f >= 3.f ? (tn - 0.5f) * 1.6f : 0.f;
        for (int s = 0; s < frames; s++) {
            float x = b[s];
            if (tilt != 0.f) { L->tilt_lp += 0.13f * (x - L->tilt_lp); L->tilt_lp = flush(L->tilt_lp); x = L->tilt_lp * (1.f + tilt) + (x - L->tilt_lp) * (1.f - tilt); }
            if (echo_lvl > 0.005f) {
                int r = L->echo_w - dl; if (r < 0) r += 44100;
                float e = L->echo[r];
                L->echo[L->echo_w] = flush(x + e * rep);
                x += e * echo_lvl;
            } else L->echo[L->echo_w] = 0.f;
            L->echo_w++; if (L->echo_w >= 44100) L->echo_w = 0;
            b[s] = x;
        }
    }
    /* ring modulator / bit crusher buses */
    float rmlev = G(p9_levels_rm), bclev = G(p9_levels_bitc);
    float ring[128], crush[128];
    {
        float att = expmap(G(p8_ring_mod_att), 0.002f, 3.f), dec = expmap(G(p8_ring_mod_dec), 0.02f, 4.f);
        float ven = G(p8_ring_mod_ven), ab = G(p8_ring_mod_ab);
        float pit = (G(p8_ring_mod_pit) - 0.5f) * 4.f, pen = (G(p8_ring_mod_pen) - 0.5f) * 4.f, pvl = G(p8_ring_mod_pvl);
        for (int s = 0; s < frames; s++) {
            if (I->rm_stage == 1) { I->rm_env += (1.1f - I->rm_env) * (1.f - expf(-3.f / (att * SR))); if (I->rm_env >= 1.f) { I->rm_env = 1.f; I->rm_stage = 2; } }
            else I->rm_env += (0.f - I->rm_env) * (1.f - expf(-3.f / (dec * SR)));
            float f = 440.f * exp2f((I->rm_note - 69.f) / 12.f + pit + pen * I->rm_env + (pvl - 0.5f) * 0.5f);
            I->rm_ph = wrap1(I->rm_ph + f / SR);
            float car = (1.f - ab) * B[s] + ab * sinf(2.f * PI_F * I->rm_ph);
            ring[s] = A[s] * car * 3.f * ((1.f - ven) + ven * I->rm_env) * rmlev;
        }
    }
    {
        float src = G(p8_crush_src); int bits = 2 + (int)(G(p8_crush_bit) * 14.f); float q = (float)(1 << (bits - 1));
        for (int s = 0; s < frames; s++) {
            float x = src < 0.25f ? 0.f : (src < 0.5f ? A[s] : (src < 0.75f ? B[s] : A[s] + B[s]));
            crush[s] = floorf(x * q + 0.5f) / q * bclev;
        }
    }
    /* stereo mix */
    float mix = G(p6_master_mix);
    float ga = cosf(mix * PI_F * 0.5f) * 1.41f * G(p9_levels_a), gb = sinf(mix * PI_F * 0.5f) * 1.41f * G(p9_levels_b);
    float pa = G(p9_pans_pa), pb = G(p9_pans_pb), pr = G(p9_pans_prm), pc = G(p9_pans_pbc);
    float Lb[128], Rb[128];
    for (int s = 0; s < frames; s++) {
        float a = A[s] * ga, b = B[s] * gb, r = ring[s], c = crush[s];
        Lb[s] = a * cosf(pa * PI_F * 0.5f) + b * cosf(pb * PI_F * 0.5f) + r * cosf(pr * PI_F * 0.5f) + c * cosf(pc * PI_F * 0.5f);
        Rb[s] = a * sinf(pa * PI_F * 0.5f) + b * sinf(pb * PI_F * 0.5f) + r * sinf(pr * PI_F * 0.5f) + c * sinf(pc * PI_F * 0.5f);
    }
    (void)X;
    /* chorus */
    if (G(p8_chorus_on) >= 0.5f) {
        float rate = expmap(G(p8_chorus_rte), 0.2f, 5.f), depth = (0.5f + G(p8_chorus_amt) * 7.5f) * 0.001f * SR;
        for (int s = 0; s < frames; s++) {
            float m = 0.5f * (Lb[s] + Rb[s]);
            I->chor[I->chor_w] = m; I->chor_ph = wrap1(I->chor_ph + rate / SR);
            float d1 = 12.f + depth * (0.5f + 0.5f * sinf(2.f * PI_F * I->chor_ph));
            float d2 = 12.f + depth * (0.5f + 0.5f * sinf(2.f * PI_F * (I->chor_ph + 0.5f)));
            float t1 = (float)I->chor_w - d1; if (t1 < 0.f) t1 += 4096.f;
            float t2 = (float)I->chor_w - d2; if (t2 < 0.f) t2 += 4096.f;
            int i1 = (int)t1, i2 = (int)t2; float f1 = t1 - i1, f2 = t2 - i2;
            float w1 = lerpf(I->chor[i1 & 4095], I->chor[(i1 + 1) & 4095], f1), w2 = lerpf(I->chor[i2 & 4095], I->chor[(i2 + 1) & 4095], f2);
            I->chor_w = (I->chor_w + 1) & 4095;
            Lb[s] = Lb[s] * 0.6f + w1 * 0.6f; Rb[s] = Rb[s] * 0.6f + w2 * 0.6f;
        }
    }
    /* reverb */
    {
        float rmix = G(p8_reverb_mix);
        if (rmix > 0.005f) {
            float fb = 0.70f + G(p8_reverb_size) * 0.27f, damp = 0.05f + G(p8_reverb_dmp) * 0.6f, wid = G(p8_reverb_wid);
            for (int s = 0; s < frames; s++) {
                float in = (Lb[s] + Rb[s]) * 0.015f, o[2] = {0.f, 0.f};
                for (int c = 0; c < 2; c++) {
                    for (int k = 0; k < 4; k++) {
                        int len = COMB_LEN[c][k]; float y = I->cb[c][k][I->ci[c][k]];
                        I->cd[c][k] = flush(y * (1.f - damp) + I->cd[c][k] * damp);
                        I->cb[c][k][I->ci[c][k]] = flush(in + I->cd[c][k] * fb);
                        if (++I->ci[c][k] >= len) I->ci[c][k] = 0;
                        o[c] += y;
                    }
                    for (int k = 0; k < 2; k++) {
                        int len = AP_LEN[c][k]; float bufo = I->ab[c][k][I->ai[c][k]];
                        float yy = -o[c] + bufo; I->ab[c][k][I->ai[c][k]] = flush(o[c] + bufo * 0.5f); o[c] = yy;
                        if (++I->ai[c][k] >= len) I->ai[c][k] = 0;
                    }
                }
                float wl = o[0] * (0.5f + 0.5f * wid) + o[1] * (0.5f - 0.5f * wid), wr = o[1] * (0.5f + 0.5f * wid) + o[0] * (0.5f - 0.5f * wid);
                Lb[s] += wl * rmix * 3.f; Rb[s] += wr * rmix * 3.f;
            }
        }
    }
    float og = G(p9_main_out) * 1.2f, drive = 1.f + G(p7_split_out) * 0.0f;
    for (int s = 0; s < frames; s++) {
        float l = tanhf(Lb[s] * og * drive), r = tanhf(Rb[s] * og * drive);
        out[2 * s] = (int16_t)(clampf(l, -1.f, 1.f) * 32000.f);
        out[2 * s + 1] = (int16_t)(clampf(r, -1.f, 1.f) * 32000.f);
    }
}

static const mpc_engine_t ENGINE = { eng_create, eng_destroy, eng_midi, eng_set, eng_get, eng_render, 0, 0 };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
