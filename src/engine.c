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
#include "xyz_pad.h"

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
    unsigned arp_step;
    int held[16], nheld;            /* keys held by the player (arp / mono) */
    float arp_vel; int arp_notes[16], narp, arp_i, arp_dir, arp_note_on;
    int arp_latched;
    double arp_t; float arp_gate_t;
    float echo[44100]; int echo_w;
    float tilt_lp;
} Layer;

typedef struct {
    float P[N_PARAMS];
    Layer L[2];
    uint32_t order;
    int view; unsigned rev; float nav_cool; int pm_page, no_mark, dirty_n; unsigned char dirty[N_PARAMS]; char pm_msg[96];
    float bend, mw, at, ribbon_prev, rm_vel, mb_last, mb_idle, mb_s, mb_pa, mb_pb, mb_pr;
    float pat[128];                 /* per-key (polyphonic) aftertouch 0..1 */
    double bpm; int bpm_ok;         /* host tempo (song_pos) */
    volatile float rib_x, rib_y; volatile unsigned rib_changed; overlay_t pad; float rib_ref; int rib_was; float rib_off;
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
#include "patches.h"
#include "arm_patches.h"
static float lget(const float *out, int ly, int slot) { int i = LIDX[ly][slot]; return i >= 0 ? out[i] : 0.f; }
static void ladd(float *out, int ly, int slot, float d) { lset(out, ly, slot, lget(out, ly, slot) + d); }
static void gset(float *out, int e, float v) { out[e] = clampf(v, 0.f, 100.f); }
/* factory patch n: family n%8, written recipe (n/8)%8, then one of 12 fixed variations (n/64), see tools/gen_patches.py and docs/PATCHES.md */
static void patch_gen(int n, float *out) {
    for (int i = 0; i < N_PARAMS; i++) out[i] = PDEF[i];
    if (n < NARM) {   /* converted Arminator patch: neutral layer, then the matched controls (tools/gen_arm_patches.py) */
        for (int ly = 0; ly < 2; ly++) for (const PatchKV *k = BASE_KV; k->slot >= 0; k++) lset(out, ly, k->slot, k->v);
        for (int m = 0; m < NARMMAP; m++) {
            if (ARM_MAP[m].kind == 'l') lset(out, ARM_MAP[m].line, ARM_MAP[m].slot, ARM_VAL[n][m]);
            else gset(out, ARM_MAP[m].slot, ARM_VAL[n][m]);
        }
        return;
    }
    int fam = n % 8, rec = (n / 8) % 8, var = (n / 64) % 12;
    const Recipe *R = &RECIPES[fam * 8 + rec];
    for (int ly = 0; ly < 2; ly++) {
        for (const PatchKV *k = BASE_KV; k->slot >= 0; k++) lset(out, ly, k->slot, k->v);
        for (const PatchKV *k = ly ? R->b : R->a; k->slot >= 0; k++) lset(out, ly, k->slot, k->v);
    }
    for (const PatchKV *k = R->g; k->slot >= 0; k++) gset(out, k->slot, k->v);
    for (int ly = 0; ly < 2; ly++) {
        switch (var) {
        case 1: ladd(out, ly, L_LPF_FRQ, 12); ladd(out, ly, L_CS80_ABR, 10); ladd(out, ly, L_CS80_IBR, 5); ladd(out, ly, L_LPF_RES, 6); break;
        case 2: ladd(out, ly, L_LPF_FRQ, -14); ladd(out, ly, L_CS80_ABR, -12); ladd(out, ly, L_LPF_RES, -4); break;
        case 3: ladd(out, ly, L_AMP_ATT, 22); ladd(out, ly, L_AMP_REL, 22); ladd(out, ly, L_LPF_ATT, 15); break;
        case 4: lset(out, ly, L_AMP_ATT, fminf(lget(out, ly, L_AMP_ATT), 4.f)); lset(out, ly, L_AMP_DEC, 26); lset(out, ly, L_AMP_SUS, 15); lset(out, ly, L_LPF_SUS, 10); lset(out, ly, L_LPF_DEC, 28); ladd(out, ly, L_AMP_REL, -12); break;
        case 5: ladd(out, ly, L_OSC_WID, 10); break;
        case 6: lset(out, ly, L_ECHO_LVL, 38); lset(out, ly, L_ECHO_TIM, 40); lset(out, ly, L_ECHO_REP, 45); break;
        case 7: lset(out, ly, L_LFO_DST, 0); lset(out, ly, L_LFO_RTE, 48); lset(out, ly, L_LFO_MDE, 0); lset(out, ly, L_VIB_LFO, 60); lset(out, ly, L_TOUCH_LAF, 50); break;
        case 8: if (ly) { lset(out, ly, L_OSC_SUB, 50); ladd(out, ly, L_AMP_SINE, 30); } break;
        case 9: if (!ly) { lset(out, ly, L_OSC_WAV, 100); lset(out, ly, L_OSC_WLV, 45); lset(out, ly, L_OSC_PLV, 0); lset(out, ly, L_LPF_RES, 40); lset(out, ly, L_LPF_FRQ, 55); lset(out, ly, L_LPF_MOD, 20); lset(out, ly, L_LPF_ATT, 20); lset(out, ly, L_AMP_ATT, 14); lset(out, ly, L_AMP_SUS, 55); lset(out, ly, L_AMP_REL, 25); lset(out, ly, L_HPF_FRQ, 35); } break;
        case 10: lset(out, ly, L_PERF_MON, 100); lset(out, ly, L_PERF_GLID, 14); break;
        default: break;
        }
    }
    if (var == 5) { gset(out, E_p6_master_det, 62); gset(out, E_p8_chorus_on, 100); gset(out, E_p8_chorus_amt, 50); }
    if (var == 6 && out[E_p8_reverb_mix] < 40.f) gset(out, E_p8_reverb_mix, 40);
    if (var == 11) { gset(out, E_p9_levels_rm, 40); gset(out, E_p8_ring_mod_ab, 50); gset(out, E_p8_ring_mod_ven, 50); gset(out, E_p9_levels_bitc, 22); gset(out, E_p8_crush_src, 66); }
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
static void pm_name(const Inst *I, int n, char *o, size_t sz) {
    if (n < 0 || n >= NSLOTS) { snprintf(o, sz, " "); return; }
    if (I->user_set[n]) snprintf(o, sz, "%03d  USER %d", n + 1, n + 1);
    else if (n < NARM) snprintf(o, sz, "%03d  %s", n + 1, ARM_NAME[n]);
    else if (n < NFACTORY) snprintf(o, sz, "%03d  Recipe %d", n + 1, n + 1 - NARM);
    else snprintf(o, sz, "%03d  - empty -", n + 1);
}
static void pm_select(Inst *I, int n) {   /* selection lives in the two preset sliders, so the Q-Links and data wheel still work */
    n = n < 0 ? 0 : n > NSLOTS - 1 ? NSLOTS - 1 : n;
    I->P[E_p11_preset_bnk] = ((float)(n / 128) + 0.5f) / 12.f * 100.f;
    I->P[E_p11_preset_pat] = (float)(n % 128) / 1.27f;
    I->pm_page = n / 8;
}
static void pm_describe(Inst *I, const char *pre) {
    char t[64]; int n = preset_index(I); pm_name(I, n, t, sizeof t);
    snprintf(I->pm_msg, sizeof I->pm_msg, "%s%s", pre, t); I->rev++;
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
    return oct + tun + crs + fine + det;
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
    float glid = lp(I, ly, L_PERF_GLID);
    int gl_on = glid > 0.01f;   /* PORT/GLISS (GTP) only changes how the glide moves, see render */
    if (!v->active || !gl_on) v->pitch = (float)note;
    if (!v->active) { v->ph1 = v->ph2 = v->phs = v->phsin = 0.f; v->ph1 = rnd01(I); v->ph2 = rnd01(I); v->lfo_gain = 0.f; v->ea.v = v->ef.v = v->eh.v = 0.f; memset(&v->h, 0, sizeof(Svf)); memset(&v->l1, 0, sizeof(Svf)); memset(&v->l2, 0, sizeof(Svf)); }
    if (lp(I, ly, L_LFO_SYN) >= 0.25f) v->lfo_ph = 0.f;   /* LFO SYNC on: LFO starts with the key */
    v->active = 1;
    v->dly = (ly == 1 && (retrig_env || !was)) ? lp(I, ly, L_ECHO_DLY) * 1.5f : 0.f;
    if (retrig_env || !was) { env_on(&v->ea); env_on(&v->ef); env_on(&v->eh); }
    I->rm_stage = 1; I->rm_note = (float)note; I->rm_vel = vel;
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

static int is_mono(const Inst *I, int ly) { return lp(I, ly, L_PERF_MON) >= 0.5f || (ly ? G(p9_voice_mnb) : G(p9_voice_mna)) >= 0.5f; }
static int arp_on(const Inst *I, int ly) { return lp(I, ly, L_ARP_SRC) >= 0.25f; }
static void layer_key(Inst *I, int ly, int note, int vel, int on) {
    Layer *L = &I->L[ly];
    float fv = vel / 127.f;
    if (arp_on(I, ly)) {
        if (on) {
            if (L->arp_latched && L->nheld == 0) { L->narp = 0; }
            L->arp_latched = 0; held_add(L, note); L->arp_vel = fv;
            int found = 0; for (int i = 0; i < L->narp; i++) if (L->arp_notes[i] == note) found = 1;
            if (!found && L->narp < 16) L->arp_notes[L->narp++] = note;
        } else {
            held_del(L, note);
            if (lp(I, ly, L_ARP_HLD) >= 0.5f) { if (L->nheld == 0) L->arp_latched = 1; }
            else { for (int i = 0; i < L->narp; i++) if (L->arp_notes[i] == note) { for (int j = i; j < L->narp - 1; j++) L->arp_notes[j] = L->arp_notes[j + 1]; L->narp--; break; } }
        }
        if (lp(I, ly, L_ARP_SRC) < 0.67f) return;   /* ARPEGGIATOR: arp only; BOTH: the keys also sound directly */
        if (on) voice_on(I, ly, note, fv, 1, is_mono(I, ly)); else voice_off(I, ly, note);
        return;
    }
    int mono = is_mono(I, ly);
    if (on) { held_add(L, note); voice_on(I, ly, note, fv, 1, mono); }
    else {
        held_del(L, note);
        if (mono && L->nheld > 0) voice_on(I, ly, L->held[L->nheld - 1], 0.8f, 1, 1);
        else voice_off(I, ly, note);
    }
}
static void all_off(Inst *I) {
    for (int ly = 0; ly < 2; ly++) { Layer *L = &I->L[ly]; L->nheld = 0; L->narp = 0; L->arp_note_on = -1;
        for (int i = 0; i < NV; i++) { Voice *v = &L->v[i]; v->gate = 0; v->held_by_pedal = 0; env_off(&v->ea); env_off(&v->ef); env_off(&v->eh); } }
}
static void key_event(Inst *I, int note, int vel, int on) {
    int split = G(p7_split_son) >= 0.5f; int sp = (int)(G(p7_split_spl) * 127.f + 0.5f);   /* split point anywhere on the keyboard (10 octaves); A below, B at/above */
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
    pad_start(&I->pad, data_dir, &I->rib_x, &I->rib_y, &I->rib_changed);   /* the touch ribbon (black stripe on the VOICE page) */
    user_load_file(I);
    return I;
}
static void eng_destroy(void *p) { Inst *I = (Inst *)p; if (I) pad_stop(&I->pad); free(p); }

static int key_idx(const char *key) {
    for (int i = 0; i < N_PARAMS; i++) if (!strcmp(PKEY[i], key)) return i;
    return -1;
}
static void set_view(Inst *I, int v) { if (v < 0 || v > N_PAGES || v == I->view) return; I->view = v; I->rev++; }
static void mark(Inst *I, int i) { if (i >= 0 && i < N_PARAMS && !I->dirty[i]) { I->dirty[i] = 1; I->dirty_n++; } }
static void eng_set(void *p, const char *key, const char *val) {
    Inst *I = (Inst *)p; int i = key_idx(key); if (i < 0) return;
    float v = (float)atof(val);
    if (i >= E_pg_0 && i <= E_pg_0 + N_PAGES) return;   /* page switches are driven by the engine only */
    if (i >= E_ql1 && i <= E_ql32) {   /* Q-Links follow the visible page */
        int t = VIEW_QL[I->view][i - E_ql1]; if (t < 0) return;
        char b[32]; snprintf(b, sizeof b, "%g", (double)(clampf(v, 0.f, 1.f) * 100.f)); I->no_mark = 1; eng_set(p, PKEY[t], b); I->no_mark = 0; mark(I, t); return;   /* the fader of that control follows, see mpc_engine_changed */
    }
    if (i == E_nav_zoom || i == E_nav_prev || i == E_nav_next || (i >= E_go_1 && i < E_go_1 + N_PAGES)) {
        if (v < 0.5f) return;
        if (i == E_nav_zoom) set_view(I, 0);
        else if (i == E_nav_prev || i == E_nav_next) {
            if (I->nav_cool > 0.f) return;
            I->nav_cool = 0.25f;   /* one tap moves one page */
            int nv = I->view <= 0 ? (i == E_nav_next ? 1 : N_PAGES) : I->view + (i == E_nav_next ? 1 : -1);
            if (nv < 1) nv = N_PAGES;
            if (nv > N_PAGES) nv = 1;
            set_view(I, nv);
        } else set_view(I, 1 + i - E_go_1);
        return;
    }
    if (i >= E_pm_1 && i <= E_pm_8) { if (v >= 0.5f) { pm_select(I, I->pm_page * 8 + (i - E_pm_1)); pm_describe(I, "Selected "); } return; }
    if (i == E_pm_prev || i == E_pm_next || i == E_pm_prev10 || i == E_pm_next10) {
        if (v < 0.5f || I->nav_cool > 0.f) return; I->nav_cool = 0.2f;
        int d = i == E_pm_prev ? -1 : i == E_pm_next ? 1 : i == E_pm_prev10 ? -10 : 10, pages = (NSLOTS + 7) / 8;
        I->pm_page = ((I->pm_page + d) % pages + pages) % pages;
        snprintf(I->pm_msg, sizeof I->pm_msg, "Page %d / %d", I->pm_page + 1, pages); I->rev++; return;
    }
    if (PBTN[i]) {
        if (v < 0.5f) return;
        if (i == E_p11_load) { preset_load(I, preset_index(I)); pm_describe(I, "Loaded "); }
        else if (i == E_p11_save) { int n = preset_index(I); memcpy(I->user[n], I->P, sizeof(I->P)); I->user_set[n] = 1; user_save_file(I); pm_describe(I, "Saved "); }
        else if (i == E_p11_init) { for (int k = 0; k < N_PARAMS; k++) if (!PBTN[k] && k != E_p11_preset_bnk && k != E_p11_preset_pat) I->P[k] = PDEF[k]; snprintf(I->pm_msg, sizeof I->pm_msg, "Initialised"); I->rev++; }
        return;
    }
    I->P[i] = clampf(v, 0.f, 100.f);
    if (!I->no_mark && I->dirty_n < N_PARAMS) for (int j = 0; j < 32; j++) if (VIEW_QL[I->view][j] == i) mark(I, E_ql1 + j);   /* a fader moved on screen: its Q-Link (name, value) follows */
    if (i == E_p11_preset_bnk || i == E_p11_preset_pat) { I->pm_page = preset_index(I) / 8; pm_describe(I, "Selected "); }
}
typedef struct { const char *suf; int n; const char *names[9]; } NamedP;
static const NamedP NAMED[] = {
    {"osc_wav", 9, {"SAWTOOTH", "RAMP 1", "RAMP 2", "SQUARE", "PEAK", "OCTAVES", "DIGITAL 1", "DIGITAL 2", "WHITE NOISE"}},
    {"osc_sub", 3, {"SINE", "SUB -1 OCT", "SUB -2 OCT"}},
    {"lfo_wav", 5, {"SINE", "TRIANGLE", "SAW", "SQUARE", "SAMPLE+HOLD"}},
    {"lfo_mde", 3, {"MOD WHEEL", "A/R ENVELOPE", "DIRECT"}},
    {"lfo_dst", 6, {"PITCH", "LO PASS FREQ", "AMPLITUDE", "PULSE WIDTH", "HI PASS FREQ", "RESONANCE"}},
    {"perf_gtp", 2, {"PORTAMENTO", "GLISSANDO"}},
    {"perf_mon", 2, {"POLY", "MONO"}},
    {"arp_src", 3, {"KEYBOARD", "ARPEGGIATOR", "BOTH"}},
    {"arp_typ", 6, {"UP", "DOWN", "UP & DOWN", "DOWN & UP", "UP & DOWN 2", "DOWN & UP 2"}},
    {"arp_hld", 2, {"HOLD OFF", "HOLD ON"}},
    {"arp_vel", 2, {"FIXED VELOCITY", "AS PLAYED"}},
    {"arp_oct", 4, {"1 OCTAVE", "2 OCTAVES", "3 OCTAVES", "4 OCTAVES"}},
    {"arp_tmp", 8, {"TEMPO / 2", "TEMPO / 1.5", "TEMPO x 0.75", "TEMPO", "TEMPO x 1.5", "TEMPO x 2", "TEMPO x 3", "TEMPO x 4"}},
    {"echo_tim", 8, {"/4", "/3", "/2", "2/3", "x1", "x1.5", "x2", "x3"}},
    {"p7_mod_bar_mde", 4, {"TOUCH DIRECT", "HOLD RELEASE", "TOUCH RELEASE", "HOLD DIRECT"}},
    {"p7_mod_bar_dst", 4, {"LINE A PITCH", "LINE B PITCH", "BOTH LINES PITCH", "RING MOD PITCH"}},
    {"gate_s1", 2, {"STEP OFF", "STEP ON"}}, {"gate_s2", 2, {"STEP OFF", "STEP ON"}}, {"gate_s3", 2, {"STEP OFF", "STEP ON"}}, {"gate_s4", 2, {"STEP OFF", "STEP ON"}},
    {"gate_s5", 2, {"STEP OFF", "STEP ON"}}, {"gate_s6", 2, {"STEP OFF", "STEP ON"}}, {"gate_s7", 2, {"STEP OFF", "STEP ON"}}, {"gate_s8", 2, {"STEP OFF", "STEP ON"}},
    {"gate_cg", 2, {"CLOCK", "GATE"}},
    {"p7_split_son", 2, {"OFF", "ON"}},
    {"p8_chorus_on", 2, {"OFF", "ON"}},
    {"p8_crush_src", 3, {"LINE A", "LINE B", "LINE A & B"}},
    {"p9_voice_mna", 2, {"POLY", "MONO"}},
    {"p9_voice_mnb", 2, {"POLY", "MONO"}},
    {"p10_ribbon_rib", 3, {"PITCH", "FILTER", "RESONANCE"}},
    {"p10_sustain_smd", 2, {"SUSTAIN I", "SUSTAIN II"}},
};
static int named_text(const char *key, float v, char *buf, int len) {
    size_t kl = strlen(key); if (kl < 9 || strcmp(key + kl - 8, "_display")) return 0;
    char base[96]; size_t bl = kl - 8; if (bl >= sizeof base) return 0; memcpy(base, key, bl); base[bl] = 0;
    for (size_t k = 0; k < sizeof NAMED / sizeof NAMED[0]; k++) {
        size_t sl = strlen(NAMED[k].suf);
        if (bl >= sl && !strcmp(base + bl - sl, NAMED[k].suf)) {
            if (!strcmp(NAMED[k].suf, "lfo_syn")) break;
            int ix = (int)(v / 100.f * ((float)NAMED[k].n - 0.001f)); if (ix < 0) ix = 0; if (ix >= NAMED[k].n) ix = NAMED[k].n - 1;
            return snprintf(buf, (size_t)len, "%s", NAMED[k].names[ix]);
        }
    }
    if (bl >= 7 && !strcmp(base + bl - 7, "lfo_syn")) return snprintf(buf, (size_t)len, "%s", v < 25.f ? "MANUAL" : "HOST SYNC");
    return 0;
}
static int eng_get(void *p, const char *key, char *buf, int len) {
    Inst *I = (Inst *)p;
    if (!strcmp(key, "display_rev")) return snprintf(buf, (size_t)len, "%u", I->rev);
    {   /* Q-Link n: value, name and text of whichever control the visible page puts there */
        char base[32]; size_t kl = strlen(key); int suf = 0;
        if (kl > 6 && !strcmp(key + kl - 5, "_name")) suf = 2; else if (kl > 9 && !strcmp(key + kl - 8, "_display")) suf = 1;
        size_t bl = kl - (suf == 2 ? 5 : suf == 1 ? 8 : 0); if (bl < sizeof base) { memcpy(base, key, bl); base[bl] = 0; int qi = key_idx(base);
            if (qi >= E_ql1 && qi <= E_ql32) {
                int t = VIEW_QL[I->view][qi - E_ql1];
                if (suf == 2) return snprintf(buf, (size_t)len, "%s", t >= 0 ? PNAME[t] : "-");
                if (suf == 1) { if (t < 0) return snprintf(buf, (size_t)len, " "); char k2[96]; snprintf(k2, sizeof k2, "%s_display", PKEY[t]); int r = named_text(k2, I->P[t], buf, len); return r > 0 ? r : snprintf(buf, (size_t)len, "%d", (int)(I->P[t] + 0.5f)); }
                return snprintf(buf, (size_t)len, "%g", t >= 0 ? (double)(I->P[t] * 0.01f) : 0.0);
            }
            if (!suf && qi >= E_pg_0 && qi <= E_pg_0 + N_PAGES) return snprintf(buf, (size_t)len, "%d", I->view == qi - E_pg_0);
        } }
    { size_t kl = strlen(key); if (kl > 9 && !strcmp(key + kl - 8, "_display")) { char b2[96]; if (kl - 8 < sizeof b2) { memcpy(b2, key, kl - 8); b2[kl - 8] = 0; int j = key_idx(b2); if (j >= 0) return named_text(key, I->P[j], buf, len); } return 0; } }
    {   /* preset list: row text, row highlight, status line */
        size_t kl = strlen(key);
        if (!strncmp(key, "pm_", 3) && kl == 7 && !strcmp(key + 4, "_on")) { int r = key[3] - '1'; return snprintf(buf, (size_t)len, "%d", r >= 0 && r < 8 && I->pm_page * 8 + r == preset_index(I)); }
        int pi = key_idx(key);
        if (pi >= E_pm_1 && pi <= E_pm_8) { int n = I->pm_page * 8 + (pi - E_pm_1); if (n >= NSLOTS) return snprintf(buf, (size_t)len, " "); char t[64]; pm_name(I, n, t, sizeof t); return snprintf(buf, (size_t)len, "%s", t); }
        if (pi == E_pm_info) return snprintf(buf, (size_t)len, "%s", I->pm_msg[0] ? I->pm_msg : "Tap a preset, then LOAD");
    }
    int i = key_idx(key); if (i < 0 || len < 6) return 0;
    return snprintf(buf, (size_t)len, "%d", (int)(PBTN[i] ? 0.f : I->P[i] + 0.5f));
}
static void eng_midi(void *p, const uint8_t *m, int n) {
    Inst *I = (Inst *)p; if (n < 2) return;
    int st = m[0] & 0xF0;
    if (st == 0x90 && n >= 3) { if (m[2] == 0) I->pat[m[1] & 127] = 0.f; key_event(I, m[1], m[2], m[2] > 0); }
    else if (st == 0x80 && n >= 3) { I->pat[m[1] & 127] = 0.f; key_event(I, m[1], 0, 0); }
    else if (st == 0xE0 && n >= 3) I->bend = ((m[2] << 7 | m[1]) - 8192) / 8192.f;
    else if (st == 0xD0) I->at = m[1] / 127.f;
    else if (st == 0xA0 && n >= 3) I->pat[m[1] & 127] = m[2] / 127.f;
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
    static const float AR[8] = {0.5f, 0.6667f, 0.75f, 1.f, 1.5f, 2.f, 3.f, 4.f};   /* arp tempo = host tempo x ratio (Tempo/2 ... x4) */
    float host = I->bpm_ok ? (float)I->bpm : 120.f;
    float bpm = host * AR[(int)(lp(I, ly, L_ARP_TMP) * 7.99f)];
    float cl = (ly ? G(p7_split_ckb) : G(p7_split_cka));
    static const unsigned char GP[8] = {0xFF, 0xAA, 0xEE, 0xBB, 0x88, 0xDD, 0xA5, 0xC3}; /* gate patterns, 8 steps; CLOCK = every step */
    unsigned char pat = cl < 0.5f ? 0xFF : GP[(int)((cl - 0.5f) * 15.99f)];
    if (lp(I, ly, L_GATE_CG) >= 0.5f) {   /* ARP GATE page: GATE = the 8 step switches pass or mute each arpeggiator step; CLOCK = every step */
        unsigned char sp = 0; for (int i = 0; i < 8; i++) if (lp(I, ly, L_GATE_S1 + i) >= 0.5f) sp |= (unsigned char)(1 << i);
        sp = (unsigned char)((sp << 1) | (sp >> 7));   /* arp_step counts from 1: step 1 = the first note */
        pat = (unsigned char)(pat & sp);
    }
    double step = (60.0 / bpm) / 4.0 * SR;
    L->arp_t += frames;
    double gate = step * (0.1 + 0.85 * lp(I, ly, L_ARP_GAT));
    if (L->arp_note_on >= 0 && L->arp_t >= gate) { voice_off(I, ly, L->arp_note_on); L->arp_note_on = -1; }
    if (L->arp_t >= step) {
        L->arp_t -= step; if (L->arp_t > step) L->arp_t = 0;
        if (L->arp_note_on >= 0) { voice_off(I, ly, L->arp_note_on); L->arp_note_on = -1; }
        L->arp_step++;
        if (L->narp > 0 && (pat >> (L->arp_step & 7)) & 1) {
            int octs = 1 + (int)(lp(I, ly, L_ARP_OCT) * 3.99f);
            int total = L->narp * octs; if (total < 1) total = 1;
            /* manual: UP, DOWN, UP&DOWN, DOWN&UP, UP&DOWN 2 (end notes repeat), DOWN&UP 2 */
            int ty = (int)(lp(I, ly, L_ARP_TYP) * 5.99f), idx;
            int a = L->arp_i;
            if (ty == 0) { a = (a + 1) % total; idx = a; }
            else if (ty == 1) { a = (a + total - 1) % total; idx = a; }
            else {
                int per = ty >= 4 ? 2 * total : (total > 1 ? 2 * total - 2 : 1);   /* ping-pong period */
                a = (a + 1) % (per > 0 ? per : 1);
                int up = a < total ? a : (ty >= 4 ? 2 * total - 1 - a : 2 * total - 2 - a);
                idx = (ty == 3 || ty == 5) ? total - 1 - up : up;
            }
            L->arp_i = a;
            if (idx >= total) idx = 0;
            int srt[16]; memcpy(srt, L->arp_notes, sizeof(int) * (size_t)L->narp);
            for (int i = 1; i < L->narp; i++) { int k = srt[i], j = i - 1; while (j >= 0 && srt[j] > k) { srt[j + 1] = srt[j]; j--; } srt[j + 1] = k; }
            int note = srt[idx % L->narp] + 12 * (idx / L->narp);
            float vel = lp(I, ly, L_ARP_VEL) >= 0.5f ? (L->arp_vel > 0.f ? L->arp_vel : 0.8f) : 0.8f;   /* FIXED velocity or as played */
            if (note > 127) note = 127;
            L->arp_note_on = note;
            voice_on(I, ly, note, vel, 1, is_mono(I, ly));
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
    Layer *L = &I->L[ly]; LP q; load_lp(I, ly, &q); (void)sub_lfo;
    int lshape = (int)(q.lwav * 4.99f);
    float lrate = expmap(q.lrte, 0.05f, 20.f);
    if (lp(I, ly, L_LFO_SYN) >= 0.25f) { static const float LR[8] = {0.25f, 0.5f, 1.f, 2.f, 3.f, 4.f, 6.f, 8.f}; lrate = (I->bpm_ok ? (float)I->bpm : 120.f) / 60.f * LR[(int)(q.lrte * 7.99f)]; }   /* LFO SYNC: rate slider becomes the ratio to the host tempo */
    float vibrate = expmap(q.vvrt, 0.5f, 10.f);
    float shlo = q.vslo * 2.f - 1.f, shhi = q.vshi * 2.f - 1.f; if (shlo > shhi) { float t = shlo; shlo = shhi; shhi = t; }
    int dst = (int)(q.ldst * 5.99f), mde = (int)(q.lmde * 2.99f);   /* dst: 0 pitch 1 LPF 2 amp 3 PW 4 HPF 5 resonance; mde: 0 mod wheel, 1 A/R envelope, 2 direct */
    float bri_master = (G(p6_master_bri) - 0.5f) * 6.f;
    float rsn = (G(p6_master_rsn) - 0.5f) * 0.5f;
    /* touch ribbon: RIB picks the target (pitch / filter / resonance), RNG the range, SMD the sustain mode (I: held keys only, II: all sounding notes) */
    int rdst = (int)(G(p10_ribbon_rib) * 2.99f); float rrng = G(p10_ribbon_rng), roff = I->rib_off; int rsus2 = G(p10_sustain_smd) >= 0.5f;
    if (rdst == 2) rsn += roff * rrng * 0.5f;
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
            { float np = v->lfo_ph + lrate * n / SR; if (np >= 1.f) v->snh_val = shlo + rnd01(I) * (shhi - shlo); v->lfo_ph = wrap1(np); }
            float lfo = lfo_wave(v->lfo_ph, lshape, v->snh_val);
            float fadet = expmap(q.latt, 0.01f, 4.f);
            if (v->gate) v->lfo_gain = fminf(1.f, v->lfo_gain + (float)n / (fadet * SR));
            else v->lfo_gain = fmaxf(0.f, v->lfo_gain - (float)n / (expmap(q.lrel, 0.01f, 4.f) * SR));
            float srcdepth = mde == 0 ? fmaxf(I->mw, I->pat[v->note & 127]) : (mde == 1 ? v->lfo_gain : 1.f);
            float ld = q.vlfo * srcdepth;
            float lfo_to_p = 0, lfo_to_f = 0, lfo_to_a = 0, lfo_to_pw = 0, lfo_to_h = 0, lfo_to_r = 0;
            float lv = lfo * ld;
            if (dst == 0) lfo_to_p += lv * 2.f; else if (dst == 1) lfo_to_f += lv * 3.f; else if (dst == 2) lfo_to_a += lv; else if (dst == 3) lfo_to_pw += lv * 0.4f; else if (dst == 4) lfo_to_h += lv * 3.f; else lfo_to_r += lv * 0.3f;
            v->vib_ph = wrap1(v->vib_ph + vibrate * n / SR);
            float vib = sinf(2.f * PI_F * v->vib_ph) * q.vvib * 0.5f;
            /* pitch (semitones) */
            float rpit = (rdst == 0 && (rsus2 || v->gate)) ? roff * rrng * 24.f : 0.f;
            float gp = lp(I, ly, L_PERF_GTP) >= 0.5f ? roundf(v->pitch) : v->pitch;   /* glissando = stepped semitones, portamento = smooth */
            float pit = rpit + (ly ? I->mb_pb : I->mb_pa) + gp + 0.03f * sinf((float)(vi * 5 + ly * 2) * 1.7f) + q.shift + I->bend * bendsemi + lfo_to_p + vib + sub_vco;
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
            float vat = fmaxf(I->at, I->pat[v->note & 127]);   /* per-key aftertouch (channel pressure as fallback) */
            float lcut_oct = q.lm * 6.f * ef + bri * 6.f + kbri + bri_master + (v->note - 60.f) / 12.f * q.t_key
                           + vel * q.t_lve * 3.f + vat * q.t_laf * 3.f + lfo_to_f + sub_vcf + (rdst == 1 ? roff * rrng * 6.f : 0.f);
            float vdr = sinf((float)(vi * 7 + ly * 3) * 2.399f); /* per-voice-card trimmer drift */
            float lcut = expmap(q.lf, 40.f, 16000.f) * exp2f(lcut_oct + 0.04f * vdr);
            float hcut = expmap(q.hf, 20.f, 8000.f) * exp2f(lfo_to_h + q.hm * 3.f * eh + vel * q.t_hve * 3.f + vat * q.t_haf * 3.f);
            float res = clampf(q.lres + rsn + lfo_to_r, 0.f, 1.f);
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
    if (I->nav_cool > 0.f) I->nav_cool -= (float)frames / SR;
    { int rt = I->pad.touching; if (rt && !I->rib_was) I->rib_ref = I->rib_x; I->rib_was = rt; I->rib_off = rt ? (I->rib_x - I->rib_ref) * 0.2f : 0.f; }   /* zero point = where the finger first touched; springs back on release */
    /* modulation bar: pitch bar, centre = 0; destination A / B / both / ring-mod oscillator; DIRECT follows at once, RELEASE glides */
    { float bar = (G(p7_mod_bar_bar) - 0.5f) * 2.f; int mm = (int)(G(p7_mod_bar_mde) * 3.99f), md = (int)(G(p7_mod_bar_dst) * 3.99f);
      if (G(p7_mod_bar_bar) != I->mb_last) { I->mb_last = G(p7_mod_bar_bar); I->mb_idle = 0.f; } else I->mb_idle += (float)frames / SR;
      /* TOUCH modes: a finger on the bar bends, and once it has been let go (slider still for 0.3 s) the bar springs back to centre; HOLD modes stay where left */
      if ((mm == 0 || mm == 2) && I->mb_idle > 0.3f) bar = 0.f;
      float tgt = bar * 7.f; int rel = (mm == 1 || mm == 2);
      I->mb_s = rel ? I->mb_s + (tgt - I->mb_s) * (1.f - expf(-(float)frames / (0.35f * SR))) : tgt;
      I->mb_pa = (md == 0 || md == 2) ? I->mb_s : 0.f; I->mb_pb = (md == 1 || md == 2) ? I->mb_s : 0.f; I->mb_pr = md == 3 ? I->mb_s : 0.f; }
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
        static const float ER[8] = {0.25f, 0.3333f, 0.5f, 0.6667f, 1.f, 1.5f, 2.f, 3.f};   /* echo time = beats x ratio of the host tempo */
        float ebeat = 60.f / (I->bpm_ok ? (float)I->bpm : 120.f);
        int dl = (int)(ebeat * ER[(int)(lp(I, ly, L_ECHO_TIM) * 7.99f)] * SR); if (dl > 44090) dl = 44090; if (dl < 64) dl = 64;
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
            float f = 440.f * exp2f((I->rm_note - 69.f + I->mb_pr) / 12.f + pit + pen * I->rm_env + pvl * 2.f * (I->rm_vel - 0.5f));
            I->rm_ph = wrap1(I->rm_ph + f / SR);
            float inp = (1.f - ab) * A[s] + ab * B[s];   /* A_B: which line feeds the ring modulator */
            float car = sinf(2.f * PI_F * I->rm_ph) * ((1.f - ven) + ven * I->rm_env);   /* V ENV: envelope on the internal oscillator volume */
            ring[s] = inp * car * 3.f * rmlev;
        }
    }
    {
        float src = G(p8_crush_src); int bits = 2 + (int)(G(p8_crush_bit) * 14.f); float q = (float)(1 << (bits - 1));
        for (int s = 0; s < frames; s++) {
            float x = src < 0.34f ? A[s] : (src < 0.67f ? B[s] : A[s] + B[s]);   /* LINE A / LINE B / A & B */
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

static void eng_song_pos(void *p, double ppq, double bpm, int playing, int frames) { Inst *I = (Inst *)p; (void)ppq; (void)playing; (void)frames; if (bpm > 20.0 && bpm < 400.0) { I->bpm = bpm; I->bpm_ok = 1; } }
static const mpc_engine_t ENGINE = { eng_create, eng_destroy, eng_midi, eng_set, eng_get, eng_render, 0, eng_song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }

/* parameters moved by the other side (a Q-Link turned -> its fader; a fader touched -> its Q-Link): reported one key per call,
 * so the host updates just those controls, nothing is polled and no page is redrawn (wrapper/engine.h). */
int mpc_engine_changed(void *inst, char *key, int len) {
    Inst *I = (Inst *)inst;
    if (!I->dirty_n) return 0;
    for (int i = 0; i < N_PARAMS; i++) if (I->dirty[i]) { I->dirty[i] = 0; I->dirty_n--; snprintf(key, (size_t)len, "%s", PKEY[i]); return 1; }
    I->dirty_n = 0; return 0;
}
