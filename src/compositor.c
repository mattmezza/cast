/* Shared RGBA composition. Original bitmap glyphs below are part of cast's license. */
#include "cast.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLICK_MAX 32
typedef struct {
    int x, y, button;
    uint64_t at;
} Click;
struct Compositor {
    double zoom, from, target, cx, cy;
    uint64_t transition, last;
    int sw, sh;
    bool centered;
    Click clicks[CLICK_MAX];
    unsigned next;
    char key[CAST_TEXT];
    uint64_t key_at;
};
typedef struct {
    double sx, sy, sw, sh, dx, dy, dw, dh;
} Transform;
static double clampd(double n, double low, double high)
{
    return n < low ? low : n > high ? high : n;
}
static int fail(char *e, size_t n, const char *s)
{
    if (e && n) {
        snprintf(e, n, "%s", s);
    }
    return -1;
}
int frame_alloc(Frame *f, int w, int h)
{
    if (!f || w < 1 || h < 1 || w > 16384 || h > 16384) {
        errno = EINVAL;
        return -1;
    }
    if (f->data && f->width == w && f->height == h && f->stride == w * 4) {
        return 0;
    }
    size_t bytes = (size_t)w * 4 * (size_t)h;
    uint8_t *p = calloc(1, bytes);
    if (!p) {
        return -1;
    }
    free(f->data);
    *f = (Frame){.data = p, .width = w, .height = h, .stride = w * 4};
    return 0;
}
void frame_free(Frame *f)
{
    if (f) {
        free(f->data);
        memset(f, 0, sizeof(*f));
    }
}
int frame_copy(Frame *dst, const Frame *src)
{
    if (!dst || !src || !src->data || src->stride < src->width * 4) {
        errno = EINVAL;
        return -1;
    }
    if (dst == src) {
        return 0;
    }
    if (frame_alloc(dst, src->width, src->height) < 0) {
        return -1;
    }
    for (int y = 0; y < src->height; y++) {
        memcpy(dst->data + (size_t)y * dst->stride, src->data + (size_t)y * src->stride,
               (size_t)src->width * 4);
    }
    dst->ts_ns = src->ts_ns;
    return 0;
}
Compositor *compositor_create(void)
{
    Compositor *c = calloc(1, sizeof(*c));
    if (c) {
        c->zoom = c->target = c->from = 1;
    }
    return c;
}
void compositor_destroy(Compositor *c)
{
    free(c);
}
void compositor_clear(Compositor *c)
{
    if (c) {
        memset(c->clicks, 0, sizeof(c->clicks));
        c->next = 0;
        c->key[0] = 0;
        c->key_at = 0;
    }
}
void compositor_click(Compositor *c, int x, int y, int button, uint64_t at)
{
    if (c && button >= 1 && button <= 3) {
        c->clicks[c->next++ % CLICK_MAX] = (Click){x, y, button, at};
    }
}
void compositor_key(Compositor *c, const char *key, uint64_t at)
{
    if (c && key) {
        snprintf(c->key, sizeof(c->key), "%s", key);
        c->key_at = at;
    }
}
static void pixel(Frame *f, int x, int y, uint32_t color, double alpha)
{
    if (x < 0 || y < 0 || x >= f->width || y >= f->height) {
        return;
    }
    uint8_t *p = f->data + (size_t)y * f->stride + x * 4;
    alpha = clampd(alpha, 0, 1);
    unsigned a = (unsigned)lround(alpha * 255);
    p[0] = (uint8_t)((((color >> 16) & 255) * a + p[0] * (255 - a) + 127) / 255);
    p[1] = (uint8_t)((((color >> 8) & 255) * a + p[1] * (255 - a) + 127) / 255);
    p[2] = (uint8_t)(((color & 255) * a + p[2] * (255 - a) + 127) / 255);
    p[3] = 255;
}
static void fill(Frame *f, uint32_t color)
{
    for (int y = 0; y < f->height; y++) {
        for (int x = 0; x < f->width; x++) {
            uint8_t *p = f->data + (size_t)y * f->stride + x * 4;
            p[0] = color >> 16;
            p[1] = color >> 8;
            p[2] = color;
            p[3] = 255;
        }
    }
}
/* Five columns, seven rows, designed for this project (no external font asset). */
static const struct {
    char c;
    uint8_t rows[7];
} glyphs[] = {{'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
              {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
              {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
              {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
              {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
              {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
              {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 25, 21, 19, 19, 17}},
              {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
              {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
              {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
              {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
              {'W', {17, 17, 17, 21, 21, 27, 17}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
              {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
              {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
              {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
              {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
              {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
              {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
              {'+', {0, 4, 4, 31, 4, 4, 0}},       {'-', {0, 0, 0, 31, 0, 0, 0}},
              {'=', {0, 0, 31, 0, 31, 0, 0}},      {' ', {0, 0, 0, 0, 0, 0, 0}},
              {'.', {0, 0, 0, 0, 0, 12, 12}},      {',', {0, 0, 0, 0, 6, 4, 8}},
              {':', {0, 12, 12, 0, 12, 12, 0}},    {';', {0, 12, 12, 0, 12, 4, 8}},
              {'/', {1, 2, 2, 4, 8, 8, 16}},       {'\\', {16, 8, 8, 4, 2, 2, 1}},
              {'[', {14, 8, 8, 8, 8, 8, 14}},      {']', {14, 2, 2, 2, 2, 2, 14}},
              {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},
              {'<', {1, 2, 4, 8, 4, 2, 1}},        {'>', {16, 8, 4, 2, 4, 8, 16}},
              {'!', {4, 4, 4, 4, 4, 0, 4}},        {'?', {14, 17, 1, 2, 4, 0, 4}},
              {'_', {0, 0, 0, 0, 0, 0, 31}},       {'\'', {4, 4, 8, 0, 0, 0, 0}},
              {'"', {10, 10, 10, 0, 0, 0, 0}},     {'*', {0, 21, 14, 31, 14, 21, 0}},
              {'#', {10, 31, 10, 10, 31, 10, 0}},  {'@', {14, 17, 23, 21, 23, 16, 14}},
              {'%', {17, 2, 4, 8, 16, 17, 0}},     {'&', {12, 18, 20, 8, 21, 18, 13}},
              {'|', {4, 4, 4, 4, 4, 4, 4}},        {'^', {4, 10, 17, 0, 0, 0, 0}},
              {'~', {0, 0, 9, 22, 0, 0, 0}},       {'`', {8, 4, 2, 0, 0, 0, 0}},
              {'$', {4, 15, 20, 14, 5, 30, 4}},    {'{', {2, 4, 4, 8, 4, 4, 2}},
              {'}', {8, 4, 4, 2, 4, 4, 8}}};
static const uint8_t *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(*glyphs); i++) {
        if (glyphs[i].c == ch) {
            return glyphs[i].rows;
        }
    }
    return glyph('?');
}
static void text(Frame *f, const char *s, int x, int y, int scale, uint32_t color)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        for (int j = 0; j < 7; j++) {
            for (int i = 0; i < 5; i++) {
                if (g[j] & (1 << (4 - i))) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            pixel(f, x + i * scale + sx, y + j * scale + sy, color, 1);
                        }
                    }
                }
            }
        }
    }
}
void compositor_neutral(const Config *cfg, Frame *f)
{
    if (frame_alloc(f, cfg->width, cfg->height) < 0) {
        return;
    }
    fill(f, cfg->pause_color);
    int scale = cfg->height / 180;
    if (scale < 1) {
        scale = 1;
    }
    int maxscale = cfg->pause_text[0] ? cfg->width / ((int)strlen(cfg->pause_text) * 6 + 4) : scale;
    if (maxscale < scale) {
        scale = maxscale;
    }
    if (scale < 1) {
        scale = 1;
    }
    text(f, cfg->pause_text, (cfg->width - (int)strlen(cfg->pause_text) * 6 * scale) / 2,
         (cfg->height - 7 * scale) / 2, scale, 0xffffff);
    f->ts_ns = cast_now_ns();
}
static double camera_aspect(const Config *cfg, int sw, int sh)
{
    if (!strcmp(cfg->shape, "circle") || !strcmp(cfg->aspect, "1:1")) {
        return 1;
    }
    if (!strcmp(cfg->aspect, "4:3")) {
        return 4.0 / 3;
    }
    if (!strcmp(cfg->aspect, "16:9")) {
        return 16.0 / 9;
    }
    return sw > 0 && sh > 0 ? (double)sw / sh : 16.0 / 9;
}
int compositor_geometry(const Config *cfg, int sw, int sh, int *x, int *y, int *w, int *h, char *e,
                        size_t n)
{
    if (!cfg || cfg->width < 1 || cfg->height < 1 || cfg->margin < 0) {
        return fail(e, n, "invalid canvas or camera margin");
    }
    int maxw = cfg->width - 2 * cfg->margin, maxh = cfg->height - 2 * cfg->margin;
    if (maxw < 1 || maxh < 1) {
        return fail(e, n, "camera margins leave no usable canvas area");
    }
    double a = camera_aspect(cfg, sw, sh);
    int cw = (int)lround(cfg->width * cfg->camera_width_percent / 100), ch = (int)lround(cw / a);
    if (cw < 1 || ch < 1) {
        return fail(e, n, "camera size must occupy at least one pixel");
    }
    if (cw > maxw || ch > maxh) {
        double k = fmin((double)maxw / cw, (double)maxh / ch);
        cw = (int)floor(cw * k);
        ch = (int)floor(ch * k);
    }
    if (cw < 1 || ch < 1) {
        return fail(e, n, "camera aspect does not fit inside canvas margins");
    }
    int cx = cfg->camera_x, cy = cfg->camera_y;
    if (!strcmp(cfg->anchor, "top-left")) {
        cx = cfg->margin;
        cy = cfg->margin;
    } else if (!strcmp(cfg->anchor, "top-right")) {
        cx = cfg->width - cfg->margin - cw;
        cy = cfg->margin;
    } else if (!strcmp(cfg->anchor, "bottom-left")) {
        cx = cfg->margin;
        cy = cfg->height - cfg->margin - ch;
    } else if (!strcmp(cfg->anchor, "bottom-right")) {
        cx = cfg->width - cfg->margin - cw;
        cy = cfg->height - cfg->margin - ch;
    }
    cx = (int)clampd(cx, cfg->margin, cfg->width - cfg->margin - cw);
    cy = (int)clampd(cy, cfg->margin, cfg->height - cfg->margin - ch);
    *x = cx;
    *y = cy;
    *w = cw;
    *h = ch;
    return 0;
}
static Transform fit(double sx, double sy, double sw, double sh, double dx, double dy, double dw,
                     double dh, bool cover)
{
    Transform t = {sx, sy, sw, sh, dx, dy, dw, dh};
    double scale = cover ? fmax(dw / sw, dh / sh) : fmin(dw / sw, dh / sh);
    if (cover) {
        t.sw = dw / scale;
        t.sh = dh / scale;
        t.sx += (sw - t.sw) / 2;
        t.sy += (sh - t.sh) / 2;
    } else {
        t.dw = sw * scale;
        t.dh = sh * scale;
        t.dx += (dw - t.dw) / 2;
        t.dy += (dh - t.dh) / 2;
    }
    return t;
}
static void screen_blit(Frame *dst, const Frame *src, Transform t)
{
    int x0 = (int)ceil(t.dx), y0 = (int)ceil(t.dy), x1 = (int)ceil(t.dx + t.dw),
        y1 = (int)ceil(t.dy + t.dh);
    x0 = (int)clampd(x0, 0, dst->width);
    x1 = (int)clampd(x1, 0, dst->width);
    y0 = (int)clampd(y0, 0, dst->height);
    y1 = (int)clampd(y1, 0, dst->height);
    if (src->width == dst->width && src->height == dst->height && t.sx == 0 && t.sy == 0 &&
        t.dx == 0 && t.dy == 0 && t.sw == src->width && t.sh == src->height && t.dw == dst->width &&
        t.dh == dst->height) {
        for (int y = 0; y < dst->height; y++) {
            uint8_t *d = dst->data + (size_t)y * dst->stride;
            memcpy(d, src->data + (size_t)y * src->stride, (size_t)dst->width * 4);
            for (int x = 0; x < dst->width; x++) {
                d[x * 4 + 3] = 255;
            }
        }
        return;
    }
    /* Compute the separable horizontal mapping once, not once per output pixel. */
    int columns[16384];
    for (int x = x0; x < x1; x++) {
        columns[x] = (int)clampd(floor(t.sx + ((x + .5 - t.dx) / t.dw) * t.sw), 0, src->width - 1);
    }
    for (int y = y0; y < y1; y++) {
        int sy = (int)clampd(floor(t.sy + ((y + .5 - t.dy) / t.dh) * t.sh), 0, src->height - 1);
        uint8_t *d = dst->data + (size_t)y * dst->stride + x0 * 4;
        const uint8_t *row = src->data + (size_t)sy * src->stride;
        for (int x = x0; x < x1; x++, d += 4) {
            const uint8_t *s = row + columns[x] * 4;
            memcpy(d, s, 4);
            d[3] = 255;
        }
    }
}
typedef struct {
    int kind; /* rectangle, rounded, circle */
    double w, h, radius;
} Mask;
static bool mask_at(Mask mask, double x, double y)
{
    double w = mask.w, h = mask.h;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= w || y >= h) {
        return false;
    }
    if (mask.kind == 2) {
        double dx = (x - w / 2) / (w / 2), dy = (y - h / 2) / (h / 2);
        return dx * dx + dy * dy <= 1;
    }
    if (mask.kind == 1) {
        double r = mask.radius;
        if ((x >= r && x <= w - r) || (y >= r && y <= h - r)) {
            return true;
        }
        double dx = fmax(r - x, fmax(0, x - (w - r))), dy = fmax(r - y, fmax(0, y - (h - r)));
        return dx * dx + dy * dy <= r * r;
    }
    return true;
}
static void camera_blit(Frame *dst, const Frame *src, const Config *cfg, int x, int y, int w, int h)
{
    double aspect = camera_aspect(cfg, src->width, src->height), cw = src->width, ch = src->height;
    if (cw / ch > aspect) {
        cw = ch * aspect;
    } else {
        ch = cw / aspect;
    }
    double sx = clampd((src->width - cw) / 2 + cfg->crop_x, 0, src->width - cw),
           sy = clampd((src->height - ch) / 2 + cfg->crop_y, 0, src->height - ch);
    int border = cfg->border_width;
    if (border * 2 >= w || border * 2 >= h) {
        border = (w < h ? w : h) / 2 - 1;
    }
    if (border < 0) {
        border = 0;
    }
    int kind = !strcmp(cfg->shape, "circle") ? 2 : !strcmp(cfg->shape, "rounded") ? 1 : 0;
    Mask outer = {kind, w, h, fmin(fmax(0, cfg->radius), fmin(w, h) / 2)};
    Mask inner = {kind, w - 2 * border, h - 2 * border,
                  fmin(fmax(0, cfg->radius - border), fmin(w - 2 * border, h - 2 * border) / 2)};
    int columns[16384];
    for (int i = 0; i < w; i++) {
        double u = (i + .5 - border) / (w - 2 * border);
        if (cfg->mirror) {
            u = 1 - u;
        }
        columns[i] = (int)clampd(floor(sx + u * cw), 0, src->width - 1);
    }
    for (int j = 0; j < h; j++) {
        double v = (j + .5 - border) / (h - 2 * border);
        int iy = (int)clampd(floor(sy + v * ch), 0, src->height - 1);
        const uint8_t *row = src->data + (size_t)iy * src->stride;
        for (int i = 0; i < w; i++) {
            if (mask_at(outer, i + .5, j + .5)) {
                if (!mask_at(inner, i + .5 - border, j + .5 - border)) {
                    pixel(dst, x + i, y + j, cfg->border_color, 1);
                    continue;
                }
                if (x + i >= 0 && x + i < dst->width && y + j >= 0 && y + j < dst->height) {
                    uint8_t *d = dst->data + (size_t)(y + j) * dst->stride + (x + i) * 4;
                    memcpy(d, row + columns[i] * 4, 4);
                    d[3] = 255;
                }
            }
        }
    }
}
static bool transform_point(Transform t, int x, int y, double *ox, double *oy)
{
    if (x < t.sx || y < t.sy || x >= t.sx + t.sw || y >= t.sy + t.sh) {
        return false;
    }
    *ox = t.dx + (x - t.sx) * t.dw / t.sw;
    *oy = t.dy + (y - t.sy) * t.dh / t.sh;
    return true;
}
static void ring(Frame *f, double x, double y, double r, double thickness, uint32_t color,
                 double opacity)
{
    int x0 = (int)fmax(0, floor(x - r - thickness)),
        x1 = (int)fmin(f->width, ceil(x + r + thickness));
    int y0 = (int)fmax(0, floor(y - r - thickness)),
        y1 = (int)fmin(f->height, ceil(y + r + thickness));
    for (int j = y0; j < y1; j++) {
        for (int i = x0; i < x1; i++) {
            double d = hypot(i + .5 - x, j + .5 - y);
            double a = clampd(thickness / 2 + .5 - fabs(d - r), 0, 1);
            if (a > 0) {
                pixel(f, i, j, color, a * opacity);
            }
        }
    }
}
static void cursor_draw(Frame *f, double x, double y, int size, uint32_t color)
{
    if (size < 4) {
        size = 4;
    }
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size * 3 / 4; i++) {
            bool tip = i <= j / 2 && j < size * 3 / 4;
            bool stem = j >= size / 2 && j < size && i >= size / 4 && i <= size / 4 + size / 6;
            if (tip || stem) {
                pixel(f, (int)x + i, (int)y + j, (i == 0 || i == j / 2) ? 0x111111 : color, 1);
            }
        }
    }
}
static Transform screen_transform(Compositor *c, const Config *cfg, const Frame *s,
                                  const Cursor *cursor, uint64_t now, double dx, double dy,
                                  double dw, double dh)
{
    if (c->sw != s->width || c->sh != s->height) {
        c->centered = false;
        c->sw = s->width;
        c->sh = s->height;
    }
    bool valid = cursor && cursor->valid && cursor->x >= 0 && cursor->y >= 0 &&
                 cursor->x < s->width && cursor->y < s->height;
    if (!c->centered) {
        c->cx = valid ? cursor->x : s->width / 2.0;
        c->cy = valid ? cursor->y : s->height / 2.0;
        c->centered = true;
    }
    double wanted = clampd(cfg->zoom_factor, 1, cfg->zoom_max);
    if (fabs(wanted - c->target) > 1e-9) {
        c->from = c->zoom;
        c->target = wanted;
        c->transition = now;
        if (!cfg->zoom_follow && valid) {
            c->cx = cursor->x;
            c->cy = cursor->y;
        }
    }
    double elapsed = now >= c->transition ? (double)(now - c->transition) / 1e6 : 0;
    double p = cfg->zoom_transition_ms ? clampd(elapsed / cfg->zoom_transition_ms, 0, 1) : 1;
    p = p * p * (3 - 2 * p);
    c->zoom = c->from + (c->target - c->from) * p;
    double vw = s->width / c->zoom, vh = s->height / c->zoom;
    /* Live and recording render the same capture timestamp with different annotation
     * switches. Advance following once so their screen pixels remain identical. */
    if (cfg->zoom_follow && valid && c->zoom > 1 && (!c->last || now > c->last)) {
        double dt = c->last && now > c->last ? (double)(now - c->last) / 1e9 : 1.0 / cfg->fps;
        double alpha = cfg->zoom_smoothing >= 1
                           ? 1
                           : 1 - pow(1 - clampd(cfg->zoom_smoothing, .001, 1), dt * cfg->fps);
        double dead = cfg->zoom_deadzone, tx = c->cx, ty = c->cy;
        if (cursor->x < c->cx - dead) {
            tx = cursor->x + dead;
        } else if (cursor->x > c->cx + dead) {
            tx = cursor->x - dead;
        }
        if (cursor->y < c->cy - dead) {
            ty = cursor->y + dead;
        } else if (cursor->y > c->cy + dead) {
            ty = cursor->y - dead;
        }
        c->cx += (tx - c->cx) * alpha;
        c->cy += (ty - c->cy) * alpha;
    }
    c->last = now;
    double center_x = clampd(c->cx, vw / 2, s->width - vw / 2);
    double center_y = clampd(c->cy, vh / 2, s->height - vh / 2);
    return fit(center_x - vw / 2, center_y - vh / 2, vw, vh, dx, dy, dw, dh,
               !strcmp(cfg->fit, "cover"));
}
int compositor_render(Compositor *c, const Config *cfg, const Frame *screen, const Frame *camera,
                      const Cursor *cursor, bool record_target, Frame *out, char *err, size_t n)
{
    if (!c || !cfg || frame_alloc(out, cfg->width, cfg->height) < 0) {
        return fail(err, n, "cannot allocate composition canvas");
    }
    uint64_t now = screen && screen->ts_ns ? screen->ts_ns : cast_now_ns();
    fill(out, cfg->pause_color);
    out->ts_ns = now;
    bool show_clicks =
        record_target ? cfg->annotations_record_clicks : cfg->annotations_live_clicks;
    bool show_keys = record_target ? cfg->annotations_record_keys : cfg->annotations_live_keys;
    bool show_screen = strcmp(cfg->layout, "camera") != 0,
         show_camera = strcmp(cfg->layout, "screen") != 0 && cfg->camera_visible;
    Transform t = {0};
    int sx = 0, sw = cfg->width;
    int split = (int)lround(cfg->width * cfg->split_ratio / 100);
    if (!strcmp(cfg->layout, "split")) {
        if (split < 1 || split >= cfg->width) {
            return fail(err, n, "split ratio leaves an empty source area");
        }
        sw -= split;
        if (!strcmp(cfg->split_side, "left")) {
            sx = split;
        }
    }
    if (show_screen && screen && screen->data && screen->width > 0 && screen->height > 0) {
        t = screen_transform(c, cfg, screen, cursor, now, sx, 0, sw, cfg->height);
        screen_blit(out, screen, t);
        double x, y;
        if (cursor && cursor->valid && transform_point(t, cursor->x, cursor->y, &x, &y)) {
            if (cfg->cursor_highlight) {
                ring(out, x, y, cfg->cursor_size, 3, cfg->cursor_color, .65);
            }
            if (cfg->cursor) {
                cursor_draw(out, x, y, cfg->cursor_size, cfg->cursor_color);
            }
        }
        if (show_clicks && cfg->clicks) {
            for (unsigned i = 0; i < CLICK_MAX; i++) {
                Click *cl = &c->clicks[i];
                if (!cl->at || now < cl->at || (!cfg->click_middle && cl->button == 2)) {
                    continue;
                }
                double age = (double)(now - cl->at) / 1e6;
                if (age >= cfg->click_duration_ms || !transform_point(t, cl->x, cl->y, &x, &y)) {
                    continue;
                }
                double p = age / cfg->click_duration_ms;
                uint32_t color = cl->button == 1   ? cfg->click_left_color
                                 : cl->button == 3 ? cfg->click_right_color
                                                   : cfg->click_middle_color;
                ring(out, x, y, cfg->click_radius * (.35 + .65 * p), 3, color, 1 - p);
            }
        }
    }
    if (show_camera && camera && camera->data) {
        int x, y, w, h;
        if (!strcmp(cfg->layout, "overlay")) {
            if (compositor_geometry(cfg, camera->width, camera->height, &x, &y, &w, &h, err, n) <
                0) {
                return -1;
            }
        } else {
            int area = !strcmp(cfg->layout, "split") ? split : cfg->width;
            int ax = !strcmp(cfg->layout, "split") && !strcmp(cfg->split_side, "right")
                         ? cfg->width - split
                         : 0;
            double a = camera_aspect(cfg, camera->width, camera->height);
            w = area;
            h = (int)lround(w / a);
            if (h > cfg->height) {
                h = cfg->height;
                w = (int)lround(h * a);
            }
            if (w < 1 || h < 1) {
                return fail(err, n, "camera does not fit its layout area");
            }
            x = ax + (area - w) / 2;
            y = (cfg->height - h) / 2;
        }
        camera_blit(out, camera, cfg, x, y, w, h);
    }
    if (show_keys && cfg->keys && c->key[0] && now >= c->key_at &&
        (now - c->key_at) / 1000000 < (uint64_t)cfg->keys_timeout_ms) {
        int scale = cfg->keys_font_size / 7;
        if (scale < 1) {
            scale = 1;
        }
        size_t len = strlen(c->key);
        int maxchars = (out->width - 16) / (6 * scale);
        if (maxchars < 1) {
            maxchars = 1;
        }
        if (len > (size_t)maxchars) {
            len = (size_t)maxchars;
        }
        char label[CAST_TEXT];
        memcpy(label, c->key, len);
        label[len] = 0;
        int w = (int)len * 6 * scale + 16, h = 7 * scale + 16;
        int x = cfg->margin, y = cfg->margin;
        if (strstr(cfg->keys_position, "right")) {
            x = out->width - cfg->margin - w;
        } else if (strstr(cfg->keys_position, "center")) {
            x = (out->width - w) / 2;
        }
        if (strstr(cfg->keys_position, "bottom")) {
            y = out->height - cfg->margin - h;
        }
        x = (int)clampd(x, 0, fmax(0, out->width - w));
        y = (int)clampd(y, 0, fmax(0, out->height - h));
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                pixel(out, x + i, y + j, cfg->keys_background, .85);
            }
        }
        text(out, label, x + 8, y + 8, scale, cfg->keys_color);
    }
    return 0;
}
