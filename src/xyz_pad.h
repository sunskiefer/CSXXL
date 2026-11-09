/* XYZ: a free X/Y pad drawn by the plugin on a hardware overlay plane of the Force's display, driven by the touchscreen
 * read from /dev/input. The DRM plane, the marker search and the buffers are VADER 0.5.0's overlay code (sunskiefer/VADER
 * src/overlay.c, MIT), unchanged in how they work; the touch reader and the pad drawing are new. See xyz_pad.c. */
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* the skin page (1280 x 628) and its markers: 2 x 2 pixels at the page's top-left and bottom-right (tools/skin_post.py) */
#define OVL_TILES_X 0
#define OVL_TILES_Y 0   /* (unused since 0.0.2: the page's place is found by rotation-aware search) */
#define OVL_TILES_W 1280
#define OVL_TILES_H 628

typedef struct { uint32_t handle, pitch, fb_id; uint64_t size; uint8_t *map; } ovl_buf_t;

typedef struct {
    char log_path[700], dir[600];
    int started, stop, live, waited_logged;
    pthread_t thread;
    /* DRM */
    int fd, crtc_index;
    uint32_t crtc_id, plane_id, format, disp_w, disp_h;
    uint64_t zpos_max;
    int has_zpos;
    ovl_buf_t buf[2];
    int back, fb_slot;
    /* the page on the display (the markers) */
    int box_found, box_logged, box_x, box_y, on_screen, fmt_unknown, fb_logged, fb_err_logged, no_vblank;
    double ms_avg;
    /* the pad: its box in page pixels (pad/layout.txt) */
    int pad_x, pad_y, bw, bh;
    /* 0.0.2: the Force's panel is portrait and MPC draws its landscape screen turned into it. rot: how the landscape screen
     * (lw x lh, u right, v down) lies in the display (fb_w x fb_h): 0 as is, 1 turned 90 clockwise, 2 turned 180, 3 turned 90
     * anticlockwise. box_x, box_y: the page's top-left on the landscape screen. The plane (the pad turned the same way): its
     * place pl_x, pl_y and size pw x ph on the display; its buffers are bufd x bufd. geom counts changes of any of it. */
    int have_page, rot, lw, lh, pw, ph, bufd, pl_x, pl_y;
    unsigned fb_w, fb_h, geom;
    /* the values (real units, -5 .. +5), shared with the engine */
    volatile float *vx, *vy;
    volatile unsigned *changed;      /* bit 0: x set by touch, bit 1: y */
    volatile int touching;
} overlay_t;

void pad_start(overlay_t *o, const char *data_dir, volatile float *vx, volatile float *vy, volatile unsigned *changed);
void pad_stop(overlay_t *o);
/* offline: one frame of the pad at (x, y) into out (bw * bh * 4, premultiplied ARGB little-endian); 0 on success */
int pad_render_test(const char *data_dir, float x, float y, int touching, uint8_t *out, int *bw, int *bh);
