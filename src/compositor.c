/* Shared RGBA composition. Original bitmap glyphs below are part of cast's license. */
#include "cast.h"
#include "composition_assets.h"
#include "presentation_text.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLICK_MAX 32
#define KEY_MAX 8
typedef struct {
    int x, y, button;
    uint64_t at;
} Click;
typedef struct {
    char label[CAST_TEXT];
    unsigned repeats;
    uint64_t at;
} Key;
typedef struct {
    Frame pixels;
    uint64_t at;
    int source_w, source_h, radius, area_w, area_h;
    bool mirror;
    uint32_t color;
    double brightness;
    int type, source_kind;
    uint32_t gradient_from, gradient_via, gradient_to;
    bool gradient_via_enabled;
    double gradient_angle, gradient_waypoint;
} Backdrop;
struct Compositor {
    double zoom, from, target, cx, cy;
    uint64_t transition, last;
    int sw, sh;
    bool centered;
    Click clicks[CLICK_MAX];
    unsigned next;
    Key keys[KEY_MAX];
    unsigned key_count;
    PresentationText *presentation;
    Backdrop camera_backdrop, screen_backdrop;
    Frame logo, logo_scaled;
    char logo_path[PATH_MAX];
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
    if (c) {
        presentation_text_destroy(c->presentation);
        frame_free(&c->camera_backdrop.pixels);
        frame_free(&c->screen_backdrop.pixels);
        frame_free(&c->logo);
        frame_free(&c->logo_scaled);
    }
    free(c);
}
void compositor_clear(Compositor *c)
{
    if (c) {
        memset(c->clicks, 0, sizeof(c->clicks));
        c->next = 0;
        memset(c->keys, 0, sizeof(c->keys));
        c->key_count = 0;
        frame_free(&c->camera_backdrop.pixels);
        frame_free(&c->screen_backdrop.pixels);
        c->camera_backdrop.at = c->screen_backdrop.at = 0;
    }
}
void compositor_click(Compositor *c, int x, int y, int button, uint64_t at)
{
    if (c && button >= 1 && button <= 3) {
        c->clicks[c->next++ % CLICK_MAX] = (Click){x, y, button, at};
    }
}
static void keys_expire(Compositor *c, uint64_t now, int timeout_ms)
{
    unsigned keep = 0;
    uint64_t timeout = timeout_ms > 0 ? (uint64_t)timeout_ms * 1000000 : 0;
    for (unsigned i = 0; i < c->key_count; i++) {
        Key *key = &c->keys[i];
        if (timeout && (now < key->at || now - key->at < timeout)) {
            c->keys[keep++] = *key;
        }
    }
    /* Erase expired labels as well as releasing their queue slots. */
    memset(c->keys + keep, 0, (KEY_MAX - keep) * sizeof(*c->keys));
    c->key_count = keep;
}
void compositor_key(Compositor *c, const char *key, uint64_t at, int timeout_ms)
{
    if (!c || !key || !*key) {
        return;
    }
    keys_expire(c, at, timeout_ms);
    if (timeout_ms <= 0) {
        return;
    }
    char label[CAST_TEXT];
    snprintf(label, sizeof(label), "%s", key);
    if (c->key_count && !strcmp(c->keys[c->key_count - 1].label, label)) {
        Key *latest = &c->keys[c->key_count - 1];
        if (latest->repeats < UINT_MAX) {
            latest->repeats++;
        }
        latest->at = at;
        return;
    }
    if (c->key_count == KEY_MAX) {
        memmove(c->keys, c->keys + 1, (KEY_MAX - 1) * sizeof(*c->keys));
        c->key_count--;
    }
    Key *latest = &c->keys[c->key_count++];
    *latest = (Key){.repeats = 1, .at = at};
    memcpy(latest->label, label, strlen(label) + 1);
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
              {'}', {8, 4, 4, 2, 4, 4, 8}},        {'a', {0, 0, 14, 1, 15, 17, 15}},
              {'b', {16, 16, 30, 17, 17, 17, 30}}, {'c', {0, 0, 14, 17, 16, 17, 14}},
              {'d', {1, 1, 15, 17, 17, 17, 15}},   {'e', {0, 0, 14, 17, 31, 16, 14}},
              {'f', {6, 8, 8, 28, 8, 8, 8}},       {'g', {0, 14, 17, 17, 15, 1, 14}},
              {'h', {16, 16, 30, 17, 17, 17, 17}}, {'i', {4, 0, 12, 4, 4, 4, 14}},
              {'j', {2, 0, 6, 2, 2, 18, 12}},      {'k', {16, 16, 18, 20, 24, 20, 18}},
              {'l', {12, 4, 4, 4, 4, 4, 14}},      {'m', {0, 0, 26, 21, 21, 21, 21}},
              {'n', {0, 0, 30, 17, 17, 17, 17}},   {'o', {0, 0, 14, 17, 17, 17, 14}},
              {'p', {0, 30, 17, 17, 30, 16, 16}},  {'q', {0, 15, 17, 17, 15, 1, 1}},
              {'r', {0, 0, 22, 25, 16, 16, 16}},   {'s', {0, 0, 15, 16, 14, 1, 30}},
              {'t', {8, 8, 28, 8, 8, 9, 6}},       {'u', {0, 0, 17, 17, 17, 19, 13}},
              {'v', {0, 0, 17, 17, 17, 10, 4}},    {'w', {0, 0, 17, 17, 21, 21, 10}},
              {'x', {0, 0, 17, 10, 4, 10, 17}},    {'y', {0, 17, 17, 17, 15, 1, 14}},
              {'z', {0, 0, 31, 2, 4, 8, 31}}};
static const uint8_t *glyph(char ch)
{
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
static void keys_draw(Compositor *c, const Config *cfg, Frame *out, uint64_t now)
{
    unsigned indices[KEY_MAX], active = 0;
    for (unsigned i = 0; i < c->key_count; i++) {
        if (c->keys[i].at <= now) {
            indices[active++] = i;
        }
    }
    if (!active) {
        return;
    }
    int padding = (int)fmin(8, fmin(out->width, out->height) / 4);
    int scale = cfg->keys_font_size / 7;
    if (scale < 1) {
        scale = 1;
    }
    /* Keep at least one glyph and a repeat suffix within even a small canvas. */
    int suffix_width = 0;
    for (unsigned i = 0; i < active; i++) {
        if (c->keys[indices[i]].repeats > 1) {
            char suffix[16];
            int width = snprintf(suffix, sizeof(suffix), "x%u", c->keys[indices[i]].repeats);
            if (width > suffix_width) {
                suffix_width = width;
            }
        }
    }
    int maxscale = (out->width - 2 * padding) / (6 * (1 + suffix_width));
    int maxheightscale = (out->height - 2 * padding) / 7;
    if (maxheightscale < maxscale) {
        maxscale = maxheightscale;
    }
    if (maxscale < 1) {
        return;
    }
    if (scale > maxscale) {
        scale = maxscale;
    }
    int h = 7 * scale + 2 * padding, gap = 4;
    unsigned visible = (unsigned)((out->height + gap) / (h + gap));
    if (visible > active) {
        visible = active;
    }
    int stack_h = (int)visible * (h + gap) - gap;
    bool bottom = strstr(cfg->keys_position, "bottom") != NULL;
    int y = bottom ? out->height - cfg->margin - stack_h : cfg->margin;
    y = (int)clampd(y, 0, out->height - stack_h);
    int maxchars = (out->width - 2 * padding) / (6 * scale);
    for (unsigned row = 0; row < visible; row++) {
        const Key *key = &c->keys[indices[active - 1 - row]];
        char suffix[16] = "", label[CAST_TEXT + 16];
        if (key->repeats > 1) {
            snprintf(suffix, sizeof(suffix), "x%u", key->repeats);
        }
        size_t len = strlen(key->label), suffix_len = strlen(suffix);
        if (len > (size_t)maxchars - suffix_len) {
            len = (size_t)maxchars - suffix_len;
        }
        memcpy(label, key->label, len);
        memcpy(label + len, suffix, suffix_len + 1);
        int w = (int)(len + suffix_len) * 6 * scale + 2 * padding;
        int x = cfg->margin;
        if (strstr(cfg->keys_position, "right")) {
            x = out->width - cfg->margin - w;
        } else if (strstr(cfg->keys_position, "center")) {
            x = (out->width - w) / 2;
        }
        x = (int)clampd(x, 0, out->width - w);
        /* The newest row stays at the configured edge, with older rows inward. */
        int row_y = y + (int)(bottom ? visible - 1 - row : row) * (h + gap);
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                pixel(out, x + i, row_y + j, cfg->keys_background, .85);
            }
        }
        text(out, label, x + padding, row_y + padding, scale, cfg->keys_color);
    }
}
static void logo_size(const Config *cfg, const Frame *logo, int *w, int *h)
{
    int available_w = (int)fmax(1, cfg->width - 2 * cfg->logo_margin_x);
    int available_h = (int)fmax(1, cfg->height - 2 * cfg->logo_margin_y);
    double width = fmax(1, cfg->width * cfg->logo_width_percent / 100);
    double scale = fmin(width / logo->width, fmin((double)available_w / logo->width,
                                                  (double)available_h / logo->height));
    *w = (int)fmax(1, lround(logo->width * scale));
    *h = (int)fmax(1, lround(logo->height * scale));
}
int compositor_prepare(Compositor *c, const Config *cfg, char *error, size_t n)
{
    if (!c || !cfg) {
        return fail(error, n, "composition and configuration are required");
    }
    if (!c->presentation) {
        c->presentation = presentation_text_create();
        if (!c->presentation) {
            return fail(error, n, "cannot initialize FreeType presentation text renderer");
        }
    }
    bool replace_logo = strcmp(c->logo_path, cfg->logo_path) != 0;
    Frame candidate = {0};
    if (replace_logo && cfg->logo_path[0] &&
        composition_logo_load(cfg->logo_path, &candidate, error, n)) {
        return -1;
    }
    const Frame *logo = replace_logo ? &candidate : &c->logo;
    int w = 0, h = 0;
    if (logo->data) {
        logo_size(cfg, logo, &w, &h);
    }
    bool replace_scaled = replace_logo || w != c->logo_scaled.width || h != c->logo_scaled.height;
    Frame scaled = {0};
    if (replace_scaled && logo->data && composition_image_scale(logo, &scaled, w, h, error, n)) {
        frame_free(&candidate);
        frame_free(&scaled);
        return -1;
    }
    if (presentation_text_prepare(c->presentation, cfg, error, n)) {
        frame_free(&candidate);
        frame_free(&scaled);
        return -1;
    }
    if (replace_logo) {
        frame_free(&c->logo);
        c->logo = candidate;
        snprintf(c->logo_path, sizeof c->logo_path, "%s", cfg->logo_path);
    }
    if (replace_scaled) {
        frame_free(&c->logo_scaled);
        c->logo_scaled = scaled;
    }
    return 0;
}
int compositor_neutral(Compositor *c, const Config *cfg, Frame *f, char *error, size_t n)
{
    if (compositor_prepare(c, cfg, error, n)) {
        return -1;
    }
    if (frame_alloc(f, cfg->width, cfg->height) < 0) {
        return fail(error, n, "cannot allocate solid paused frame");
    }
    fill(f, cfg->pause_color);
    f->ts_ns = cast_now_ns();
    return presentation_text_draw(c->presentation, cfg, false, f, error, n);
}
/* Three sliding-window box passes approximate a Gaussian, at linear cost in pixels.
 * Larger output canvases are sampled to a bounded working image before filtering. */
static void blur_pass(const uint8_t *src, uint8_t *dst, int width, int height, int radius,
                      bool vertical)
{
    int lines = vertical ? width : height, length = vertical ? height : width;
    int stride = vertical ? width * 4 : 4;
    unsigned divisor = (unsigned)(radius * 2 + 1);
    for (int line = 0; line < lines; line++) {
        int offset = vertical ? line * 4 : line * width * 4;
        unsigned sum[3] = {0};
        for (int i = -radius; i <= radius; i++) {
            int index = i < 0 ? 0 : i >= length ? length - 1 : i;
            const uint8_t *pixel = src + offset + index * stride;
            for (int ch = 0; ch < 3; ch++) {
                sum[ch] += pixel[ch];
            }
        }
        for (int i = 0; i < length; i++) {
            uint8_t *pixel = dst + offset + i * stride;
            for (int ch = 0; ch < 3; ch++) {
                pixel[ch] = (uint8_t)((sum[ch] + divisor / 2) / divisor);
            }
            pixel[3] = 255;
            int removed = i - radius, added = i + radius + 1;
            removed = removed < 0 ? 0 : removed;
            added = added >= length ? length - 1 : added;
            const uint8_t *old = src + offset + removed * stride;
            const uint8_t *next = src + offset + added * stride;
            for (int ch = 0; ch < 3; ch++) {
                sum[ch] += next[ch];
                sum[ch] -= old[ch];
            }
        }
    }
}
static int image_blur(Frame *frame, int blur_radius, uint32_t tint_color, double tint_opacity,
                      char *error, size_t n)
{
    if (!frame || !frame->data || frame->width < 1 || frame->height < 1 ||
        frame->stride < frame->width * 4) {
        return fail(error, n, "blur requires an owned RGBA frame");
    }
    if (blur_radius < 1 || blur_radius > 128 || !isfinite(tint_opacity) || tint_opacity < 0 ||
        tint_opacity > 1) {
        return fail(error, n, "blur radius must be 1..128 and opacity must be 0..1");
    }
    int step = (int)fmax(1, ceil(fmax((double)frame->width / 640, (double)frame->height / 360)));
    int width = (frame->width + step - 1) / step, height = (frame->height + step - 1) / step;
    size_t bytes = (size_t)width * height * 4;
    uint8_t *first = malloc(bytes), *second = malloc(bytes);
    if (!first || !second) {
        free(first);
        free(second);
        return fail(error, n, "cannot allocate bounded blur working image");
    }
    /* Average every source pixel during downsampling rather than aliasing fine text. */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned sum[3] = {0}, count = 0;
            for (int sy = y * step; sy < (y + 1) * step && sy < frame->height; sy++) {
                for (int sx = x * step; sx < (x + 1) * step && sx < frame->width; sx++) {
                    const uint8_t *pixel = frame->data + (size_t)sy * frame->stride + sx * 4;
                    for (int ch = 0; ch < 3; ch++) {
                        sum[ch] += pixel[ch];
                    }
                    count++;
                }
            }
            uint8_t *pixel = first + ((size_t)y * width + x) * 4;
            for (int ch = 0; ch < 3; ch++) {
                pixel[ch] = (uint8_t)((sum[ch] + count / 2) / count);
            }
            pixel[3] = 255;
        }
    }
    int radius = (int)fmax(1, lround((double)blur_radius / step));
    for (int pass = 0; pass < 3; pass++) {
        blur_pass(first, second, width, height, radius, false);
        blur_pass(second, first, width, height, radius, true);
    }
    unsigned alpha = (unsigned)lround(tint_opacity * 255);
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < frame->width; x++) {
            const uint8_t *sample = first + ((size_t)(y / step) * width + x / step) * 4;
            uint8_t *pixel = frame->data + (size_t)y * frame->stride + x * 4;
            for (int ch = 0; ch < 3; ch++) {
                unsigned tint = (tint_color >> (16 - ch * 8)) & 255;
                pixel[ch] = (uint8_t)((tint * alpha + sample[ch] * (255 - alpha) + 127) / 255);
            }
            pixel[3] = 255;
        }
    }
    free(first);
    free(second);
    return 0;
}
int compositor_blur(Compositor *c, const Config *cfg, Frame *frame, char *error, size_t n)
{
    if (compositor_prepare(c, cfg, error, n) ||
        image_blur(frame, cfg->blur_radius, cfg->blur_color, cfg->blur_opacity, error, n)) {
        return -1;
    }
    return presentation_text_draw(c->presentation, cfg, true, frame, error, n);
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
    } else if (!strcmp(cfg->anchor, "top") || !strcmp(cfg->anchor, "top-center")) {
        cx = (cfg->width - cw) / 2;
        cy = cfg->margin;
    } else if (!strcmp(cfg->anchor, "bottom") || !strcmp(cfg->anchor, "bottom-center")) {
        cx = (cfg->width - cw) / 2;
        cy = cfg->height - cfg->margin - ch;
    } else if (!strcmp(cfg->anchor, "left") || !strcmp(cfg->anchor, "center-left")) {
        cx = cfg->margin;
        cy = (cfg->height - ch) / 2;
    } else if (!strcmp(cfg->anchor, "right") || !strcmp(cfg->anchor, "center-right")) {
        cx = cfg->width - cfg->margin - cw;
        cy = (cfg->height - ch) / 2;
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
typedef struct {
    Mask mask;
    double x, y;
} Clip;
static void clipped_pixel(Frame *out, const Clip *clip, int x, int y, uint32_t color,
                          double opacity)
{
    if (!clip || mask_at(clip->mask, x + .5 - clip->x, y + .5 - clip->y)) {
        pixel(out, x, y, color, opacity);
    }
}
static bool mask_span(Mask mask, double origin_x, double origin_y, int y, int limit_left,
                      int limit_right, int *left, int *right)
{
    double local_y = y + .5 - origin_y;
    if (local_y < 0 || local_y >= mask.h || mask.w <= 0 || mask.h <= 0) {
        return false;
    }
    double first = 0, last = mask.w;
    if (mask.kind == 1 && mask.radius > 0 &&
        (local_y < mask.radius || local_y > mask.h - mask.radius)) {
        double vertical = fmax(mask.radius - local_y, local_y - (mask.h - mask.radius));
        double reach = sqrt(fmax(0, mask.radius * mask.radius - vertical * vertical));
        first = mask.radius - reach;
        last = mask.w - mask.radius + reach;
    }
    int begin = (int)fmax(limit_left, ceil(origin_x + first - .5));
    int end = (int)fmin(limit_right, floor(origin_x + last - .5) + 1);
    end = (int)fmin(end, ceil(origin_x + mask.w - .5));
    /* Correct rounding against the shared predicate at the two boundaries.
     * The row interior needs no per-pixel mask calculations. */
    while (begin > limit_left && mask_at(mask, begin - .5 - origin_x, local_y)) {
        begin--;
    }
    while (begin < end && !mask_at(mask, begin + .5 - origin_x, local_y)) {
        begin++;
    }
    while (end < limit_right && mask_at(mask, end + .5 - origin_x, local_y)) {
        end++;
    }
    while (end > begin && !mask_at(mask, end - .5 - origin_x, local_y)) {
        end--;
    }
    *left = begin;
    *right = end;
    return begin < end;
}
static void border_span(Frame *out, int y, int left, int right, uint32_t color)
{
    uint8_t rgba[] = {(uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color, 255};
    uint8_t *dest = out->data + (size_t)y * out->stride + left * 4;
    for (int x = left; x < right; x++, dest += 4) {
        memcpy(dest, rgba, 4);
    }
}
static Clip screen_layer_draw(Frame *out, const Frame *source, const Config *cfg, Transform t)
{
    double radius = clampd(cfg->screen_radius, 0, fmin(t.dw, t.dh) / 2);
    int border = (int)clampd(cfg->screen_border_width, 0, fmax(0, (fmin(t.dw, t.dh) - 1) / 2));
    Mask outer = {radius > 0, t.dw, t.dh, radius};
    Mask inner = {radius > 0, t.dw - 2 * border, t.dh - 2 * border, fmax(0, radius - border)};
    Clip clip = {inner, t.dx + border, t.dy + border};
    if (!radius && !border) {
        screen_blit(out, source, t);
        return clip;
    }
    int left = (int)fmax(0, ceil(t.dx)), right = (int)fmin(out->width, ceil(t.dx + t.dw));
    int top = (int)fmax(0, ceil(t.dy)), bottom = (int)fmin(out->height, ceil(t.dy + t.dh));
    int columns[16384];
    for (int x = left; x < right; x++) {
        columns[x] = (int)clampd(floor(t.sx + (x + .5 - t.dx) * t.sw / t.dw), 0, source->width - 1);
    }
    for (int y = top; y < bottom; y++) {
        int outer_left, outer_right;
        if (!mask_span(outer, t.dx, t.dy, y, left, right, &outer_left, &outer_right)) {
            continue;
        }
        int inner_left, inner_right;
        if (!mask_span(inner, clip.x, clip.y, y, outer_left, outer_right, &inner_left,
                       &inner_right)) {
            border_span(out, y, outer_left, outer_right, cfg->screen_border_color);
            continue;
        }
        border_span(out, y, outer_left, inner_left, cfg->screen_border_color);
        border_span(out, y, inner_right, outer_right, cfg->screen_border_color);
        int row = (int)clampd(floor(t.sy + (y + .5 - t.dy) * t.sh / t.dh), 0, source->height - 1);
        uint8_t *dest = out->data + (size_t)y * out->stride + inner_left * 4;
        const uint8_t *source_row = source->data + (size_t)row * source->stride;
        for (int x = inner_left; x < inner_right; x++, dest += 4) {
            memcpy(dest, source_row + columns[x] * 4, 4);
            dest[3] = 255;
        }
    }
    return clip;
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
static void gradient_fill(Frame *out, const Config *cfg)
{
    double angle = cfg->gradient_angle * M_PI / 180;
    double dx = cos(angle), dy = sin(angle);
    if (fabs(dx) < 1e-10) {
        dx = 0;
    }
    if (fabs(dy) < 1e-10) {
        dy = 0;
    }
    double origin = fmin(0, dx * (out->width - 1)) + fmin(0, dy * (out->height - 1));
    double extent = fabs(dx) * (out->width - 1) + fabs(dy) * (out->height - 1);
    double waypoint = clampd(cfg->gradient_waypoint / 100, .01, .99);
    for (int y = 0; y < out->height; y++) {
        for (int x = 0; x < out->width; x++) {
            double position = extent > 0 ? (dx * x + dy * y - origin) / extent : 0;
            uint32_t from = cfg->gradient_from, to = cfg->gradient_to;
            if (cfg->gradient_via_enabled) {
                if (position <= waypoint) {
                    to = cfg->gradient_via;
                    position /= waypoint;
                } else {
                    from = cfg->gradient_via;
                    position = (position - waypoint) / (1 - waypoint);
                }
            }
            position = clampd(position, 0, 1);
            uint8_t *pixel = out->data + (size_t)y * out->stride + x * 4;
            for (int channel = 0; channel < 3; channel++) {
                int shift = 16 - channel * 8;
                double first = (from >> shift) & 255, last = (to >> shift) & 255;
                pixel[channel] = (uint8_t)lround(first + (last - first) * position);
            }
            pixel[3] = 255;
        }
    }
}
static int background_draw(Backdrop *cache, const Config *cfg, const Frame *source, int source_kind,
                           bool mirror, const char *mode, uint32_t color, int blur_radius,
                           double brightness, Frame *out, int x, int y, int w, int h, char *error,
                           size_t n)
{
    bool dynamic = !strcmp(mode, "blurred") && source && source->data && source->width > 0 &&
                   source->height > 0;
    if (!mode[0] || !strcmp(mode, "solid")) {
        if (!mode[0]) {
            color = cfg->pause_color; /* Preserve zero-initialized embedding configurations. */
        }
        frame_free(&cache->pixels);
        memset(cache, 0, sizeof *cache);
        uint8_t rgba[] = {(uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color, 255};
        for (int j = 0; j < h; j++) {
            uint8_t *row = out->data + (size_t)(y + j) * out->stride + x * 4;
            for (int i = 0; i < w; i++) {
                memcpy(row + i * 4, rgba, 4);
            }
        }
        return 0;
    }
    int type = dynamic ? 2 : 1;
    int step = dynamic ? (int)fmax(1, ceil(fmax((double)w / 320, (double)h / 180))) : 1;
    int width = (w + step - 1) / step, height = (h + step - 1) / step;
    bool reuse =
        cache->pixels.data && cache->type == type && cache->area_w == w && cache->area_h == h;
    if (dynamic) {
        reuse = reuse && source->ts_ns && cache->at == source->ts_ns &&
                cache->source_kind == source_kind && cache->source_w == source->width &&
                cache->source_h == source->height && cache->mirror == mirror &&
                cache->color == color && cache->radius == blur_radius &&
                cache->brightness == brightness;
    } else {
        reuse = reuse && cache->gradient_from == cfg->gradient_from &&
                cache->gradient_via == cfg->gradient_via &&
                cache->gradient_to == cfg->gradient_to &&
                cache->gradient_via_enabled == cfg->gradient_via_enabled &&
                cache->gradient_angle == cfg->gradient_angle &&
                cache->gradient_waypoint == cfg->gradient_waypoint;
    }
    if (!reuse) {
        /* A missing source replaces its reduced image with a fresh static gradient. */
        if (frame_alloc(&cache->pixels, width, height)) {
            return fail(error, n, "cannot allocate composition background");
        }
        if (dynamic) {
            Transform cover = fit(0, 0, source->width, source->height, 0, 0, width, height, true);
            screen_blit(&cache->pixels, source, cover);
            for (int j = 0; j < height; j++) {
                uint8_t *row = cache->pixels.data + (size_t)j * cache->pixels.stride;
                if (mirror) {
                    for (int i = 0; i < width / 2; i++) {
                        uint8_t swap[4];
                        memcpy(swap, row + i * 4, 4);
                        memcpy(row + i * 4, row + (width - 1 - i) * 4, 4);
                        memcpy(row + (width - 1 - i) * 4, swap, 4);
                    }
                }
                for (int i = 0; i < width; i++) {
                    for (int channel = 0; channel < 3; channel++) {
                        row[i * 4 + channel] = (uint8_t)lround(row[i * 4 + channel] * brightness);
                    }
                }
            }
            int radius = (int)fmax(1, lround((double)blur_radius / step));
            if (image_blur(&cache->pixels, radius, color, .25, error, n)) {
                frame_free(&cache->pixels);
                cache->type = 0;
                return -1;
            }
        } else {
            gradient_fill(&cache->pixels, cfg);
        }
        cache->type = type;
        cache->area_w = w;
        cache->area_h = h;
        cache->at = dynamic ? source->ts_ns : 0;
        cache->source_w = dynamic ? source->width : 0;
        cache->source_h = dynamic ? source->height : 0;
        cache->source_kind = source_kind;
        cache->mirror = mirror;
        cache->color = color;
        cache->radius = blur_radius;
        cache->brightness = brightness;
        cache->gradient_from = cfg->gradient_from;
        cache->gradient_via = cfg->gradient_via;
        cache->gradient_to = cfg->gradient_to;
        cache->gradient_via_enabled = cfg->gradient_via_enabled;
        cache->gradient_angle = cfg->gradient_angle;
        cache->gradient_waypoint = cfg->gradient_waypoint;
    }
    Transform enlarged = fit(0, 0, width, height, x, y, w, h, true);
    screen_blit(out, &cache->pixels, enlarged);
    return 0;
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
                 double opacity, const Clip *clip)
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
                clipped_pixel(f, clip, i, j, color, a * opacity);
            }
        }
    }
}
static void cursor_draw(Frame *f, double x, double y, int size, uint32_t color, const Clip *clip)
{
    if (size < 4) {
        size = 4;
    }
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size * 3 / 4; i++) {
            bool tip = i <= j / 2 && j < size * 3 / 4;
            bool stem = j >= size / 2 && j < size && i >= size / 4 && i <= size / 4 + size / 6;
            if (tip || stem) {
                clipped_pixel(f, clip, (int)x + i, (int)y + j,
                              (i == 0 || i == j / 2) ? 0x111111 : color, 1);
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
static void anchor_position(const char *anchor, int canvas_w, int canvas_h, int w, int h,
                            int margin_x, int margin_y, int *x, int *y)
{
    *x = strstr(anchor, "left")    ? margin_x
         : strstr(anchor, "right") ? canvas_w - margin_x - w
                                   : (canvas_w - w) / 2;
    *y = strstr(anchor, "top")      ? margin_y
         : strstr(anchor, "bottom") ? canvas_h - margin_y - h
                                    : (canvas_h - h) / 2;
    *x = (int)clampd(*x, 0, canvas_w - w);
    *y = (int)clampd(*y, 0, canvas_h - h);
}
static void alpha_blit(Frame *out, const Frame *source, int x, int y, double opacity)
{
    unsigned factor = (unsigned)lround(clampd(opacity, 0, 1) * 255);
    if (!source || !source->data || !factor) {
        return;
    }
    for (int j = 0; j < source->height; j++) {
        const uint8_t *row = source->data + (size_t)j * source->stride;
        for (int i = 0; i < source->width; i++) {
            int px = x + i, py = y + j;
            if (px < 0 || py < 0 || px >= out->width || py >= out->height) {
                continue;
            }
            const uint8_t *src = row + i * 4;
            unsigned alpha = (src[3] * factor + 127) / 255;
            if (!alpha) {
                continue;
            }
            uint8_t *dst = out->data + (size_t)py * out->stride + px * 4;
            if (alpha == 255) {
                memcpy(dst, src, 4);
            } else {
                for (int channel = 0; channel < 3; channel++) {
                    dst[channel] =
                        (uint8_t)((src[channel] * alpha + dst[channel] * (255 - alpha) + 127) /
                                  255);
                }
                dst[3] = 255;
            }
        }
    }
}
static void overlays_draw(Compositor *c, const Config *cfg, Frame *out)
{
    int x, y;
    if (cfg->logo_enabled && c->logo.data) {
        int w = c->logo_scaled.width, h = c->logo_scaled.height;
        anchor_position(cfg->logo_anchor, out->width, out->height, w, h, cfg->logo_margin_x,
                        cfg->logo_margin_y, &x, &y);
        alpha_blit(out, &c->logo_scaled, x, y, cfg->logo_opacity);
    }
    if (cfg->text_enabled) {
        const Frame *text = presentation_text_overlay(c->presentation);
        if (text && text->data) {
            anchor_position(cfg->text_anchor, out->width, out->height, text->width, text->height,
                            cfg->text_margin_x, cfg->text_margin_y, &x, &y);
            alpha_blit(out, text, x, y, cfg->text_opacity);
        }
    }
}
static void stage_screen_geometry(const Config *cfg, const Frame *screen, int camera_x,
                                  int camera_y, int camera_w, int camera_h, int *x, int *y, int *w,
                                  int *h)
{
    int margin = (int)clampd(cfg->screen_margin, 0, (fmin(cfg->width, cfg->height) - 1) / 2);
    double percent = cfg->screen_width_percent > 0 ? cfg->screen_width_percent : 78;
    double scale = fmin((double)cfg->width * percent / 100 / screen->width,
                        fmin((double)(cfg->width - 2 * margin) / screen->width,
                             (double)(cfg->height - 2 * margin) / screen->height));
    *w = (int)fmax(1, lround(screen->width * scale));
    *h = (int)fmax(1, lround(screen->height * scale));
    int horizontal = strstr(cfg->anchor, "left") ? -1 : strstr(cfg->anchor, "right") ? 1 : 0;
    int vertical = strstr(cfg->anchor, "top") ? -1 : strstr(cfg->anchor, "bottom") ? 1 : 0;
    if (!strcmp(cfg->anchor, "free")) {
        horizontal = camera_x + camera_w / 2.0 >= cfg->width / 2.0 ? 1 : -1;
        vertical = camera_y + camera_h / 2.0 >= cfg->height / 2.0 ? 1 : -1;
    }
    *x = horizontal < 0   ? cfg->width - margin - *w
         : horizontal > 0 ? margin
                          : (cfg->width - *w) / 2;
    *y = vertical < 0 ? cfg->height - margin - *h : vertical > 0 ? margin : (cfg->height - *h) / 2;
}
int compositor_render(Compositor *c, const Config *cfg, const Frame *screen, const Frame *camera,
                      const Cursor *cursor, bool record_target, Frame *out, char *err, size_t n)
{
    if (!c || !cfg || frame_alloc(out, cfg->width, cfg->height) < 0) {
        return fail(err, n, "cannot allocate composition canvas");
    }
    if (compositor_prepare(c, cfg, err, n)) {
        return -1;
    }
    uint64_t now = screen && screen->ts_ns ? screen->ts_ns : cast_now_ns();
    keys_expire(c, now, cfg->keys_timeout_ms);
    fill(out, cfg->pause_color);
    out->ts_ns = now;
    bool show_clicks =
        record_target ? cfg->annotations_record_clicks : cfg->annotations_virtual_clicks;
    bool show_keys = record_target ? cfg->annotations_record_keys : cfg->annotations_virtual_keys;
    bool show_screen = strcmp(cfg->layout, "camera") != 0,
         show_camera = strcmp(cfg->layout, "screen") != 0 && cfg->camera_visible;
    bool stage = !strcmp(cfg->layout, "stage");
    Transform t = {0};
    int sx = 0, sy = 0, sw = cfg->width, sh = cfg->height;
    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (stage && compositor_geometry(cfg, camera && camera->data ? camera->width : 640,
                                     camera && camera->data ? camera->height : 480, &cx, &cy, &cw,
                                     &ch, err, n)) {
        return -1;
    }
    if (stage && screen && screen->data) {
        stage_screen_geometry(cfg, screen, cx, cy, cw, ch, &sx, &sy, &sw, &sh);
    }
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
    if (!strcmp(cfg->layout, "split") || !strcmp(cfg->layout, "camera")) {
        int area = !strcmp(cfg->layout, "split") ? split : cfg->width;
        int ax = !strcmp(cfg->layout, "split") && !strcmp(cfg->split_side, "right")
                     ? cfg->width - split
                     : 0;
        if (background_draw(&c->camera_backdrop, cfg, cfg->camera_visible ? camera : NULL, 2,
                            cfg->mirror, cfg->camera_background, cfg->camera_background_color,
                            cfg->camera_background_blur_radius, cfg->camera_background_brightness,
                            out, ax, 0, area, cfg->height, err, n)) {
            return -1;
        }
    } else {
        frame_free(&c->camera_backdrop.pixels);
        c->camera_backdrop.type = 0;
    }
    if (show_screen && (stage || !strcmp(cfg->layout, "screen") || cfg->screen_radius ||
                        cfg->screen_border_width || !strcmp(cfg->screen_background, "gradient") ||
                        !strcmp(cfg->screen_background, "solid"))) {
        bool use_camera = !strcmp(cfg->background_source, "camera");
        const Frame *background = use_camera ? (cfg->camera_visible ? camera : NULL) : screen;
        if (background_draw(&c->screen_backdrop, cfg, background, use_camera ? 2 : 1,
                            use_camera && cfg->mirror, cfg->screen_background,
                            cfg->screen_background_color, cfg->screen_background_blur_radius,
                            cfg->screen_background_brightness, out, stage ? 0 : sx, 0,
                            stage ? cfg->width : sw, cfg->height, err, n)) {
            return -1;
        }
    } else {
        frame_free(&c->screen_backdrop.pixels);
        c->screen_backdrop.type = 0;
    }
    if (show_screen && screen && screen->data && screen->width > 0 && screen->height > 0) {
        t = screen_transform(c, cfg, screen, cursor, now, sx, sy, sw, sh);
        Clip clip = screen_layer_draw(out, screen, cfg, t);
        double x, y;
        if (cursor && cursor->valid && transform_point(t, cursor->x, cursor->y, &x, &y)) {
            if (cfg->cursor_highlight) {
                ring(out, x, y, cfg->cursor_size, 3, cfg->cursor_color, .65, &clip);
            }
            if (cfg->cursor) {
                cursor_draw(out, x, y, cfg->cursor_size, cfg->cursor_color, &clip);
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
                ring(out, x, y, cfg->click_radius * (.35 + .65 * p), 3, color, 1 - p, &clip);
            }
        }
    }
    if (show_camera && camera && camera->data) {
        int x, y, w, h;
        if (stage) {
            x = cx;
            y = cy;
            w = cw;
            h = ch;
        } else if (!strcmp(cfg->layout, "overlay")) {
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
    if (show_keys && cfg->keys) {
        keys_draw(c, cfg, out, now);
    }
    overlays_draw(c, cfg, out);
    return 0;
}
