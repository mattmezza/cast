#include "cast.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    c.annotations_live_keys = c.annotations_record_keys = true;
    c.annotations_live_clicks = c.annotations_record_clicks = true;
    snprintf(c.layout, sizeof(c.layout), "overlay");
    snprintf(c.shape, sizeof(c.shape), "rectangle");
    snprintf(c.anchor, sizeof(c.anchor), "bottom-right");
    snprintf(c.aspect, sizeof(c.aspect), "native");
    snprintf(c.fit, sizeof(c.fit), "contain");
    snprintf(c.split_side, sizeof(c.split_side), "left");
    snprintf(c.keys_position, sizeof(c.keys_position), "bottom-left");
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
    cfg.margin = 300;
    assert(compositor_geometry(&cfg, 1920, 1080, &x, &y, &w, &h, error, sizeof(error)) < 0);
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
    compositor_neutral(&cfg, &out);
    assert(at(&out, 0, 0) == 0x334455);
    bool found = false;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            found |= at(&out, x, y) == 0xffffff;
        }
    }
    assert(found && out.ts_ns == now);
    frame_free(&out);
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
    cfg.annotations_live_keys = false;
    now += 3000000000;
    render(c, &cfg, NULL, NULL, NULL, false, &out); /* Expire while hidden. */
    cfg.annotations_live_keys = true;
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
    Frame screen = source(100, 50, 0, true), live = {0}, record = {0};
    Compositor *c = compositor_create();
    render(c, &cfg, &screen, NULL, &cursor, false, &live);
    cursor.x = 20;
    now += 34000000;
    render(c, &cfg, &screen, NULL, &cursor, false, &live);
    render(c, &cfg, &screen, NULL, &cursor, true, &record);
    assert(memcmp(live.data, record.data, (size_t)live.stride * live.height) == 0);
    now += 34000000;
    render(c, &cfg, &screen, NULL, &cursor, true, &record);
    render(c, &cfg, &screen, NULL, &cursor, false, &live);
    assert(memcmp(live.data, record.data, (size_t)live.stride * live.height) == 0);
    compositor_destroy(c);
    frame_free(&screen);
    frame_free(&live);
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

int main(void)
{
    test_frames();
    test_geometry();
    test_layout_and_masks();
    test_crop_and_fit();
    test_zoom_and_annotations();
    test_follow_lane_consistency();
    test_screen_sampling();
    test_neutral();
    test_key_history();
    test_key_canvas_bounds();
    puts("visual: owned frames, geometry, masks, fitting, crop/mirror, zoom transforms and lane "
         "privacy, bounded key history and repeat counts passed");
    return 0;
}
