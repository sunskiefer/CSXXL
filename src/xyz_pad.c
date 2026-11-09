/* XYZ's pad (L'Cronx). Parts of this file are VADER 0.5.0's overlay code (sunskiefer/VADER src/overlay.c, MIT licence), reused as is:
 * the log, finding MPC's DRM handle among the process's files, choosing a spare overlay plane, the plane's buffers, and reading
 * the display's picture to find the skin's two marker pixels (so the pad is shown only while XYZ's page is on screen). They
 * are marked "from VADER" below. New here: the touchscreen reader (evdev, multitouch), the pad drawing and the value mapping.
 *
 * What it does: the plugin runs inside MPC's process. This thread opens the touchscreen device, and takes a spare hardware
 * plane of the display at the pad's place on XYZ's page. A finger that goes down inside the pad moves the dot: its place
 * is turned into X and Y (-5 .. +5, matching the grid image), written into the engine, and reported to MPC as a parameter
 * change (so it can be automated and mapped to Q-Links). Everything it finds and does goes to "<plugin folder>/pad/xyz.log".
 * "<plugin folder>/pad/pad.off" switches the pad off (the X and Y knobs stay). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <linux/input.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#include "xyz_pad.h"

/* ---- from VADER (src/overlay.c 0.5.0), with the buffer size taken from the overlay (o->bw, o->bh) instead of constants ---- */
/* ------------------------------------------------------------------------------------------------ the log */
static void logf_(overlay_t *o, const char *fmt, ...) {
    if (!o->log_path[0]) return;
    FILE *f = fopen(o->log_path, "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%6ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------------------------------------ DRM */
static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* the DRM cards this process has open (MPC's own handles): up to 8 candidates; the one that lets us set a plane is
 * MPC's master handle */
static int find_card_fds(overlay_t *o, int *fds, int max) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) { logf_(o, "no /proc/self/fd: %s", strerror(errno)); return 0; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        char link[64], target[256];
        int fd = atoi(e->d_name);
        if (fd < 3) continue;
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t ln = readlink(link, target, sizeof target - 1);   /* XYZ: was "n", which shadowed the card counter below (VADER's overlay.c has the same slip) */
        if (ln <= 0) continue;
        target[ln] = 0;
        if (strncmp(target, "/dev/dri/card", 13)) continue;
        struct drm_mode_card_res r;
        memset(&r, 0, sizeof r);
        if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) { logf_(o, "fd %d %s: GETRESOURCES: %s", fd, target, strerror(errno)); continue; }
        logf_(o, "fd %d is %s: %u crtcs, %u connectors, %u fbs", fd, target, r.count_crtcs, r.count_connectors, r.count_fbs);
        fds[n++] = fd;
    }
    closedir(d);
    return n;
}

static int get_prop_u64(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val, uint32_t *prop_id,
                        uint64_t *vmin, uint64_t *vmax) {
    struct drm_mode_obj_get_properties gp;
    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj;
    gp.obj_type = type;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return -1;
    uint32_t n = gp.count_props;
    if (!n || n > 64) return -1;
    uint32_t ids[64];
    uint64_t vals[64];
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    gp.count_props = n;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return -1;
    for (uint32_t i = 0; i < n && i < gp.count_props; i++) {
        struct drm_mode_get_property p;
        memset(&p, 0, sizeof p);
        p.prop_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        if (strcmp(p.name, name)) continue;
        if (val) *val = vals[i];
        if (prop_id) *prop_id = ids[i];
        if ((vmin || vmax) && p.count_values >= 2 && (p.flags & DRM_MODE_PROP_RANGE)) {
            uint64_t range[2];
            p.values_ptr = (uintptr_t)range;
            p.count_values = 2;
            p.count_enum_blobs = 0;
            if (!xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) { if (vmin) *vmin = range[0]; if (vmax) *vmax = range[1]; }
        }
        return (int)(p.flags & DRM_MODE_PROP_IMMUTABLE ? 2 : 1);
    }
    return 0;
}

static void log_props(overlay_t *o, int fd, uint32_t obj, uint32_t type, const char *what) {
    struct drm_mode_obj_get_properties gp;
    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj;
    gp.obj_type = type;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return;
    uint32_t n = gp.count_props;
    if (!n || n > 64) return;
    uint32_t ids[64];
    uint64_t vals[64];
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    gp.count_props = n;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return;
    char line[512];
    int ln = snprintf(line, sizeof line, "  %s %u props:", what, obj);
    for (uint32_t i = 0; i < n && i < gp.count_props && ln < 480; i++) {
        struct drm_mode_get_property p;
        memset(&p, 0, sizeof p);
        p.prop_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        ln += snprintf(line + ln, sizeof line - (size_t)ln, " %s=%llu%s", p.name, (unsigned long long)vals[i],
                       (p.flags & DRM_MODE_PROP_IMMUTABLE) ? "(ro)" : "");
    }
    logf_(o, "%s", line);
}

static int create_buffer(overlay_t *o, ovl_buf_t *b) {
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width = o->bufd;
    cd.height = o->bufd;
    cd.bpp = 32;
    if (xioctl(o->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { logf_(o, "CREATE_DUMB: %s", strerror(errno)); return -1; }
    b->handle = cd.handle;
    b->pitch = cd.pitch;
    b->size = cd.size;
    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof fb);
    fb.width = o->bufd;
    fb.height = o->bufd;
    fb.pixel_format = o->format;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    if (xioctl(o->fd, DRM_IOCTL_MODE_ADDFB2, &fb)) { logf_(o, "ADDFB2 (%.4s): %s", (char *)&o->format, strerror(errno)); return -1; }
    b->fb_id = fb.fb_id;
    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (xioctl(o->fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { logf_(o, "MAP_DUMB: %s", strerror(errno)); return -1; }
    b->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, o->fd, (off_t)md.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; logf_(o, "mmap dumb: %s", strerror(errno)); return -1; }
    memset(b->map, 0, cd.size);
    logf_(o, "buffer: handle %u fb %u pitch %u size %llu", b->handle, b->fb_id, b->pitch, (unsigned long long)b->size);
    return 0;
}

static void destroy_buffer(overlay_t *o, ovl_buf_t *b) {
    if (b->map) munmap(b->map, b->size);
    if (b->fb_id) { uint32_t id = b->fb_id; xioctl(o->fd, DRM_IOCTL_MODE_RMFB, &id); }
    if (b->handle) { struct drm_mode_destroy_dumb dd = {.handle = b->handle}; xioctl(o->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd); }
    memset(b, 0, sizeof *b);
}

static int set_plane(overlay_t *o, uint32_t fb_id, int x, int y) {
    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof sp);
    sp.plane_id = o->plane_id;
    sp.crtc_id = o->crtc_id;
    sp.fb_id = fb_id;
    if (fb_id) {
        sp.crtc_x = x;
        sp.crtc_y = y;
        sp.crtc_w = o->pw;
        sp.crtc_h = o->ph;
        sp.src_x = 0;
        sp.src_y = 0;
        sp.src_w = o->pw << 16;
        sp.src_h = o->ph << 16;
    }
    return xioctl(o->fd, DRM_IOCTL_MODE_SETPLANE, &sp);
}

/* the display: MPC's CRTC and a spare plane on it that takes ARGB. 0: ready. */
static int drm_setup_fd(overlay_t *o);
static int drm_setup(overlay_t *o) {
    int fds[8], n = find_card_fds(o, fds, 8);
    if (!n) { logf_(o, "no DRM card among this process's files"); return -1; }
    for (int i = 0; i < n; i++) {
        o->fd = fds[i];
        logf_(o, "trying fd %d", o->fd);
        if (drm_setup_fd(o) == 0) {
            /* can we set a plane on it? (only the display's master can: a disable of our unused plane is harmless) */
            if (set_plane(o, 0, 0, 0) == 0) return 0;
            logf_(o, "fd %d: SETPLANE: %s (not the master handle?)", o->fd, strerror(errno));
        }
        for (int k = 0; k < 2; k++) destroy_buffer(o, &o->buf[k]);
    }
    o->fd = -1;
    return -1;
}
static int drm_setup_fd(overlay_t *o) {
    uint64_t cap = 1;
    struct drm_set_client_cap cc = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = cap};
    if (xioctl(o->fd, DRM_IOCTL_SET_CLIENT_CAP, &cc)) logf_(o, "universal planes: %s", strerror(errno));
    /* the active CRTC */
    struct drm_mode_card_res r;
    memset(&r, 0, sizeof r);
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) { logf_(o, "GETRESOURCES: %s", strerror(errno)); return -1; }
    uint32_t crtcs[16], ncrtc = r.count_crtcs > 16 ? 16 : r.count_crtcs;
    memset(&r, 0, sizeof r);
    r.crtc_id_ptr = (uintptr_t)crtcs;
    r.count_crtcs = ncrtc;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) { logf_(o, "GETRESOURCES 2: %s", strerror(errno)); return -1; }
    o->crtc_id = 0;
    for (uint32_t i = 0; i < ncrtc; i++) {
        struct drm_mode_crtc c;
        memset(&c, 0, sizeof c);
        c.crtc_id = crtcs[i];
        if (xioctl(o->fd, DRM_IOCTL_MODE_GETCRTC, &c)) continue;
        logf_(o, "crtc %u: fb %u, %ux%u at %u,%u, mode %s %ux%u", c.crtc_id, c.fb_id, c.mode.hdisplay, c.mode.vdisplay, c.x, c.y,
              c.mode.name, c.mode.hdisplay, c.mode.vdisplay);
        if (c.fb_id && !o->crtc_id) { o->crtc_id = c.crtc_id; o->crtc_index = (int)i; o->disp_w = c.mode.hdisplay; o->disp_h = c.mode.vdisplay; }
    }
    if (!o->crtc_id) { logf_(o, "no active crtc"); return -1; }
    /* the planes */
    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof pr);
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) { logf_(o, "GETPLANERESOURCES: %s", strerror(errno)); return -1; }
    uint32_t planes[32], np = pr.count_planes > 32 ? 32 : pr.count_planes;
    pr.plane_id_ptr = (uintptr_t)planes;
    pr.count_planes = np;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) { logf_(o, "GETPLANERESOURCES 2: %s", strerror(errno)); return -1; }
    logf_(o, "%u planes", np);
    o->plane_id = 0;
    int best_rank = -1;
    for (uint32_t i = 0; i < np; i++) {
        struct drm_mode_get_plane gp;
        memset(&gp, 0, sizeof gp);
        gp.plane_id = planes[i];
        uint32_t fmts[64];
        gp.format_type_ptr = (uintptr_t)fmts;
        gp.count_format_types = 64;
        if (xioctl(o->fd, DRM_IOCTL_MODE_GETPLANE, &gp)) { logf_(o, "plane %u: GETPLANE: %s", planes[i], strerror(errno)); continue; }
        uint64_t type = 99, zpos = 0, zmin = 0, zmax = 0;
        get_prop_u64(o->fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &type, NULL, NULL, NULL);
        int hasz = get_prop_u64(o->fd, planes[i], DRM_MODE_OBJECT_PLANE, "zpos", &zpos, NULL, &zmin, &zmax);
        int ar24 = 0, xr24 = 0;
        char fl[256];
        int fn = 0;
        for (uint32_t k = 0; k < gp.count_format_types && k < 64; k++) {
            if (fmts[k] == DRM_FORMAT_ARGB8888) ar24 = 1;
            if (fmts[k] == DRM_FORMAT_XRGB8888) xr24 = 1;
            if (fn < 240) fn += snprintf(fl + fn, sizeof fl - (size_t)fn, " %.4s", (char *)&fmts[k]);
        }
        logf_(o, "plane %u: type %s, crtcs 0x%x, in use: crtc %u fb %u, zpos %llu [%llu..%llu]%s, formats:%s",
              planes[i], type == 1 ? "primary" : type == 2 ? "cursor" : type == 0 ? "overlay" : "?", gp.possible_crtcs,
              gp.crtc_id, gp.fb_id, (unsigned long long)zpos, (unsigned long long)zmin, (unsigned long long)zmax,
              hasz == 2 ? "(ro)" : hasz == 1 ? "" : "(none)", fl);
        log_props(o, o->fd, planes[i], DRM_MODE_OBJECT_PLANE, "plane");
        if (!(gp.possible_crtcs & (1u << o->crtc_index))) continue;
        if (gp.fb_id) continue;              /* MPC's own */
        if (type == 1) continue;             /* a primary plane */
        if (!ar24 && !xr24) continue;
        int rank = (type == 0 ? 2 : 1) + (ar24 ? 4 : 0);
        if (rank > best_rank) { best_rank = rank; o->plane_id = planes[i]; o->format = ar24 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888; o->zpos_max = zmax; o->has_zpos = hasz == 1; }
    }
    if (!o->plane_id) { logf_(o, "no spare plane on crtc %u takes ARGB/XRGB", o->crtc_id); return -1; }
    logf_(o, "using plane %u (%.4s) on crtc %u", o->plane_id, (char *)&o->format, o->crtc_id);
    if (o->has_zpos && o->zpos_max) {
        uint32_t pid = 0;
        get_prop_u64(o->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "zpos", NULL, &pid, NULL, NULL);
        struct drm_mode_obj_set_property sp = {.value = o->zpos_max, .prop_id = pid, .obj_id = o->plane_id, .obj_type = DRM_MODE_OBJECT_PLANE};
        if (xioctl(o->fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp)) logf_(o, "zpos %llu: %s", (unsigned long long)o->zpos_max, strerror(errno));
        else logf_(o, "zpos set to %llu", (unsigned long long)o->zpos_max);
    }
    if (create_buffer(o, &o->buf[0])) return -1;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ where the box is */
/* MPC's picture, read back from the display (read-only), to find the skin's marker pixels. Mapped for one look and
 * released at once: a mapping kept would pin MPC's buffer, and MPC needs its buffers back when it rebuilds a page
 * (0.5.1 froze the display on leaving the MAIN page). */
typedef struct { uint32_t fb_id; uint8_t *map; uint8_t *base; size_t len; uint32_t pitch, w, h, fmt; } fbmap_t;

static int scanout_open(overlay_t *o, fbmap_t *m) {
    memset(m, 0, sizeof *m);
    struct drm_mode_crtc c;
    memset(&c, 0, sizeof c);
    c.crtc_id = o->crtc_id;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id) return -1;
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.fb_id = c.fb_id;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETFB2, &f)) {
        /* an older kernel: the legacy GETFB (a handle, the pitch, bpp and depth; the format follows) */
        struct drm_mode_fb_cmd f1;
        memset(&f1, 0, sizeof f1);
        f1.fb_id = c.fb_id;
        if (xioctl(o->fd, DRM_IOCTL_MODE_GETFB, &f1)) { if (!o->fb_err_logged++) logf_(o, "GETFB2 and GETFB %u: %s", c.fb_id, strerror(errno)); return -1; }
        memset(&f, 0, sizeof f);
        f.fb_id = f1.fb_id;
        f.width = f1.width;
        f.height = f1.height;
        f.pitches[0] = f1.pitch;
        f.handles[0] = f1.handle;
        f.pixel_format = f1.bpp == 32 ? DRM_FORMAT_XRGB8888 : f1.bpp == 24 ? DRM_FORMAT_RGB888 : f1.bpp == 16 ? DRM_FORMAT_RGB565 : 0;
    }
    if (!f.handles[0]) { if (!o->fb_err_logged++) logf_(o, "GETFB %u: no handle (not the master handle, and not root?)", c.fb_id); return -1; }
    struct drm_prime_handle p = {.handle = f.handles[0], .flags = DRM_CLOEXEC};
    int rc = xioctl(o->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p);
    struct drm_gem_close gc = {.handle = f.handles[0]};
    xioctl(o->fd, DRM_IOCTL_GEM_CLOSE, &gc);   /* GETFB gave us a handle of our own: drop it at once */
    if (rc) { if (!o->fb_err_logged++) logf_(o, "PRIME export of fb %u: %s", c.fb_id, strerror(errno)); return -1; }
    size_t len = (size_t)f.pitches[0] * f.height + f.offsets[0];
    uint8_t *mm = mmap(NULL, len, PROT_READ, MAP_SHARED, p.fd, 0);
    close(p.fd);
    if (mm == MAP_FAILED) { if (!o->fb_err_logged++) logf_(o, "mmap of fb %u: %s", c.fb_id, strerror(errno)); return -1; }
    m->fb_id = c.fb_id;
    m->base = mm;
    m->map = mm + f.offsets[0];
    m->len = len;
    m->pitch = f.pitches[0];
    m->w = f.width;
    m->h = f.height;
    m->fmt = f.pixel_format;
    if (!o->fb_logged) { o->fb_logged = 1; logf_(o, "MPC's picture: fb %u, %ux%u %.4s, pitch %u", c.fb_id, f.width, f.height, (char *)&f.pixel_format, f.pitches[0]); }
    return 0;
}

static void scanout_close(fbmap_t *m) {
    if (m->base) munmap(m->base, m->len);
    memset(m, 0, sizeof *m);
}

/* the markers (tools/vader_skin.py glass()): 2 x 2 pixels at the tiles' top-left and bottom-right, in these colours */
static const uint8_t MARK_TL[4][3] = {{23, 7, 91}, {91, 7, 23}, {7, 91, 23}, {23, 91, 7}};
static const uint8_t MARK_BR[4][3] = {{91, 23, 7}, {7, 23, 91}, {61, 7, 61}, {7, 61, 61}};

static inline int px_rgb(const fbmap_t *m, int x, int y, uint8_t *rgb) {
    if (x < 0 || y < 0 || x >= (int)m->w || y >= (int)m->h) return 0;
    const uint8_t *p = m->map + (size_t)y * m->pitch;
    switch (m->fmt) {
    case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888: p += x * 4; rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; return 1;
    case DRM_FORMAT_XBGR8888: case DRM_FORMAT_ABGR8888: p += x * 4; rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; return 1;
    case DRM_FORMAT_RGB888: p += x * 3; rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; return 1;
    case DRM_FORMAT_BGR888: p += x * 3; rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; return 1;
    case DRM_FORMAT_RGB565: { p += x * 2; uint16_t v = (uint16_t)(p[0] | p[1] << 8); rgb[0] = (uint8_t)((v >> 11) << 3); rgb[1] = (uint8_t)(((v >> 5) & 63) << 2); rgb[2] = (uint8_t)((v & 31) << 3); return 1; }
    default: return 0;
    }
}
static inline int near_(const uint8_t *a, const uint8_t *b) {
    return abs(a[0] - b[0]) <= 8 && abs(a[1] - b[1]) <= 8 && abs(a[2] - b[2]) <= 8;
}
/* (VADER's unrotated marker search, marker_at / find_box / locate, is replaced below by a rotation-aware one) */

/* ================================================================================================ new in XYZ */

/* ---- the grid image (pad/grid.png) and what its lines mean: measured from the file with code (tools/measure_grid.py):
 * 747 x 747, the axes cross at (359.5, 388.5), 60 pixels per unit, so the outer gridlines are x, y = -5 and +5 exactly. */
#define GRID_N 747
#define GRID_OX 359.5f
#define GRID_OY 388.5f
#define GRID_U 60.0f
#define VAL_MAX 5.0f

static inline float clampf_(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* pad box (page pixels) from <dir>/layout.txt: a line "pad x y w h". Page is 1280 x 628. */
static void load_layout_(overlay_t *o) {
    o->pad_x = 340; o->pad_y = 14; o->bw = 600; o->bh = 600;
    char path[800];
    snprintf(path, sizeof path, "%s/layout.txt", o->dir);
    FILE *f = fopen(path, "r");
    if (!f) { logf_(o, "no layout.txt: the pad is at its default place %d,%d %dx%d", o->pad_x, o->pad_y, o->bw, o->bh); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int x, y, w, h;
        if (line[0] == '#') continue;
        if (sscanf(line, " pad %d %d %d %d", &x, &y, &w, &h) == 4) {
            if (w < 64) w = 64; if (h < 64) h = 64;
            if (w > OVL_TILES_W) w = OVL_TILES_W; if (h > OVL_TILES_H) h = OVL_TILES_H;
            if (x < 0) x = 0; if (y < 0) y = 0;
            if (x + w > OVL_TILES_W) x = OVL_TILES_W - w; if (y + h > OVL_TILES_H) y = OVL_TILES_H - h;
            o->pad_x = x; o->pad_y = y; o->bw = w; o->bh = h;
        }
    }
    fclose(f);
    logf_(o, "layout.txt: pad at %d,%d, %dx%d (page pixels)", o->pad_x, o->pad_y, o->bw, o->bh);
}

static void load_layout(overlay_t *o) {
    load_layout_(o);
    o->bufd = o->bw > o->bh ? o->bw : o->bh;   /* the plane's buffers are square, big enough for the pad turned either way */
}

/* ---- where MPC's landscape screen lies in the display: found from the page's marker pixels, nothing assumed.
 * The Force's panel is portrait (800 x 1280 in the 0.0.1 log) and MPC draws its 1280 x 800 landscape screen turned into it. */
#define PAGE_W OVL_TILES_W
#define PAGE_H OVL_TILES_H

static inline void ui_to_fb(int rot, int lw, int lh, int u, int v, int *x, int *y) {
    switch (rot) {
    case 0: *x = u; *y = v; break;
    case 1: *x = lh - 1 - v; *y = u; break;
    case 2: *x = lw - 1 - u; *y = lh - 1 - v; break;
    default: *x = v; *y = lw - 1 - u; break;
    }
}
static inline void fb_to_ui_f(int rot, int lw, int lh, float x, float y, float *u, float *v) {   /* the same turn, backwards, in continuous coordinates */
    switch (rot) {
    case 0: *u = x; *v = y; break;
    case 1: *u = y; *v = (float)lh - x; break;
    case 2: *u = (float)lw - x; *v = (float)lh - y; break;
    default: *u = (float)lw - y; *v = x; break;
    }
}

/* a 2 x 2 marker block whose top-left is at (u, v) on the landscape screen, looked for in the display's picture */
static int marker_rot(const fbmap_t *m, int rot, int lw, int lh, int u, int v, const uint8_t mk[4][3]) {
    for (int k = 0; k < 4; k++) {
        int x, y;
        uint8_t c[3];
        ui_to_fb(rot, lw, lh, u + (k & 1), v + (k >> 1), &x, &y);
        if (!px_rgb(m, x, y, c) || !near_(c, mk[k])) return 0;
    }
    return 1;
}
static int page_here(const fbmap_t *m, int rot, int lw, int lh, int ox, int oy) {
    return marker_rot(m, rot, lw, lh, ox, oy, MARK_TL) && marker_rot(m, rot, lw, lh, ox + PAGE_W - 2, oy + PAGE_H - 2, MARK_BR);
}
/* all four turns, the page anywhere (2 pixels of slack each way): a few thousand pixel reads */
static int find_page(const fbmap_t *m, int *rot, int *lw, int *lh, int *ox, int *oy) {
    for (int r = 0; r < 4; r++) {
        int LW = (r & 1) ? (int)m->h : (int)m->w, LH = (r & 1) ? (int)m->w : (int)m->h;
        if (LW < PAGE_W - 4 || LH < PAGE_H - 4) continue;
        for (int y0 = -2; y0 <= LH - PAGE_H + 2; y0++)
            for (int x0 = -2; x0 <= LW - PAGE_W + 2; x0++)
                if (page_here(m, r, LW, LH, x0, y0)) { *rot = r; *lw = LW; *lh = LH; *ox = x0; *oy = y0; return 1; }
    }
    return 0;
}

/* the pad's rectangle on the display, from the page's place and the turn */
static int compute_plane(overlay_t *o) {
    int u0 = o->box_x + o->pad_x, v0 = o->box_y + o->pad_y;
    int xa, ya, xb, yb;
    ui_to_fb(o->rot, o->lw, o->lh, u0, v0, &xa, &ya);
    ui_to_fb(o->rot, o->lw, o->lh, u0 + o->bw - 1, v0 + o->bh - 1, &xb, &yb);
    o->pl_x = xa < xb ? xa : xb;
    o->pl_y = ya < yb ? ya : yb;
    o->pw = abs(xa - xb) + 1;
    o->ph = abs(ya - yb) + 1;
    if ((long)o->pw * o->ph != (long)o->bw * o->bh || o->pw > o->bufd || o->ph > o->bufd || o->pl_x < 0 || o->pl_y < 0 ||
        o->pl_x + o->pw > (int)o->fb_w || o->pl_y + o->ph > (int)o->fb_h) {
        logf_(o, "the pad does not fit on the display: plane %d,%d %dx%d on %ux%u", o->pl_x, o->pl_y, o->pw, o->ph, o->fb_w, o->fb_h);
        return -1;
    }
    o->geom++;
    return 0;
}

/* 1: XYZ's page is on screen; 0: not (another page, a list over it). full: search the whole picture when it isn't where it was. */
static int locate(overlay_t *o, int full) {
    fbmap_t m;
    if (scanout_open(o, &m)) { o->fmt_unknown = o->fb_err_logged > 0; return 0; }
    o->fmt_unknown = 0;
    o->fb_w = m.w; o->fb_h = m.h;
    int on = 0;
    if (o->have_page) on = page_here(&m, o->rot, o->lw, o->lh, o->box_x, o->box_y);
    if (!on && full) {
        int r, lw, lh, ox, oy;
        if (find_page(&m, &r, &lw, &lh, &ox, &oy)) {
            int moved = !o->have_page || r != o->rot || lw != o->lw || ox != o->box_x || oy != o->box_y;
            o->rot = r; o->lw = lw; o->lh = lh; o->box_x = ox; o->box_y = oy;
            if (compute_plane(o) == 0) {
                o->have_page = 1; on = 1;
                if (moved) logf_(o, "XYZ's page found: display %ux%u %.4s, MPC's %dx%d screen turned %s, page at %d,%d on it; the pad's plane is %d,%d %dx%d on the display",
                                 m.w, m.h, (char *)&m.fmt, lw, lh, r == 0 ? "not at all" : r == 1 ? "90 degrees clockwise" : r == 2 ? "180 degrees" : "90 degrees anticlockwise",
                                 ox, oy, o->pl_x, o->pl_y, o->pw, o->ph);
            } else o->have_page = 0;
        }
    }
    scanout_close(&m);
    return on;
}

static uint8_t *g_base;   /* the grid image scaled to the pad's size, premultiplied ARGB bytes (B, G, R, A) */

static uint8_t *read_file(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 16 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = len;
    return b;
}

static int load_grid(overlay_t *o) {
    int w = o->bw, h = o->bh;
    g_base = malloc((size_t)w * h * 4);
    if (!g_base) return -1;
    /* CSXXL ribbon: a black stripe with rounded ends, a thin grey rim and a faint centre tick */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t *p = g_base + ((size_t)y * w + x) * 4;
        float r = h * 0.5f, cx = x + 0.5f, cy = y + 0.5f, dx = cx < r ? r - cx : (cx > w - r ? cx - (w - r) : 0.f), d = sqrtf(dx * dx + (cy - r) * (cy - r));
        float in = clampf_(r - d + 0.5f, 0, 1), rim = clampf_(r - 2.f - d + 0.5f, 0, 1);
        float c = 14.f + (1.f - rim) * 60.f; if (fabsf(cx - w * 0.5f) < 1.5f && fabsf(cy - r) < r * 0.5f) c += 60.f;
        p[0] = p[1] = p[2] = (uint8_t)(c * in); p[3] = (uint8_t)(255 * in);
    }
    return 0;
    char path[800];
    snprintf(path, sizeof path, "%s/grid.png", o->dir);
    long n = 0;
    uint8_t *file = read_file(path, &n);
    int iw = 0, ih = 0, ch = 0;
    uint8_t *img = file ? stbi_load_from_memory(file, (int)n, &iw, &ih, &ch, 4) : NULL;
    free(file);
    if (!img || iw != GRID_N || ih != GRID_N) {
        logf_(o, "grid.png %s (%dx%d): drawing a plain grid instead", img ? "is not 747x747" : "not readable", iw, ih);
        free(img);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *p = g_base + ((size_t)y * w + x) * 4;
                float ix = (float)x * GRID_N / w, iy = (float)y * GRID_N / h;
                float ux = (ix - GRID_OX) / GRID_U, uy = (iy - GRID_OY) / GRID_U;
                int axis = fabsf(ux) < 0.04f || fabsf(uy) < 0.04f, grid = fabsf(ux - roundf(ux)) < 0.02f || fabsf(uy - roundf(uy)) < 0.02f;
                uint8_t c = axis ? 200 : grid ? 90 : 20;
                p[0] = p[1] = p[2] = c; p[3] = 255;
            }
        return 0;
    }
    /* premultiply, then scale bilinearly (the image is 747 and the pad usually smaller) */
    float *pm = malloc((size_t)iw * ih * 4 * sizeof(float));
    if (!pm) { stbi_image_free(img); return -1; }
    for (int i = 0; i < iw * ih; i++) {
        float a = img[i * 4 + 3] / 255.0f;
        pm[i * 4 + 0] = img[i * 4 + 2] * a;   /* B */
        pm[i * 4 + 1] = img[i * 4 + 1] * a;   /* G */
        pm[i * 4 + 2] = img[i * 4 + 0] * a;   /* R */
        pm[i * 4 + 3] = 255.0f * a;
    }
    stbi_image_free(img);
    float sx = (float)iw / w, sy = (float)ih / h;
    for (int y = 0; y < h; y++) {
        float fy = ((float)y + 0.5f) * sy - 0.5f;
        int y0 = (int)floorf(fy); float ty = fy - y0;
        int y1 = y0 + 1; if (y0 < 0) y0 = 0; if (y1 > ih - 1) y1 = ih - 1; if (y0 > ih - 1) y0 = ih - 1;
        for (int x = 0; x < w; x++) {
            float fx = ((float)x + 0.5f) * sx - 0.5f;
            int x0 = (int)floorf(fx); float tx = fx - x0;
            int x1 = x0 + 1; if (x0 < 0) x0 = 0; if (x1 > iw - 1) x1 = iw - 1; if (x0 > iw - 1) x0 = iw - 1;
            uint8_t *p = g_base + ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; c++) {
                float v = (pm[((size_t)y0 * iw + x0) * 4 + c] * (1 - tx) + pm[((size_t)y0 * iw + x1) * 4 + c] * tx) * (1 - ty) +
                          (pm[((size_t)y1 * iw + x0) * 4 + c] * (1 - tx) + pm[((size_t)y1 * iw + x1) * 4 + c] * tx) * ty;
                p[c] = (uint8_t)clampf_(v + 0.5f, 0, 255);
            }
        }
    }
    free(pm);
    logf_(o, "grid.png: %dx%d scaled to %dx%d", iw, ih, w, h);
    return 0;
}

/* a place in the pad (pixels, 0..bw, 0..bh) <-> X and Y. Outside the grid's outer lines the value stays at +-5. */
static void pos_to_val(const overlay_t *o, float px, float py, float *x, float *y) {
    /* CSXXL ribbon: X runs -5..+5 along the stripe, Y unused */
    *x = clampf_((px / (float)o->bw - 0.5f) * 2.f * VAL_MAX, -VAL_MAX, VAL_MAX); *y = 0.f; return;
    float ix = px * GRID_N / o->bw, iy = py * GRID_N / o->bh;
    *x = clampf_((ix - GRID_OX) / GRID_U, -VAL_MAX, VAL_MAX);
    *y = clampf_(-(iy - GRID_OY) / GRID_U, -VAL_MAX, VAL_MAX);
}
static void val_to_pos(const overlay_t *o, float x, float y, float *px, float *py) {
    (void)y; *px = (x / (2.f * VAL_MAX) + 0.5f) * (float)o->bw; *py = (float)o->bh * 0.5f; return;
    *px = (GRID_OX + x * GRID_U) * o->bw / GRID_N;
    *py = (GRID_OY - y * GRID_U) * o->bh / GRID_N;
}

/* ---- drawing: the picture, a faint crosshair through the dot, and the dot (premultiplied ARGB, blended over the picture) */
static inline void blend_px(uint8_t *p, float r, float g, float b, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    float ia = 1 - a;
    p[0] = (uint8_t)clampf_(b * a + p[0] * ia + 0.5f, 0, 255);
    p[1] = (uint8_t)clampf_(g * a + p[1] * ia + 0.5f, 0, 255);
    p[2] = (uint8_t)clampf_(r * a + p[2] * ia + 0.5f, 0, 255);
    p[3] = (uint8_t)clampf_(255 * a + p[3] * ia + 0.5f, 0, 255);
}

static void render_pad(const overlay_t *o, uint8_t *out, float x, float y, int touching) {
    int w = o->bw, h = o->bh;
    memcpy(out, g_base, (size_t)w * h * 4);
    float cx, cy; val_to_pos(o, x, y, &cx, &cy);
    if (!touching) return;                                        /* at rest the ribbon is just the black stripe */
    for (int j = 2; j < h - 2; j++) for (int d = -3; d <= 3; d++) {
        int xx = (int)floorf(cx) + d; if (xx < 0 || xx >= w) continue;
        uint8_t *p = out + ((size_t)j * w + xx) * 4;
        if (p[3] > 200) blend_px(p, 235, 235, 232, 0.85f - 0.2f * (float)abs(d));
    }
}

/* the landscape picture of the pad (bw x bh) turned into the plane's buffer (pw x ph): a fixed step per pixel along each side */
static void rotate_pad(const overlay_t *o, const uint8_t *src, uint8_t *dst) {
    int u0 = o->box_x + o->pad_x, v0 = o->box_y + o->pad_y;
    int x0, y0, x1, y1, x2, y2;
    ui_to_fb(o->rot, o->lw, o->lh, u0, v0, &x0, &y0);
    ui_to_fb(o->rot, o->lw, o->lh, u0 + 1, v0, &x1, &y1);
    ui_to_fb(o->rot, o->lw, o->lh, u0, v0 + 1, &x2, &y2);
    long sa = (long)(y1 - y0) * o->pw + (x1 - x0), sb = (long)(y2 - y0) * o->pw + (x2 - x0);
    long base = (long)(y0 - o->pl_y) * o->pw + (x0 - o->pl_x);
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (int b = 0; b < o->bh; b++) {
        long row = base + (long)b * sb;
        for (int a = 0; a < o->bw; a++) d[row + (long)a * sa] = s[(size_t)b * o->bw + a];
    }
}

int pad_render_test(const char *data_dir, float x, float y, int touching, uint8_t *out, int *bw, int *bh) {
    overlay_t o;
    memset(&o, 0, sizeof o);
    snprintf(o.dir, sizeof o.dir, "%s", data_dir);
    load_layout(&o);
    if (load_grid(&o)) return -1;
    if (out) render_pad(&o, out, x, y, touching);
    *bw = o.bw; *bh = o.bh;
    free(g_base); g_base = NULL;
    return 0;
}

/* ---- the touchscreen (evdev). The kernel gives positions in the display's orientation (the touch chip's own transform is
 * applied there); here they are only scaled to the display's size. Multitouch slot protocol; one finger drives the pad: the
 * first one that goes down inside it. */
typedef struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } ev32_t;
typedef struct {
    int fd, mt, minx, maxx, miny, maxy, slot, claimed;
    struct { int down, fresh, x, y; } s[10];
    long nevents, nclaims, nlost;
    int size_logged;
} touch_t;

#define TBIT(a, n) (((a)[(n) / 8] >> ((n) % 8)) & 1)

static int touch_open(overlay_t *o, touch_t *t) {
    memset(t, 0, sizeof *t);
    t->fd = -1; t->claimed = -1;
    char cand[40][64];
    int nc = 0;
    snprintf(cand[nc++], 64, "/dev/input/touchscreen0");
    for (int i = 0; i < 32 && nc < 40; i++) snprintf(cand[nc++], 64, "/dev/input/event%d", i);
    for (int i = 0; i < nc; i++) {
        int fd = open(cand[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (i == 0) logf_(o, "touch: %s: %s", cand[i], strerror(errno)); continue; }
        char name[96] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        uint8_t abs_[(ABS_MAX + 8) / 8 + 1];
        memset(abs_, 0, sizeof abs_);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_), abs_);
        int mt = TBIT(abs_, ABS_MT_POSITION_X) && TBIT(abs_, ABS_MT_POSITION_Y);
        int st = TBIT(abs_, ABS_X) && TBIT(abs_, ABS_Y);
        logf_(o, "touch: %s is \"%s\": multitouch %d, single-touch axes %d", cand[i], name, mt, st);
        if (!mt && !st) { close(fd); continue; }
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ax) || ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &ay)) { close(fd); continue; }
        if (ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) { close(fd); continue; }
        t->fd = fd; t->mt = mt;
        t->minx = ax.minimum; t->maxx = ax.maximum; t->miny = ay.minimum; t->maxy = ay.maximum;
        logf_(o, "touch: using %s: x %d..%d, y %d..%d (display %ux%u)", cand[i], t->minx, t->maxx, t->miny, t->maxy, o->disp_w, o->disp_h);
        return 0;
    }
    logf_(o, "touch: no touchscreen found: the pad shows the values but can't be dragged (use the X and Y knobs)");
    return -1;
}

static int pad_set(overlay_t *o, float x, float y) {
    int ch = 0;
    if (x != *o->vx) { *o->vx = x; ch |= 1; }
    if (y != *o->vy) { *o->vy = y; ch |= 2; }
    if (ch) __atomic_or_fetch(o->changed, (unsigned)ch, __ATOMIC_RELEASE);
    return ch;
}

static void touch_sync(overlay_t *o, touch_t *t) {
    int shown = o->on_screen && !o->fmt_unknown && o->have_page;
    float fw = (float)(o->fb_w ? o->fb_w : o->disp_w), fh = (float)(o->fb_h ? o->fb_h : o->disp_h);
    for (int i = 0; i < 10; i++) {
        if (!t->s[i].down) { if (t->claimed == i) { t->claimed = -1; o->touching = 0; pad_set(o, 0.f, 0.f); logf_(o, "finger up"); } t->s[i].fresh = 0; continue; }
        /* the touch chip's own range -> display pixels (the chip's x, y lie as the display's do), -> MPC's landscape screen -> the pad */
        float fx = ((float)(t->s[i].x - t->minx) / (float)(t->maxx - t->minx)) * fw;
        float fy = ((float)(t->s[i].y - t->miny) / (float)(t->maxy - t->miny)) * fh;
        float u = 0, v = 0, lx = -1, ly = -1;
        if (o->have_page) {
            fb_to_ui_f(o->rot, o->lw, o->lh, fx, fy, &u, &v);
            lx = u - (float)(o->box_x + o->pad_x);
            ly = v - (float)(o->box_y + o->pad_y);
        }
        if (t->claimed < 0 && t->s[i].fresh) {
            t->s[i].fresh = 0;
            int inside = lx >= 0 && lx < (float)o->bw && ly >= 0 && ly < (float)o->bh;
            if (t->nevents < 60 || t->nclaims < 5)
                logf_(o, "touch down: raw %d,%d -> display %.0f,%.0f -> screen %.0f,%.0f -> in the pad %.0f,%.0f (pad is %dx%d): %s, page shown %d",
                      t->s[i].x, t->s[i].y, fx, fy, u, v, lx, ly, o->bw, o->bh, inside ? "INSIDE" : "outside", shown);
            if (shown && inside) { t->claimed = i; t->nclaims++; { float x0, y0; pos_to_val(o, lx, ly, &x0, &y0); pad_set(o, x0, y0); } o->touching = 1; }
        }
        if (t->claimed == i) {
            float x, y;
            pos_to_val(o, lx, ly, &x, &y);
            pad_set(o, x, y);
        }
    }
}

static void touch_read(overlay_t *o, touch_t *t) {
    ev32_t ev[64];
    for (;;) {
        ssize_t n = read(t->fd, ev, sizeof ev);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) return; if (errno == ENODEV) { logf_(o, "touch: device gone"); close(t->fd); t->fd = -1; } return; }
        if (n == 0) return;
        if (n % (ssize_t)sizeof(ev32_t) && !t->size_logged++) logf_(o, "touch: read %zd bytes, not a multiple of %zu", n, sizeof(ev32_t));
        int cnt = (int)(n / (ssize_t)sizeof(ev32_t));
        for (int k = 0; k < cnt; k++) {
            t->nevents++;
            if (ev[k].type == EV_ABS) {
                int c = ev[k].code, v = ev[k].value;
                if (t->mt) {
                    if (c == ABS_MT_SLOT) t->slot = v < 0 ? 0 : v > 9 ? 9 : v;
                    else if (c == ABS_MT_TRACKING_ID) { if (v < 0) t->s[t->slot].down = 0; else { t->s[t->slot].down = 1; t->s[t->slot].fresh = 1; } }
                    else if (c == ABS_MT_POSITION_X) t->s[t->slot].x = v;
                    else if (c == ABS_MT_POSITION_Y) t->s[t->slot].y = v;
                } else {
                    if (c == ABS_X) t->s[0].x = v; else if (c == ABS_Y) t->s[0].y = v;
                }
            } else if (ev[k].type == EV_KEY && ev[k].code == BTN_TOUCH && !t->mt) {
                t->s[0].down = ev[k].value != 0;
                if (ev[k].value) t->s[0].fresh = 1;
            } else if (ev[k].type == EV_SYN && ev[k].code == SYN_REPORT) {
                touch_sync(o, t);
            }
        }
    }
}

/* ---- the thread */
static pthread_mutex_t g_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static overlay_t *g_owner;   /* one XYZ instance in this process has the pad; the others keep their knobs */

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

static void *pad_thread(void *p) {
    overlay_t *o = p;
    logf_(o, "XYZ pad starting (Pseudo X/Y pad)");
    for (;;) {
        pthread_mutex_lock(&g_owner_lock);
        if (!g_owner) g_owner = o;
        int mine = g_owner == o;
        pthread_mutex_unlock(&g_owner_lock);
        if (mine) break;
        if (o->stop) return NULL;
        if (!o->waited_logged) { o->waited_logged = 1; logf_(o, "another XYZ instance has the pad: waiting"); }
        for (int i = 0; i < 20 && !o->stop; i++) usleep(100000);
    }
    uint8_t *land = NULL, *rotf = NULL, *prev = NULL;
    touch_t t;
    t.fd = -1;
    int tries = 0;
    while (!o->stop && drm_setup(o)) {
        for (int i = 0; i < 2; i++) destroy_buffer(o, &o->buf[i]);
        if (++tries >= 3) { logf_(o, "giving up: no pad (the X and Y knobs stay)"); goto done; }
        logf_(o, "retry %d in 3 s", tries);
        for (int i = 0; i < 30 && !o->stop; i++) usleep(100000);
    }
    if (o->stop) goto done;
    if (load_grid(o)) { logf_(o, "no memory for the grid"); goto done; }
    land = malloc((size_t)o->bw * o->bh * 4);
    rotf = calloc((size_t)o->bufd * o->bufd, 4);
    prev = calloc((size_t)o->bufd * o->bufd, 4);
    if (!land || !rotf || !prev) { logf_(o, "no memory"); goto done; }
    touch_open(o, &t);
    __atomic_store_n(&o->live, 1, __ATOMIC_RELEASE);
    logf_(o, "live");
    int shown = 0, first = 1, fails = 0, drawn_touch = -1, looks = 0;
    unsigned geom_seen = ~0u;
    float dx = 1e9f, dy = 1e9f;
    double last_look = 0, last_draw = 0, last_stat = now_ms();
    long frames = 0;
    while (!o->stop) {
        if (t.fd >= 0) {
            struct pollfd pf = {.fd = t.fd, .events = POLLIN};
            int r = poll(&pf, 1, 12);
            if (r > 0 && (pf.revents & POLLIN)) touch_read(o, &t);
            if (t.fd < 0) logf_(o, "touch: closed");
        } else usleep(12000);
        double now = now_ms();
        if (now - last_look >= 100) {   /* 10 looks a second at the display's picture; a whole-picture search twice a second while the page isn't found */
            int on = locate(o, (looks++ % 5) == 0);
            if (on != o->on_screen) logf_(o, "page %s", on ? "on screen" : "not on screen");
            o->on_screen = on;
            last_look = now;
        }
        int on = o->on_screen && !o->fmt_unknown && o->have_page;
        if (on) {
            float x = *o->vx, y = *o->vy;
            int tch = o->touching;
            unsigned geom = o->geom;
            if (first || geom != geom_seen || x != dx || y != dy || tch != drawn_touch || !shown) {
                if (now - last_draw >= 10) {
                    if (geom != geom_seen) first = 1;   /* the plane moved or turned: a whole new picture */
                    render_pad(o, land, x, y, tch);
                    rotate_pad(o, land, rotf);
                    if (!o->no_vblank) {
                        union drm_wait_vblank wv;
                        memset(&wv, 0, sizeof wv);
                        wv.request.type = _DRM_VBLANK_RELATIVE | (o->crtc_index ? (unsigned)(o->crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK : 0);
                        wv.request.sequence = 1;
                        if (xioctl(o->fd, DRM_IOCTL_WAIT_VBLANK, &wv)) { o->no_vblank = 1; logf_(o, "WAIT_VBLANK: %s (copying without it)", strerror(errno)); }
                    }
                    ovl_buf_t *b = &o->buf[0];
                    for (int yy = 0; yy < o->ph; yy++) {
                        const uint8_t *src = rotf + (size_t)yy * o->pw * 4;
                        uint8_t *pv = prev + (size_t)yy * o->pw * 4;
                        if (!first && !memcmp(src, pv, (size_t)o->pw * 4)) continue;
                        memcpy(b->map + (size_t)yy * b->pitch, src, (size_t)o->pw * 4);
                        memcpy(pv, src, (size_t)o->pw * 4);
                    }
                    int replace = !shown || geom != geom_seen;
                    first = 0; dx = x; dy = y; drawn_touch = tch; last_draw = now; frames++; geom_seen = geom;
                    if (replace) {
                        if (set_plane(o, b->fb_id, o->pl_x, o->pl_y)) {
                            if (++fails == 1 || fails % 300 == 0) logf_(o, "SETPLANE: %s (%d so far)", strerror(errno), fails);
                            if (errno == EACCES || errno == EPERM) { logf_(o, "not the display's master: no pad"); break; }
                            shown = 0;
                        } else { shown = 1; logf_(o, "shown: plane at %d,%d, %dx%d on the display", o->pl_x, o->pl_y, o->pw, o->ph); }
                    }
                }
            }
        } else if (shown) {
            set_plane(o, 0, 0, 0);
            shown = 0; first = 1;
            logf_(o, "hidden (%s)", o->fmt_unknown ? "the display's picture can't be read" : "XYZ's page is not on the display");
        }
        if (now - last_stat >= 30000) {
            logf_(o, "%ld touch events, %ld fingers on the pad, %ld frames, %s", t.nevents, t.nclaims, frames, shown ? "shown" : "hidden");
            if (shown && t.fd >= 0 && t.nevents == 0) logf_(o, "touch: no events yet (if you touched the pad: MPC may hold the touchscreen exclusively)");
            last_stat = now;
        }
    }
    set_plane(o, 0, 0, 0);
    for (int i = 0; i < 2; i++) destroy_buffer(o, &o->buf[i]);
    __atomic_store_n(&o->live, 0, __ATOMIC_RELEASE);
    logf_(o, "stopped");
done:
    if (t.fd >= 0) close(t.fd);
    free(land); free(rotf); free(prev); free(g_base); g_base = NULL;
    pthread_mutex_lock(&g_owner_lock);
    if (g_owner == o) g_owner = NULL;
    pthread_mutex_unlock(&g_owner_lock);
    return NULL;
}

void pad_start(overlay_t *o, const char *data_dir, volatile float *vx, volatile float *vy, volatile unsigned *changed) {
    memset(o, 0, sizeof *o);
    o->fd = -1;
    o->vx = vx; o->vy = vy; o->changed = changed;
    if (!data_dir || !data_dir[0]) return;
    snprintf(o->dir, sizeof o->dir, "%s", data_dir);
    char off[700];
    snprintf(off, sizeof off, "%s/pad.off", data_dir);
    if (access(off, F_OK) == 0) return;
    snprintf(o->log_path, sizeof o->log_path, "%s/xyz.log", data_dir);
    FILE *f = fopen(o->log_path, "w");   /* a fresh log each start */
    if (f) fclose(f); else { o->log_path[0] = 0; return; }
    if (access("/proc/self/fd", R_OK)) return;
    load_layout(o);
    o->started = 1;
    if (pthread_create(&o->thread, NULL, pad_thread, o)) { o->started = 0; logf_(o, "no thread"); }
}

void pad_stop(overlay_t *o) {
    if (!o->started) return;
    o->stop = 1;
    pthread_join(o->thread, NULL);
    o->started = 0;
}
