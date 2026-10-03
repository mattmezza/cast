#include "cast.h"
#include "presentation_text.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t now = 1000000000;
uint64_t cast_now_ns(void)
{
    return now;
}

static Config config(int w, int h)
{
    Config c = {0};
    c.width = w;
    c.height = h;
    c.fps = 30;
    c.camera_visible = true;
    c.camera_width_percent = 25;
    c.split_ratio = 25;
    c.zoom_factor = 1;
    c.zoom_min = 1;
    c.zoom_max = 8;
    c.zoom_smoothing = 1;
    c.keys_timeout_ms = 1000;
    c.keys_font_size = 7;
    c.keys_color = 0xffffff;
    c.click_duration_ms = 500;
    c.click_radius = 10;
    c.click_left_color = 0xff0000;
    c.click_right_color = 0x00ff00;
    c.camera_background_color = 0x20252b;
    c.camera_background_blur_radius = 96;
    c.camera_background_brightness = .25;
    c.gradient_from = 0x101827;
    c.gradient_via = 0x26354a;
    c.gradient_to = 0x080b12;
    c.gradient_via_enabled = true;
    c.gradient_angle = 135;
    c.gradient_waypoint = 50;
    c.screen_width_percent = 78;
    c.screen_background_blur_radius = 96;
    c.screen_background_brightness = .25;
    c.annotations_virtual_keys = c.annotations_record_keys = true;
    c.annotations_virtual_clicks = c.annotations_record_clicks = true;
    snprintf(c.layout, sizeof(c.layout), "overlay");
    snprintf(c.shape, sizeof(c.shape), "rectangle");
    snprintf(c.anchor, sizeof(c.anchor), "bottom-right");
    snprintf(c.aspect, sizeof(c.aspect), "native");
    snprintf(c.fit, sizeof(c.fit), "contain");
    snprintf(c.split_side, sizeof(c.split_side), "left");
    snprintf(c.keys_position, sizeof(c.keys_position), "bottom-left");
    snprintf(c.camera_background, sizeof(c.camera_background), "blurred");
    return c;
}

static Frame source(int w, int h, uint32_t color, bool gradient)
{
    Frame f = {0};
    assert(frame_alloc(&f, w, h) == 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = f.data + y * f.stride + 4 * x;
            p[0] = gradient ? (uint8_t)x : color >> 16;
            p[1] = gradient ? (uint8_t)y : color >> 8;
            p[2] = gradient ? 0 : color;
            p[3] = 255;
        }
    }
    f.ts_ns = now;
    return f;
}

static uint32_t at(const Frame *f, int x, int y)
{
    const uint8_t *p = f->data + y * f->stride + x * 4;
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static void render(Compositor *c, Config *cfg, Frame *s, Frame *camera, Cursor *cur, bool record,
                   Frame *out)
{
    char error[CAST_ERR];
    if (s) {
        s->ts_ns = now;
    }
    assert(compositor_render(c, cfg, s, camera, cur, record, out, error, sizeof(error)) == 0);
}

static void test_frames(void)
{
    Frame a = source(20, 10, 0x123456, false), b = {0};
    assert(frame_copy(&b, &a) == 0);
    assert(b.data != a.data && at(&b, 12, 8) == 0x123456 && b.ts_ns == a.ts_ns);
    assert(frame_alloc(&b, 0, 10) < 0);
    frame_free(&a);
    frame_free(&b);
    assert(!a.data && !b.data);
}

static void test_geometry(void)
{
    Config cfg = config(1000, 600);
    cfg.margin = 20;
    int x, y, w, h;
    char error[CAST_ERR];
    assert(compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof(error)) == 0);
    assert(x + w == 980 && y + h == 580 && w == 250 && h == 141);
    cfg.camera_width_percent += 5;
    assert(compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof(error)) == 0);
    assert(w == 300 && x + w == 980 && y + h == 580);
    snprintf(cfg.shape, sizeof(cfg.shape), "circle");
    cfg.camera_width_percent = 100;
    assert(compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof(error)) == 0);
    assert(w == 560 && h == 560 && x == 420 && y == 20);
    snprintf(cfg.anchor, sizeof(cfg.anchor), "free");
    cfg.camera_x = -500;
    cfg.camera_y = 9000;
    assert(compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof(error)) == 0);
    assert(x == 20 && y == 20);
    cfg.camera_width_percent = 25;
    snprintf(cfg.shape, sizeof(cfg.shape), "rectangle");
    const char *anchors[] = {"top",        "bottom",        "left",        "right",
                             "top-center", "bottom-center", "center-left", "center-right"};
    for (size_t i = 0; i < sizeof anchors / sizeof *anchors; i++) {
        snprintf(cfg.anchor, sizeof cfg.anchor, "%s", anchors[i]);
        assert(!compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof error));
        int side = (int)i % 4;
        if (side < 2) {
            assert(x == (cfg.width - w) / 2);
            assert(y == (side == 0 ? cfg.margin : cfg.height - cfg.margin - h));
        } else {
            assert(y == (cfg.height - h) / 2);
            assert(x == (side == 2 ? cfg.margin : cfg.width - cfg.margin - w));
        }
        cfg.camera_width_percent += 5;
        int resized_x, resized_y, resized_w, resized_h;
        assert(!compositor_geometry(&cfg, 1920, 1080, &resized_x, &resized_y, &resized_w,
                                    &resized_h, error, sizeof error));
        if (side < 2) {
            assert(resized_x == (cfg.width - resized_w) / 2);
            assert(resized_y == (side == 0 ? cfg.margin : cfg.height - cfg.margin - resized_h));
        } else {
            assert(resized_y == (cfg.height - resized_h) / 2);
            assert(resized_x == (side == 2 ? cfg.margin : cfg.width - cfg.margin - resized_w));
        }
        cfg.camera_width_percent -= 5;
    }
    cfg.margin = 300;
    assert(compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof(error)) < 0);
}

static void test_text_templates(void)
{
    struct tm captured = {.tm_year = 126,
                          .tm_mon = 9,
                          .tm_mday = 2,
                          .tm_hour = 14,
                          .tm_min = 37,
                          .tm_sec = 59,
                          .tm_wday = 5,
                          .tm_yday = 274};
    char output[256], error[CAST_ERR];
    const char *input = "{{Meet}} {date} {time} {datetime} {date:%d/%m/%Y} {time:%Hh%M}";
    assert(!presentation_template_expand(input, &captured, output, sizeof output, error,
                                         sizeof error));
    assert(!strcmp(output, "{Meet} 2026-10-02 14:37:59 2026-10-02 14:37:59 02/10/2026 14h37"));
    assert(!presentation_template_expand("café {{date}}", &captured, output, sizeof output, error,
                                         sizeof error));
    assert(!strcmp(output, "café {date}"));
    assert(!presentation_template_expand("$(date) `time`", &captured, output, sizeof output, error,
                                         sizeof error));
    assert(!strcmp(output, "$(date) `time`")); /* Plain text, never shell execution. */
    const char *invalid[] = {"{unknown}",     "{date",     "date}",
                             "{time:}",       "{date:%Q}", "{date:%999999999Y}",
                             "{date:{time}}", "\xc0\x80",  "\xed\xa0\x80"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        assert(presentation_template_validate(invalid[i], error, sizeof error) < 0);
        assert(error[0]);
    }
    assert(presentation_template_validate("{date:%1000Y}{time:%1000H}", error, sizeof error) < 0);
    assert(presentation_template_validate("{date:%800Y%800m}", error, sizeof error) < 0);
    char long_template[256];
    memset(long_template, 'x', 200);
    snprintf(long_template + 200, sizeof long_template - 200, "{date:%%1400Y}");
    assert(presentation_template_validate(long_template, error, sizeof error) < 0);
    assert(!presentation_template_validate("{date:%1400Y}", error, sizeof error));
    assert(!presentation_template_validate(input, error, sizeof error));
    struct {
        char value[8];
        unsigned char guard[8];
    } bounded = {.guard = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa}};
    assert(presentation_template_expand("{datetime}", &captured, bounded.value,
                                        sizeof bounded.value, error, sizeof error) < 0);
    for (size_t i = 0; i < sizeof bounded.guard; i++) {
        assert(bounded.guard[i] == 0xaa);
    }
}

static void assert_solid(const Frame *frame, uint32_t color)
{
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < frame->width; x++) {
            assert(at(frame, x, y) == color);
            assert(frame->data[(size_t)y * frame->stride + x * 4 + 3] == 255);
        }
    }
}

static void test_styled_text(void)
{
    Config cfg = config(320, 180);
    strcpy(cfg.pause_font, "Noto Sans");
    strcpy(cfg.blur_font, "Noto Sans");
    cfg.pause_foreground = cfg.blur_foreground = 0xffffff;
    cfg.pause_title_size = cfg.blur_title_size = 48;
    cfg.pause_subtitle_size = cfg.blur_subtitle_size = 24;
    cfg.pause_color = 0x123456;
    cfg.blur_radius = 16;
    cfg.blur_opacity = .5;
    Compositor *c = compositor_create();
    assert(c);
    Frame out = {0}, expected = {0};
    char error[CAST_ERR];
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert_solid(&out, cfg.pause_color); /* Both text fields are optional. */
    strcpy(cfg.pause_subtitle, "Réunion – café");
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    int left = out.width, right = 0, top = out.height, bottom = 0;
    bool antialiased = false, opaque = false;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            uint32_t color = at(&out, x, y);
            if (color != cfg.pause_color) {
                if (x < left) {
                    left = x;
                }
                if (x > right) {
                    right = x;
                }
                if (y < top) {
                    top = y;
                }
                if (y > bottom) {
                    bottom = y;
                }
                opaque |= color == cfg.pause_foreground;
                antialiased |= color != cfg.pause_foreground;
            }
        }
    }
    assert(left < right && top < bottom && opaque && antialiased);
    assert(abs(left + right + 1 - out.width) <= 1);
    assert(abs(top + bottom + 1 - out.height) <= 1);
    strcpy(cfg.pause_text, "Title");
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!frame_copy(&expected, &out));
    Config bad = cfg;
    strcpy(bad.pause_font, "serif");
    strcpy(bad.blur_font, "Noto Sans:file=/no/such/cast-font.ttf");
    assert(compositor_prepare(c, &bad, error, sizeof error) < 0);
    assert(strstr(error, "font file"));
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * out.height));
    /* Expansion and literal messages use exactly the same styled renderer. */
    strcpy(cfg.pause_text, "{date:Title}");
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * out.height));
    cfg.width = 80;
    cfg.height = 40;
    strcpy(cfg.pause_text, "A very long title with café and accented UTF-8");
    strcpy(cfg.pause_subtitle, "A second line");
    cfg.pause_title_size = cfg.pause_subtitle_size = 256;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    for (int x = 0; x < out.width; x++) {
        assert(at(&out, x, 0) == cfg.pause_color);
        assert(at(&out, x, out.height - 1) == cfg.pause_color);
    }
    cfg.pause_text[0] = cfg.pause_subtitle[0] = 0;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert_solid(&out, cfg.pause_color); /* No cached previous message remains. */
    frame_free(&out);
    frame_free(&expected);
    compositor_destroy(c);
}

typedef struct {
    int first, last;
} InkBand;
static unsigned ink_bands(const Frame *frame, uint32_t background, InkBand *bands, unsigned limit)
{
    unsigned count = 0;
    bool previous = false;
    for (int y = 0; y < frame->height; y++) {
        bool ink = false;
        for (int x = 0; x < frame->width; x++) {
            ink |= at(frame, x, y) != background;
        }
        if (ink && !previous) {
            assert(count < limit);
            bands[count++] = (InkBand){.first = y, .last = y};
        } else if (ink) {
            bands[count - 1].last = y;
        }
        previous = ink;
    }
    return count;
}
static void test_footer_and_gap(void)
{
    Config cfg = config(480, 360);
    strcpy(cfg.pause_font, "Noto Sans");
    strcpy(cfg.blur_font, "Noto Sans");
    cfg.pause_foreground = cfg.blur_foreground = 0xffffff;
    cfg.pause_color = cfg.blur_color = 0x102030;
    cfg.pause_title_size = cfg.pause_subtitle_size = 32;
    cfg.pause_footer_size = 18;
    strcpy(cfg.pause_text, "MMMM");
    strcpy(cfg.pause_subtitle, "MMMM");
    Compositor *c = compositor_create();
    assert(c);
    Frame out = {0}, expected = {0};
    char error[CAST_ERR];
    InkBand bands[4];
    cfg.pause_text_gap = 0;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.pause_color, bands, 4) == 1);
    cfg.pause_text_gap = 27;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.pause_color, bands, 4) == 2);
    assert(bands[1].first - bands[0].last - 1 == 27);
    assert(abs(bands[0].first + bands[1].last + 1 - cfg.height) <= 1);
    assert(!frame_copy(&expected, &out));

    strcpy(cfg.pause_footer, "Réunion – café");
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.pause_color, bands, 4) == 3);
    int margin = 30; /* min(48, min(width,height)/12), shared by all text edges. */
    assert(bands[2].last == cfg.height - margin - 1);
    assert(bands[2].first > bands[1].last + margin);
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * (bands[1].last + 1)));
    assert(!frame_copy(&expected, &out));
    strcpy(cfg.pause_footer, "{date:Réunion – café}");
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * out.height));

    strcpy(cfg.blur_title, cfg.pause_text);
    strcpy(cfg.blur_subtitle, cfg.pause_subtitle);
    strcpy(cfg.blur_footer, cfg.pause_footer);
    cfg.blur_title_size = cfg.pause_title_size;
    cfg.blur_subtitle_size = cfg.pause_subtitle_size;
    cfg.blur_footer_size = cfg.pause_footer_size;
    cfg.blur_text_gap = cfg.pause_text_gap;
    cfg.blur_radius = 8;
    cfg.blur_opacity = 1;
    assert(!compositor_blur(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * out.height));
    cfg.blur_text_gap = 9;
    assert(!compositor_blur(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.blur_color, bands, 4) == 3);
    assert(bands[1].first - bands[0].last - 1 == 9);
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * out.height));

    /* Every field uses one captured time, even when a second changes during draw. */
    cfg.pause_title_size = cfg.pause_subtitle_size = cfg.pause_footer_size = 24;
    strcpy(cfg.pause_text, "{time:%H:%M:%S}");
    strcpy(cfg.pause_subtitle, cfg.pause_text);
    strcpy(cfg.pause_footer, cfg.pause_text);
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.pause_color, bands, 4) == 3);
    for (unsigned i = 1; i < 3; i++) {
        assert(bands[i].last - bands[i].first == bands[0].last - bands[0].first);
        assert(!memcmp(out.data + (size_t)bands[i].first * out.stride,
                       out.data + (size_t)bands[0].first * out.stride,
                       (size_t)(bands[0].last - bands[0].first + 1) * out.stride));
    }

    cfg.pause_text[0] = cfg.pause_subtitle[0] = 0;
    strcpy(cfg.pause_footer, "Footer");
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.pause_color, bands, 4) == 1);
    assert(bands[0].last == cfg.height - margin - 1);
    cfg.blur_title[0] = cfg.blur_subtitle[0] = 0;
    strcpy(cfg.blur_footer, cfg.pause_footer);
    cfg.blur_footer_size = cfg.pause_footer_size;
    cfg.blur_radius = 8;
    cfg.blur_opacity = 1;
    assert(!frame_copy(&expected, &out));
    assert(!compositor_blur(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, expected.data, (size_t)out.stride * out.height));
    cfg.pause_footer[0] = 0;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert_solid(&out, cfg.pause_color);

    /* A constrained canvas scales an independently configured gap with all fonts. */
    cfg.width = 80;
    cfg.height = 40;
    strcpy(cfg.pause_text, "MMMM");
    strcpy(cfg.pause_subtitle, "MMMM");
    strcpy(cfg.pause_footer, "MMMM");
    cfg.pause_title_size = cfg.pause_subtitle_size = cfg.pause_footer_size = 256;
    cfg.pause_text_gap = 512;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(ink_bands(&out, cfg.pause_color, bands, 4) == 3);
    assert(bands[0].first > 0 && bands[2].last < out.height - 1);
    assert(bands[1].first > bands[0].last && bands[2].first > bands[1].last);
    assert(bands[1].first - bands[0].last - 1 < cfg.pause_text_gap);
    /* Even text too tall at the minimum font size stays inside disjoint regions. */
    for (unsigned i = 0; i < 60; i++) {
        cfg.pause_text[i * 2] = cfg.pause_subtitle[i * 2] = cfg.pause_footer[i * 2] = 'M';
        cfg.pause_text[i * 2 + 1] = cfg.pause_subtitle[i * 2 + 1] = cfg.pause_footer[i * 2 + 1] =
            '\n';
    }
    cfg.pause_text[120] = cfg.pause_subtitle[120] = cfg.pause_footer[120] = 0;
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    for (int x = 0; x < out.width; x++) {
        assert(at(&out, x, 0) == cfg.pause_color);
        assert(at(&out, x, out.height - 1) == cfg.pause_color);
    }
    frame_free(&out);
    frame_free(&expected);
    compositor_destroy(c);
}

static void test_font_replacement(void)
{
    Config cfg = config(480, 360);
    strcpy(cfg.pause_font, "Noto Sans");
    strcpy(cfg.blur_font, "Noto Sans");
    strcpy(cfg.pause_text, "Family Wiii 0123");
    strcpy(cfg.blur_title, cfg.pause_text);
    cfg.pause_foreground = cfg.blur_foreground = 0xffffff;
    cfg.pause_color = cfg.blur_color = 0x102030;
    cfg.pause_title_size = cfg.blur_title_size = 48;
    cfg.blur_radius = 8;
    cfg.blur_opacity = 1;
    Compositor *c = compositor_create();
    assert(c);
    Frame out = {0}, sans = {0};
    char error[CAST_ERR];
    assert(!compositor_prepare(c, &cfg, error, sizeof error));
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!frame_copy(&sans, &out));
    strcpy(cfg.pause_font, "serif");
    assert(!compositor_prepare(c, &cfg, error, sizeof error));
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(memcmp(out.data, sans.data, (size_t)out.stride * out.height));
    strcpy(cfg.pause_font, "Noto Sans");
    assert(!compositor_prepare(c, &cfg, error, sizeof error));
    assert(!compositor_neutral(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, sans.data, (size_t)out.stride * out.height));
    strcpy(cfg.blur_font, "monospace");
    assert(!compositor_prepare(c, &cfg, error, sizeof error));
    assert(!compositor_blur(c, &cfg, &out, error, sizeof error));
    assert(memcmp(out.data, sans.data, (size_t)out.stride * out.height));
    strcpy(cfg.blur_font, "Noto Sans");
    assert(!compositor_prepare(c, &cfg, error, sizeof error));
    assert(!compositor_blur(c, &cfg, &out, error, sizeof error));
    assert(!memcmp(out.data, sans.data, (size_t)out.stride * out.height));
    frame_free(&out);
    frame_free(&sans);
    compositor_destroy(c);
}

static void test_blurred_frames(void)
{
    Config cfg = config(100, 60);
    cfg.blur_radius = 8;
    cfg.blur_opacity = 0;
    cfg.blur_foreground = 0xffffff;
    cfg.blur_title_size = 24;
    cfg.blur_subtitle_size = 12;
    strcpy(cfg.blur_font, "Noto Sans");
    strcpy(cfg.pause_font, "Noto Sans");
    Compositor *c = compositor_create();
    assert(c);
    char error[CAST_ERR];
    Frame uniform = source(100, 60, 0x123456, false);
    assert(!compositor_blur(c, &cfg, &uniform, error, sizeof error));
    assert_solid(&uniform, 0x123456);
    cfg.blur_opacity = 1;
    cfg.blur_color = 0xabcdef;
    assert(!compositor_blur(c, &cfg, &uniform, error, sizeof error));
    assert_solid(&uniform, cfg.blur_color);
    Frame impulse = source(100, 60, 0, false), narrow = {0}, broad = {0};
    uint8_t *center = impulse.data + 30 * impulse.stride + 50 * 4;
    center[0] = center[1] = center[2] = 255;
    assert(!frame_copy(&narrow, &impulse) && !frame_copy(&broad, &impulse));
    cfg.blur_opacity = 0;
    cfg.blur_radius = 1;
    assert(!compositor_blur(c, &cfg, &narrow, error, sizeof error));
    cfg.blur_radius = 4;
    assert(!compositor_blur(c, &cfg, &broad, error, sizeof error));
    assert(at(&narrow, 50, 30) < 0xffffff && at(&narrow, 50, 30) > 0);
    assert(at(&narrow, 50, 30) > at(&broad, 50, 30));
    assert(at(&narrow, 54, 30) == 0);
    assert(at(&impulse, 50, 30) == 0xffffff); /* The selected raw/frozen frame stays untouched. */
    Frame a = source(100, 60, 0x123456, false), b = source(100, 60, 0x123456, false);
    strcpy(cfg.blur_title, "Title");
    strcpy(cfg.blur_subtitle, "Subtitle");
    cfg.blur_opacity = 1;
    cfg.blur_color = cfg.pause_color = 0x203040;
    cfg.pause_foreground = cfg.blur_foreground;
    cfg.pause_title_size = cfg.blur_title_size;
    cfg.pause_subtitle_size = cfg.blur_subtitle_size;
    strcpy(cfg.pause_text, cfg.blur_title);
    strcpy(cfg.pause_subtitle, cfg.blur_subtitle);
    assert(!compositor_blur(c, &cfg, &a, error, sizeof error));
    assert(!compositor_neutral(c, &cfg, &b, error, sizeof error));
    assert(!memcmp(a.data, b.data, (size_t)a.stride * a.height));
    assert(a.ts_ns == now); /* Blur preserves the selected lane's timestamp. */
    cfg.blur_title[0] = cfg.blur_subtitle[0] = 0;
    cfg.blur_opacity = .5;
    cfg.blur_color = 0x000000;
    Frame large = source(1280, 720, 0x2468ac, false);
    assert(!compositor_blur(c, &cfg, &large, error, sizeof error));
    assert_solid(&large, 0x123456); /* Bounded downsample path preserves uniform colour. */
    cfg.blur_radius = 0;
    assert(compositor_blur(c, &cfg, &large, error, sizeof error) < 0);
    frame_free(&uniform);
    frame_free(&impulse);
    frame_free(&narrow);
    frame_free(&broad);
    frame_free(&a);
    frame_free(&b);
    frame_free(&large);
    compositor_destroy(c);
}

static void test_camera_backgrounds(void)
{
    Config cfg = config(240, 180);
    strcpy(cfg.layout, "camera");
    Frame screen = source(240, 180, 0x123456, false);
    Frame camera = source(160, 90, 0xff0000, false), out = {0}, first = {0};
    Compositor *c = compositor_create();
    assert(c);
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 120, 90) == 0xff0000); /* Full-quality foreground geometry is preserved. */
    uint32_t background = at(&out, 120, 5);
    assert(background != cfg.pause_color && background != 0xff0000);
    assert((background >> 16) > (background & 255));
    assert(!frame_copy(&first, &out));
    render(c, &cfg, &screen, &camera, NULL, true,
           &out); /* Same snapshot can reuse its processed backdrop. */
    assert(!memcmp(first.data, out.data, (size_t)out.stride * out.height));
    for (int y = 0; y < camera.height; y++) {
        for (int x = 0; x < camera.width; x++) {
            uint8_t *pixel = camera.data + (size_t)y * camera.stride + x * 4;
            pixel[0] = pixel[1] = 0;
            pixel[2] = 255;
        }
    }
    camera.ts_ns++;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 120, 90) == 0x0000ff);
    assert(at(&out, 120, 5) !=
           background); /* Every new camera frame repaints the dynamic backdrop. */
    assert((at(&out, 120, 5) & 255) > (at(&out, 120, 5) >> 16));
    cfg.camera_visible = false;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(!frame_copy(&first, &out));
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(!memcmp(first.data, out.data, (size_t)out.stride * out.height));
    assert(at(&out, 120, 5) != at(&out, 120, 175)); /* Fresh static gradient fallback. */
    cfg.camera_visible = true;
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(!memcmp(first.data, out.data, (size_t)out.stride * out.height));
    strcpy(cfg.camera_background, "solid");
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert_solid(&out, cfg.camera_background_color);
    /* The gradient fills only the split camera slot; screen letterboxing stays neutral. */
    strcpy(cfg.layout, "split");
    strcpy(cfg.camera_background, "gradient");
    cfg.split_ratio = 50;
    for (int right = 0; right < 2; right++) {
        strcpy(cfg.split_side, right ? "right" : "left");
        render(c, &cfg, &screen, NULL, NULL, false, &out);
        int camera_x = right ? 180 : 60, screen_x = right ? 60 : 180;
        assert(at(&out, camera_x, 2) != at(&out, camera_x, 177));
        assert(at(&out, screen_x, 2) == cfg.pause_color);
        assert(at(&out, screen_x, 90) == 0x123456);
    }
    /* Overlay/screen composition is unaffected, and clearing rejects an old cached backdrop. */
    strcpy(cfg.layout, "screen");
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert_solid(&out, 0x123456);
    compositor_clear(c);
    strcpy(cfg.layout, "camera");
    strcpy(cfg.camera_background, "blurred");
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 120, 5) != background);
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&out);
    frame_free(&first);
    compositor_destroy(c);
}

static void test_layout_and_masks(void)
{
    Config cfg = config(100, 100);
    Frame screen = source(100, 100, 0x112233, false);
    Frame camera = source(100, 100, 0xaabbcc, false), out = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 10, 10) == 0x112233 && at(&out, 90, 90) == 0xaabbcc);
    cfg.camera_visible = false;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 90, 90) == 0x112233);
    cfg.camera_visible = true;
    snprintf(cfg.shape, sizeof(cfg.shape), "circle");
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 75, 75) == 0x112233 && at(&out, 87, 87) == 0xaabbcc);
    snprintf(cfg.shape, sizeof(cfg.shape), "rounded");
    cfg.radius = 8;
    cfg.border_width = 3;
    cfg.border_color = 0xff0000;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 75, 75) == 0x112233 && at(&out, 87, 76) == 0xff0000);
    assert(at(&out, 87, 87) == 0xaabbcc);
    snprintf(cfg.layout, sizeof(cfg.layout), "split");
    snprintf(cfg.shape, sizeof(cfg.shape), "rectangle");
    cfg.border_width = 0;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 10, 50) == 0xaabbcc && at(&out, 50, 50) == 0x112233);
    snprintf(cfg.split_side, sizeof(cfg.split_side), "right");
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 90, 50) == 0xaabbcc && at(&out, 50, 50) == 0x112233);
    snprintf(cfg.layout, sizeof(cfg.layout), "camera");
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 10, 10) == 0xaabbcc);
    compositor_destroy(c);
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&out);
}

static void test_crop_and_fit(void)
{
    Config cfg = config(100, 100);
    snprintf(cfg.layout, sizeof(cfg.layout), "screen");
    Frame screen = source(100, 50, 0, true), camera = {0}, out = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 0, 24) == 0 && at(&out, 20, 40) == (20u << 16 | 15u << 8));
    snprintf(cfg.fit, sizeof(cfg.fit), "cover");
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert((at(&out, 0, 0) >> 16) == 25 && (at(&out, 99, 99) >> 16) == 74);
    camera = source(100, 50, 0, true);
    snprintf(cfg.layout, sizeof(cfg.layout), "camera");
    snprintf(cfg.aspect, sizeof(cfg.aspect), "1:1");
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert((at(&out, 0, 50) >> 16) == 25 && (at(&out, 99, 50) >> 16) == 74);
    cfg.crop_x = 1000;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert((at(&out, 0, 50) >> 16) == 50 && (at(&out, 99, 50) >> 16) == 99);
    cfg.mirror = true;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert((at(&out, 0, 50) >> 16) == 99 && (at(&out, 99, 50) >> 16) == 50);
    compositor_destroy(c);
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&out);
}

static void test_zoom_and_annotations(void)
{
    Config cfg = config(100, 50);
    snprintf(cfg.layout, sizeof(cfg.layout), "screen");
    cfg.zoom_factor = 2;
    cfg.zoom_transition_ms = 250;
    Cursor cur = {75, 25, true, now};
    Frame screen = source(100, 50, 0, true), out = {0}, plain = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, &screen, NULL, &cur, false, &out);
    assert((at(&out, 50, 25) >> 16) == 50);
    now += 125000000;
    render(c, &cfg, &screen, NULL, &cur, false, &out);
    unsigned mid = at(&out, 50, 25) >> 16;
    assert(mid >= 65 && mid <= 67);
    now += 125000000;
    render(c, &cfg, &screen, NULL, &cur, false, &out);
    assert((at(&out, 50, 25) >> 16) == 75);
    cur.x = 20;
    render(c, &cfg, &screen, NULL, &cur, false, &out);
    assert((at(&out, 50, 25) >> 16) == 75); /* Locked target. */
    cfg.zoom_follow = true;
    cfg.zoom_deadzone = 10;
    now += 34000000;
    render(c, &cfg, &screen, NULL, &cur, false, &out);
    assert((at(&out, 50, 25) >> 16) == 30);
    cur.valid = false;
    render(c, &cfg, &screen, NULL, &cur, false, &out);
    assert((at(&out, 50, 25) >> 16) == 30); /* Leaving source holds view. */
    cfg.clicks = true;
    compositor_click(c, 30, 25, 1, now);
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert((at(&out, 53, 25) >> 16) > 150); /* Ring shares screen transform. */
    cfg.annotations_record_clicks = false;
    render(c, &cfg, &screen, NULL, NULL, true, &plain);
    assert((at(&plain, 53, 25) >> 16) < 40);
    compositor_clear(c);
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 53, 25) == at(&plain, 53, 25));
    cfg.keys = true;
    compositor_key(c, "Ctrl+A", now, cfg.keys_timeout_ms);
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 9, 35) == 0xffffff);
    cfg.annotations_record_keys = false;
    render(c, &cfg, &screen, NULL, NULL, true, &plain);
    assert(at(&plain, 9, 35) != 0xffffff);
    now += 1100000000;
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 9, 35) != 0xffffff);
    compositor_destroy(c);
    frame_free(&screen);
    frame_free(&out);
    frame_free(&plain);
}

static void test_neutral(void)
{
    Config cfg = config(100, 50);
    cfg.pause_color = 0x334455;
    snprintf(cfg.pause_text, sizeof(cfg.pause_text), "Paused");
    Frame out = {0};
    Compositor *c = compositor_create();
    char error[CAST_ERR];
    assert(compositor_neutral(c, &cfg, &out, error, sizeof error) == 0);
    assert(at(&out, 0, 0) == 0x334455);
    bool found = false;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            found |= at(&out, x, y) == 0xffffff;
        }
    }
    assert(found && out.ts_ns == now);
    frame_free(&out);
    compositor_destroy(c);
}

/* Compare the synthetic input sequence with literal display labels, so repeat
 * counts, retained rows, ordering and expiry are checked in the rendered frame. */
static void expect_keys(Compositor *c, Config *cfg, const char **labels, size_t count)
{
    Compositor *reference = compositor_create();
    assert(reference);
    for (size_t i = 0; i < count; i++) {
        compositor_key(reference, labels[i], now, cfg->keys_timeout_ms);
    }
    Frame actual = {0}, expected = {0};
    render(c, cfg, NULL, NULL, NULL, false, &actual);
    render(reference, cfg, NULL, NULL, NULL, false, &expected);
    assert(memcmp(actual.data, expected.data, (size_t)actual.stride * actual.height) == 0);
    frame_free(&actual);
    frame_free(&expected);
    compositor_destroy(reference);
}

static void test_key_history(void)
{
    Config cfg = config(320, 256);
    cfg.keys = true;
    cfg.keys_timeout_ms = 3000;
    cfg.margin = 12;
    cfg.keys_background = 0x123456;
    cfg.pause_color = 0x345678;
    Compositor *c = compositor_create();
    assert(c);
    now += 1000000000;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"j"}, 1);
    now += 50000000;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"jx2"}, 1);
    Frame out = {0};
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    /* Lowercase j and literal lowercase x must remain visually distinct from
     * the original uppercase-only bitmap font. Verify the pixels directly. */
    const uint8_t bits[3][7] = {
        {2, 0, 6, 2, 2, 18, 12}, {0, 0, 17, 10, 4, 10, 17}, {14, 17, 1, 2, 4, 8, 31}};
    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < 7; y++) {
            for (int x = 0; x < 5; x++) {
                bool white = at(&out, cfg.margin + 8 + 6 * ch + x,
                                cfg.height - cfg.margin - 23 + 8 + y) == cfg.keys_color;
                assert(white == !!(bits[ch][y] & (1 << (4 - x))));
            }
        }
    }
    now += 50000000;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"jx3"}, 1);
    now += 25000000;
    compositor_key(c, "k", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"jx3", "k"}, 2);
    now += 25000000;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"jx3", "k", "j"}, 3);
    now += 25000000;
    compositor_key(c, "Ctrl+S", now, cfg.keys_timeout_ms);
    now += 25000000;
    compositor_key(c, "Ctrl+S", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"jx3", "k", "j", "Ctrl+Sx2"}, 4);
    /* A burst between frames still counts every adjacent observation. */
    compositor_clear(c);
    for (int i = 0; i < 12; i++) {
        compositor_key(c, "j", now + (uint64_t)i * 1000000, cfg.keys_timeout_ms);
    }
    now += 12000000;
    expect_keys(c, &cfg, (const char *[]){"jx12"}, 1);
    /* The timeout refreshes only the repeated row, not unrelated history. */
    compositor_clear(c);
    uint64_t started = now;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    now += 1000000000;
    compositor_key(c, "k", now, cfg.keys_timeout_ms);
    now += 1000000000;
    compositor_key(c, "k", now, cfg.keys_timeout_ms);
    now = started + 2999999999;
    expect_keys(c, &cfg, (const char *[]){"j", "kx2"}, 2);
    now++;
    expect_keys(c, &cfg, (const char *[]){"kx2"}, 1);
    now = started + 5000000000;
    expect_keys(c, &cfg, NULL, 0);
    /* Expiry is also applied on input even if no frame was rendered. */
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    now += 3000000000;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"j"}, 1);
    now += 2900000000;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    now += 200000000;
    expect_keys(c, &cfg, (const char *[]){"jx2"}, 1);
    now += 2800000000;
    expect_keys(c, &cfg, NULL, 0);
    /* Bursts retain the latest eight labels, with predictable oldest eviction. */
    compositor_clear(c);
    for (char ch = 'a'; ch <= 'j'; ch++) {
        char label[2] = {ch, 0};
        compositor_key(c, label, now, cfg.keys_timeout_ms);
    }
    const char *last_eight[] = {"c", "d", "e", "f", "g", "h", "i", "j"};
    expect_keys(c, &cfg, last_eight, 8);
    for (int top = 0; top < 2; top++) {
        for (int side = 0; side < 3; side++) {
            snprintf(cfg.keys_position, sizeof(cfg.keys_position), "%s-%s", top ? "top" : "bottom",
                     side == 0   ? "left"
                     : side == 1 ? "center"
                                 : "right");
            cfg.height = 50; /* Only the most recent two rows fit. */
            expect_keys(c, &cfg, (const char *[]){"i", "j"}, 2);
        }
    }
    /* Privacy/source/keys-clear barriers erase the whole history and counts. */
    compositor_clear(c);
    cfg.height = 256;
    expect_keys(c, &cfg, NULL, 0);
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"j"}, 1);
    cfg.annotations_record_keys = false;
    Frame plain = {0};
    render(c, &cfg, NULL, NULL, NULL, true, &plain);
    for (int y = 0; y < plain.height; y++) {
        for (int x = 0; x < plain.width; x++) {
            assert(at(&plain, x, y) == cfg.pause_color);
        }
    }
    cfg.annotations_virtual_keys = false;
    now += 3000000000;
    render(c, &cfg, NULL, NULL, NULL, false, &out); /* Expire while hidden. */
    cfg.annotations_virtual_keys = true;
    compositor_key(c, "j", now, cfg.keys_timeout_ms);
    expect_keys(c, &cfg, (const char *[]){"j"}, 1);
    compositor_destroy(c);
    frame_free(&out);
    frame_free(&plain);
}

static void test_key_canvas_bounds(void)
{
    Config cfg = config(80, 32);
    cfg.keys = true;
    cfg.keys_font_size = 96;
    cfg.keys_background = 0xff0000;
    cfg.margin = 500;
    strcpy(cfg.keys_position, "bottom-right");
    char long_key[CAST_TEXT + 100];
    memset(long_key, 'j', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = 0;
    Compositor *c = compositor_create();
    assert(c);
    compositor_key(c, long_key, now, cfg.keys_timeout_ms);
    compositor_key(c, long_key, now, cfg.keys_timeout_ms);
    Frame out = {0};
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 4, 2) != 0); /* Clamped background remains on the canvas. */
    /* Every box dimension and text scale fits, including the repeat suffix. */
    expect_keys(c, &cfg, (const char *[]){"jjjx2"}, 1);
    cfg.width = 8;
    cfg.height = 8;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    cfg.width = cfg.height = 1;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == cfg.pause_color);
    compositor_destroy(c);
    frame_free(&out);
}

static void test_follow_lane_consistency(void)
{
    Config cfg = config(100, 50);
    strcpy(cfg.layout, "screen");
    cfg.zoom_factor = 2;
    cfg.zoom_follow = true;
    cfg.zoom_smoothing = .25;
    Cursor cursor = {75, 25, true, now};
    Frame screen = source(100, 50, 0, true), virtual = {0}, record = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, &screen, NULL, &cursor, false, &virtual);
    cursor.x = 20;
    now += 34000000;
    render(c, &cfg, &screen, NULL, &cursor, false, &virtual);
    render(c, &cfg, &screen, NULL, &cursor, true, &record);
    assert(memcmp(virtual.data, record.data, (size_t)virtual.stride * virtual.height) == 0);
    now += 34000000;
    render(c, &cfg, &screen, NULL, &cursor, true, &record);
    render(c, &cfg, &screen, NULL, &cursor, false, &virtual);
    assert(memcmp(virtual.data, record.data, (size_t)virtual.stride * virtual.height) == 0);
    compositor_destroy(c);
    frame_free(&screen);
    frame_free(&virtual);
    frame_free(&record);
}

static void test_screen_sampling(void)
{
    Config cfg = config(123, 79);
    strcpy(cfg.layout, "screen");
    Frame screen = source(317, 113, 0, true), out = {0};
    Compositor *c = compositor_create();
    for (int cover = 0; cover <= 1; cover++) {
        strcpy(cfg.fit, cover ? "cover" : "contain");
        render(c, &cfg, &screen, NULL, NULL, false, &out);
        double scale = cover ? fmax(123.0 / 317, 79.0 / 113) : fmin(123.0 / 317, 79.0 / 113);
        double sw = cover ? 123 / scale : 317, sh = cover ? 79 / scale : 113;
        double sx = (317 - sw) / 2, sy = (113 - sh) / 2;
        double dw = cover ? 123 : sw * scale, dh = cover ? 79 : sh * scale;
        double dx = (123 - dw) / 2, dy = (79 - dh) / 2;
        for (int y = 0; y < out.height; y++) {
            for (int x = 0; x < out.width; x++) {
                uint32_t expected = 0;
                if (x >= ceil(dx) && x < ceil(dx + dw) && y >= ceil(dy) && y < ceil(dy + dh)) {
                    int ix = (int)floor(sx + (x + .5 - dx) / dw * sw);
                    int iy = (int)floor(sy + (y + .5 - dy) / dh * sh);
                    ix = ix < 0 ? 0 : ix > 316 ? 316 : ix;
                    iy = iy < 0 ? 0 : iy > 112 ? 112 : iy;
                    expected = (uint32_t)(uint8_t)ix << 16 | (uint32_t)(uint8_t)iy << 8;
                }
                assert(at(&out, x, y) == expected);
            }
        }
    }
    frame_free(&screen);
    screen = source(123, 79, 0x123456, false);
    screen.data[3] = 7;
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == 0x123456 && out.data[3] == 255);
    compositor_destroy(c);
    frame_free(&screen);
    frame_free(&out);
}

static void test_stage_geometry(void)
{
    Config cfg = config(400, 240);
    strcpy(cfg.layout, "stage");
    strcpy(cfg.screen_background, "solid");
    cfg.screen_background_color = 0x0000aa;
    cfg.camera_width_percent = 25;
    cfg.margin = 10;
    cfg.screen_margin = 20;
    cfg.screen_width_percent = 50;
    Frame screen = source(200, 100, 0xaa0000, false);
    Frame camera = source(100, 75, 0x00aa00, false), out = {0};
    Compositor *c = compositor_create();
    const char *anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right",
                             "top",      "bottom",    "left",        "right"};
    const int positions[][2] = {{180, 120}, {20, 120}, {180, 20}, {20, 20},
                                {100, 120}, {100, 20}, {180, 70}, {20, 70}};
    char error[CAST_ERR];
    for (unsigned i = 0; i < 8; i++) {
        strcpy(cfg.anchor, anchors[i]);
        render(c, &cfg, &screen, &camera, NULL, false, &out);
        int x = positions[i][0], y = positions[i][1];
        assert(at(&out, x, y) == 0xaa0000);
        assert(at(&out, x + 199, y + 99) == 0xaa0000);
        assert(at(&out, x - 1, y) == 0x0000aa);
        int cx, cy, cw, ch;
        assert(!compositor_geometry(&cfg, 100, 75, &cx, &cy, &cw, &ch, error, sizeof error));
        assert(cw == 100 && ch == 75);
        assert(at(&out, cx + 50, cy + 37) == 0x00aa00);
    }
    strcpy(cfg.anchor, "free");
    cfg.camera_x = 10;
    cfg.camera_y = 10;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 180, 120) == 0xaa0000);
    cfg.camera_x = 290;
    cfg.camera_y = 155;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 20, 20) == 0xaa0000);
    strcpy(cfg.anchor, "bottom-right");
    cfg.camera_width_percent = 60;
    cfg.screen_width_percent = 85;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 30, 30) == 0xaa0000);
    assert(at(&out, 200, 100) == 0x00aa00); /* Natural overlap keeps the camera in front. */
    cfg.camera_visible = false;
    cfg.zoom_factor = 2;
    cfg.zoom_transition_ms = 0;
    frame_free(&screen);
    screen = source(200, 100, 0, true);
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert((at(&out, 20, 20) >> 16) == 50); /* Zoom still crops the screen's source coordinates. */
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&out);
    compositor_destroy(c);
}
static void test_shared_gradients_and_screen_backdrop(void)
{
    Config cfg = config(101, 31);
    strcpy(cfg.layout, "screen");
    strcpy(cfg.screen_background, "gradient");
    cfg.gradient_from = 0xff0000;
    cfg.gradient_via = 0x00ff00;
    cfg.gradient_to = 0x0000ff;
    cfg.gradient_angle = 0;
    cfg.gradient_waypoint = 25;
    Frame out = {0}, reference = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == 0xff0000 && at(&out, 25, 15) == 0x00ff00);
    assert(at(&out, 100, 30) == 0x0000ff);
    cfg.gradient_via_enabled = false;
    cfg.gradient_angle = 90;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == 0xff0000 && at(&out, 100, 30) == 0x0000ff);
    assert(at(&out, 0, 15) == at(&out, 100, 15));
    cfg.gradient_angle = 180;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == 0x0000ff && at(&out, 100, 30) == 0xff0000);
    cfg.gradient_angle = 135;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 100, 0) == 0xff0000 && at(&out, 0, 30) == 0x0000ff);
    assert(!frame_copy(&reference, &out));
    strcpy(cfg.layout, "camera");
    strcpy(cfg.camera_background, "gradient");
    cfg.camera_visible = false;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(!memcmp(reference.data, out.data, (size_t)out.stride * out.height));
    compositor_clear(c);
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(!memcmp(reference.data, out.data, (size_t)out.stride * out.height));

    cfg = config(120, 80);
    strcpy(cfg.layout, "stage");
    strcpy(cfg.screen_background, "blurred");
    strcpy(cfg.background_source, "screen");
    cfg.screen_margin = 10;
    cfg.screen_width_percent = 50;
    cfg.screen_background_brightness = 1;
    cfg.screen_background_color = 0;
    cfg.camera_width_percent = 20;
    cfg.camera_visible = false;
    Frame screen = source(120, 80, 0xff0000, false);
    Frame camera = source(120, 80, 0x00ff00, false);
    screen.ts_ns = camera.ts_ns = now;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 0, 0) == 0xbf0000);
    assert(!frame_copy(&reference, &out));
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(!memcmp(reference.data, out.data, (size_t)out.stride * out.height));
    frame_free(&screen);
    screen = source(120, 80, 0x0000ff, false);
    now++;
    camera.ts_ns = now;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 0, 0) == 0x0000bf);
    strcpy(cfg.background_source, "camera");
    cfg.camera_visible = true;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 0, 0) == 0x00bf00);
    cfg.camera_visible = false;
    cfg.gradient_angle = 0;
    render(c, &cfg, &screen, &camera, NULL, false, &out);
    assert(at(&out, 0, 0) == cfg.gradient_from); /* No replay of a hidden camera. */
    strcpy(cfg.background_source, "screen");
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == cfg.gradient_from); /* Missing screen replaces its processed cache. */
    frame_free(&screen);
    frame_free(&camera);
    frame_free(&reference);
    frame_free(&out);
    compositor_destroy(c);
    now--;
}
static void test_screen_mask_and_annotations(void)
{
    Config cfg = config(100, 60);
    strcpy(cfg.layout, "screen");
    strcpy(cfg.screen_background, "solid");
    cfg.screen_background_color = 0x0000aa;
    cfg.screen_radius = 20;
    cfg.screen_border_width = 3;
    cfg.screen_border_color = 0xffffff;
    Frame screen = source(100, 60, 0x223344, false), out = {0}, reference = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == 0x0000aa);
    assert(at(&out, 50, 0) == 0xffffff && at(&out, 50, 4) == 0x223344);
    assert(!frame_copy(&reference, &out));
    cfg.cursor = cfg.cursor_highlight = cfg.clicks = true;
    cfg.cursor_size = 35;
    cfg.cursor_color = 0xffff00;
    cfg.click_radius = 35;
    compositor_click(c, 1, 1, 1, now);
    Cursor cursor = {.x = 1, .y = 1, .valid = true};
    render(c, &cfg, &screen, NULL, &cursor, false, &out);
    unsigned changed = 0;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            if (at(&reference, x, y) != 0x223344) {
                assert(at(&out, x, y) == at(&reference, x, y));
            }
            changed += at(&out, x, y) != at(&reference, x, y);
        }
    }
    assert(changed);
    cfg.cursor = cfg.cursor_highlight = cfg.clicks = false;
    strcpy(cfg.layout, "split");
    cfg.split_ratio = 50;
    strcpy(cfg.camera_background, "solid");
    cfg.camera_background_color = 0xaa0000;
    cfg.camera_visible = false;
    render(c, &cfg, &screen, NULL, NULL, false, &out);
    assert(at(&out, 0, 0) == 0xaa0000); /* Screen backdrop stays inside its split allocation. */
    assert(at(&out, 50, 15) == 0x0000aa);
    frame_free(&screen);
    frame_free(&reference);
    frame_free(&out);
    compositor_destroy(c);
}
static bool rounded_reference(double x, double y, double width, double height, double radius)
{
    if (x < 0 || y < 0 || x >= width || y >= height || width <= 0 || height <= 0) {
        return false;
    }
    double nearest_x = fmax(radius, fmin(x, width - radius));
    double nearest_y = fmax(radius, fmin(y, height - radius));
    double dx = x - nearest_x, dy = y - nearest_y;
    return dx * dx + dy * dy <= radius * radius;
}
static void test_screen_mask_scanline_reference(void)
{
    Config cfg = config(93, 57);
    strcpy(cfg.layout, "screen");
    strcpy(cfg.screen_background, "solid");
    cfg.screen_background_color = 0x112233;
    cfg.screen_border_color = 0x445566;
    Compositor *c = compositor_create();
    Frame screen = {0}, out = {0};
    const int shapes[][2] = {{71, 39}, {27, 61}};
    const int radii[] = {0, 1, 9, 50}, borders[] = {0, 1, 6, 100};
    for (unsigned shape = 0; shape < 2; shape++) {
        frame_free(&screen);
        screen = source(shapes[shape][0], shapes[shape][1], 0xabcdef, false);
        double scale = fmin(93.0 / screen.width, 57.0 / screen.height);
        double width = screen.width * scale, height = screen.height * scale;
        double origin_x = (93 - width) / 2, origin_y = (57 - height) / 2;
        for (unsigned r = 0; r < 4; r++) {
            for (unsigned b = 0; b < 4; b++) {
                cfg.screen_radius = radii[r];
                cfg.screen_border_width = borders[b];
                render(c, &cfg, &screen, NULL, NULL, false, &out);
                double radius = fmin(cfg.screen_radius, fmin(width, height) / 2);
                int border =
                    (int)fmin(cfg.screen_border_width, fmax(0, (fmin(width, height) - 1) / 2));
                for (int y = 0; y < out.height; y++) {
                    for (int x = 0; x < out.width; x++) {
                        uint32_t expected = cfg.screen_background_color;
                        bool rectangle = x >= ceil(origin_x) && x < ceil(origin_x + width) &&
                                         y >= ceil(origin_y) && y < ceil(origin_y + height);
                        if (rectangle && !radius && !border) {
                            expected = 0xabcdef;
                        } else if (rectangle &&
                                   rounded_reference(x + .5 - origin_x, y + .5 - origin_y, width,
                                                     height, radius)) {
                            bool content = rounded_reference(
                                x + .5 - origin_x - border, y + .5 - origin_y - border,
                                width - 2 * border, height - 2 * border, fmax(0, radius - border));
                            expected = content ? 0xabcdef : cfg.screen_border_color;
                        }
                        assert(at(&out, x, y) == expected);
                    }
                }
            }
        }
    }
    frame_free(&screen);
    frame_free(&out);
    compositor_destroy(c);
}
static void test_prepared_logo_and_static_text(void)
{
    /* 4x2 RGBA PNG: transparent red, half red, opaque blue, opaque green. */
    static const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0,    0,    0,    0x0d, 0x49, 0x48,
        0x44, 0x52, 0,    0,    0,    4,    0,    0,    0,    2,    8,    6,    0,    0,
        0,    0x7f, 0xa8, 0x7d, 0x63, 0,    0,    0,    0x17, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xc0, 0,    0xc4, 0x0d, 0x40, 0xf2, 0x3f, 3,    4,
        0xa2, 0x0a, 0,    0,    0xcc, 0xe6, 0x0c, 0xf5, 0x8a, 0xe1, 0x27, 0x5c, 0,    0,
        0,    0,    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    char path[] = "/tmp/cast-logo-XXXXXX", error[CAST_ERR];
    int fd = mkstemp(path);
    assert(fd >= 0 && write(fd, png, sizeof png) == sizeof png);
    close(fd);
    Config cfg = config(80, 40);
    strcpy(cfg.layout, "screen");
    strcpy(cfg.screen_background, "solid");
    cfg.screen_background_color = 0x203040;
    strcpy(cfg.logo_path, path);
    cfg.logo_width_percent = 5;
    cfg.logo_opacity = 1;
    cfg.logo_margin_x = 3;
    cfg.logo_margin_y = 4;
    Compositor *c = compositor_create();
    Frame out = {0}, reference = {0};
    assert(!compositor_prepare(c, &cfg, error, sizeof error)); /* Path validates while disabled. */
    assert(!unlink(path));
    cfg.logo_enabled = true;
    const char *anchors[] = {"top-left", "top-right", "bottom-left", "bottom-right",
                             "top",      "bottom",    "left",        "right"};
    const int xy[][2] = {{3, 4}, {73, 4}, {3, 34}, {73, 34}, {38, 4}, {38, 34}, {3, 19}, {73, 19}};
    for (unsigned i = 0; i < 8; i++) {
        strcpy(cfg.logo_anchor, anchors[i]);
        render(c, &cfg, NULL, NULL, NULL, false, &out);
        int x = xy[i][0], y = xy[i][1];
        assert(at(&out, x, y) == 0x203040);
        assert(at(&out, x + 1, y) == 0x901820);
        assert(at(&out, x + 2, y) == 0x0000ff && at(&out, x + 3, y) == 0x00ff00);
    }
    cfg.logo_opacity = .5;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(at(&out, 75, 19) == 0x1018a0); /* Global opacity multiplies the file's alpha. */
    assert(!frame_copy(&reference, &out));
    Config bad = cfg;
    strcpy(bad.logo_path, "/no/such/cast-logo.png");
    strcpy(bad.pause_font, "serif");
    assert(compositor_prepare(c, &bad, error, sizeof error) < 0);
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(!memcmp(reference.data, out.data, (size_t)out.stride * out.height));
    strcpy(bad.logo_path, "/dev/null");
    assert(compositor_prepare(c, &bad, error, sizeof error) < 0 && strstr(error, "regular"));
    char broken[] = "/tmp/cast-logo-broken-XXXXXX";
    fd = mkstemp(broken);
    assert(fd >= 0 && write(fd, png, 12) == 12);
    strcpy(bad.logo_path, broken);
    assert(compositor_prepare(c, &bad, error, sizeof error) < 0 && strstr(error, "decode"));
    assert(!ftruncate(fd, 9 * 1024 * 1024));
    assert(compositor_prepare(c, &bad, error, sizeof error) < 0 && strstr(error, "8 MiB"));
    close(fd);
    unlink(broken);
    cfg.logo_enabled = false;
    cfg.logo_path[0] = 0;
    assert(!compositor_prepare(c, &cfg, error, sizeof error));

    cfg = config(240, 100);
    strcpy(cfg.layout, "screen");
    strcpy(cfg.screen_background, "solid");
    cfg.screen_background_color = 0x203040;
    cfg.text_enabled = true;
    strcpy(cfg.text_content, "F {date}"); /* Literal, never a template. */
    strcpy(cfg.text_font, "Noto Sans");
    cfg.text_size = 20;
    cfg.text_color = 0x00ff00;
    cfg.text_opacity = 1;
    cfg.text_margin_x = 7;
    cfg.text_margin_y = 9;
    PresentationText *text_cache = presentation_text_create();
    assert(text_cache && !presentation_text_prepare(text_cache, &cfg, error, sizeof error));
    const Frame *mask = presentation_text_overlay(text_cache);
    assert(mask && mask->data && mask->width > 40);
    const uint8_t *cached_pixels = mask->data;
    int text_w = mask->width, text_h = mask->height;
    for (unsigned i = 0; i < 8; i++) {
        strcpy(cfg.text_anchor, anchors[i]);
        assert(!presentation_text_prepare(text_cache, &cfg, error, sizeof error));
        assert(presentation_text_overlay(text_cache)->data == cached_pixels);
        render(c, &cfg, NULL, NULL, NULL, false, &out);
        int left = out.width, right = -1, top = out.height, bottom = -1;
        for (int y = 0; y < out.height; y++) {
            for (int x = 0; x < out.width; x++) {
                if (at(&out, x, y) != cfg.screen_background_color) {
                    left = x < left ? x : left;
                    right = x > right ? x : right;
                    top = y < top ? y : top;
                    bottom = y > bottom ? y : bottom;
                }
            }
        }
        int x = strstr(anchors[i], "left")    ? 7
                : strstr(anchors[i], "right") ? 240 - 7 - text_w
                                              : (240 - text_w) / 2;
        int y = strstr(anchors[i], "top")      ? 9
                : strstr(anchors[i], "bottom") ? 100 - 9 - text_h
                                               : (100 - text_h) / 2;
        assert(left == x && right == x + text_w - 1);
        assert(top == y && bottom == y + text_h - 1);
    }
    assert(!frame_copy(&reference, &out));
    cfg.text_opacity = .5;
    assert(!presentation_text_prepare(text_cache, &cfg, error, sizeof error));
    assert(presentation_text_overlay(text_cache)->data == cached_pixels);
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(memcmp(reference.data, out.data, (size_t)out.stride * out.height));
    bad = cfg;
    strcpy(bad.text_font, "Noto Sans:file=/no/such/cast-text.ttf");
    assert(presentation_text_prepare(text_cache, &bad, error, sizeof error) < 0);
    assert(presentation_text_overlay(text_cache)->data == cached_pixels);
    assert(!frame_copy(&reference, &out));
    assert(compositor_prepare(c, &bad, error, sizeof error) < 0);
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(!memcmp(reference.data, out.data, (size_t)out.stride * out.height));
    strcpy(cfg.text_font, "serif");
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert(memcmp(reference.data, out.data, (size_t)out.stride * out.height));
    cfg.text_enabled = false;
    render(c, &cfg, NULL, NULL, NULL, false, &out);
    assert_solid(&out, cfg.screen_background_color);
    presentation_text_destroy(text_cache);
    frame_free(&reference);
    frame_free(&out);
    compositor_destroy(c);
}

int main(void)
{
    test_frames();
    test_geometry();
    test_layout_and_masks();
    test_crop_and_fit();
    test_zoom_and_annotations();
    test_follow_lane_consistency();
    test_screen_sampling();
    test_stage_geometry();
    test_shared_gradients_and_screen_backdrop();
    test_screen_mask_and_annotations();
    test_screen_mask_scanline_reference();
    test_prepared_logo_and_static_text();
    test_neutral();
    test_text_templates();
    test_styled_text();
    test_footer_and_gap();
    test_font_replacement();
    test_blurred_frames();
    test_camera_backgrounds();
    test_key_history();
    test_key_canvas_bounds();
    puts("visual: owned frames, geometry, masks, fitting, crop/mirror, zoom transforms and lane "
         "privacy, bounded key history, styled UTF-8/templates, footers/gaps/font reload, blur and "
         "camera backgrounds, stage, gradients, masked screen and cached logo/static text passed");
    return 0;
}
