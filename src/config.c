#include "cast.h"
#include "ini.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    T_STRING,
    T_INT,
    T_DOUBLE,
    T_BOOL,
    T_COLOR,
    T_ENUM
} Type;
typedef struct {
    const char *section, *key;
    Type type;
    size_t offset, size;
    double min, max;
    const char *choices, *def;
} Setting;
#define S(sec, key, field, def)                                                                    \
    {sec, key, T_STRING, offsetof(Config, field), sizeof(((Config *)0)->field), 0, 0, NULL, def}
#define E(sec, key, field, choices, def)                                                           \
    {sec, key, T_ENUM, offsetof(Config, field), sizeof(((Config *)0)->field), 0, 0, choices, def}
#define I(sec, key, field, min, max, def)                                                          \
    {sec, key, T_INT, offsetof(Config, field), 0, min, max, NULL, def}
#define D(sec, key, field, min, max, def)                                                          \
    {sec, key, T_DOUBLE, offsetof(Config, field), 0, min, max, NULL, def}
#define B(sec, key, field, def) {sec, key, T_BOOL, offsetof(Config, field), 0, 0, 0, NULL, def}
#define C(sec, key, field, def) {sec, key, T_COLOR, offsetof(Config, field), 0, 0, 0, NULL, def}
static const Setting settings[] = {
    E("output", "backend", backend, "xorg,wayland,synthetic", "xorg"),
    S("output", "device", output_device, "/dev/video10"),
    I("output", "width", width, 64, 7680, "1920"),
    I("output", "height", height, 64, 4320, "1080"),
    I("output", "fps", fps, 1, 120, "30"),
    B("output", "enabled", live_enabled, "true"),
    C("output", "pause_color", pause_color, "#20252b"),
    S("output", "pause_text", pause_text, "Paused"),
    S("camera", "device", camera_device, "/dev/video0"),
    B("camera", "enabled", camera_enabled, "true"),
    B("camera", "visible", camera_visible, "true"),
    E("camera", "shape", shape, "rectangle,rounded,circle", "rounded"),
    E("camera", "anchor", anchor, "top-left,top-right,bottom-left,bottom-right,free",
      "bottom-right"),
    E("camera", "aspect", aspect, "native,16:9,4:3,1:1", "native"),
    D("camera", "width_percent", camera_width_percent, 1, 100, "22"),
    I("camera", "margin", margin, 0, 4096, "24"),
    I("camera", "radius", radius, 0, 2048, "24"),
    I("camera", "border_width", border_width, 0, 256, "2"),
    C("camera", "border_color", border_color, "#ffffff"),
    I("camera", "x", camera_x, -7680, 7680, "0"),
    I("camera", "y", camera_y, -4320, 4320, "0"),
    I("camera", "crop_x", crop_x, -16384, 16384, "0"),
    I("camera", "crop_y", crop_y, -16384, 16384, "0"),
    B("camera", "mirror", mirror, "false"),
    S("camera", "corner_order", corner_order, "bottom-right,bottom-left,top-left,top-right"),
    E("composition", "layout", layout, "overlay,split,screen,camera", "overlay"),
    S("composition", "layout_order", layout_order, "overlay,split,screen,camera"),
    E("composition", "split_side", split_side, "left,right", "left"),
    D("composition", "split_ratio", split_ratio, 5, 95, "25"),
    E("composition", "fit", fit, "contain,cover", "contain"),
    S("composition", "preset_order", preset_order, "coding,demo,conversation"),
    S("capture", "monitor", monitor, ""),
    E("capture", "kind", capture_kind, "monitor,region", "monitor"),
    I("capture", "x", region_x, 0, 16384, "0"),
    I("capture", "y", region_y, 0, 16384, "0"),
    I("capture", "width", region_w, 0, 16384, "0"),
    I("capture", "height", region_h, 0, 16384, "0"),
    D("zoom", "factor", zoom_factor, 1, 20, "2"),
    D("zoom", "min", zoom_min, 1, 20, "1"),
    D("zoom", "max", zoom_max, 1, 20, "8"),
    D("zoom", "step", zoom_step, 0.01, 10, "0.25"),
    I("zoom", "transition_ms", zoom_transition_ms, 0, 3000, "250"),
    B("zoom", "follow", zoom_follow, "true"),
    I("zoom", "deadzone", zoom_deadzone, 0, 2000, "80"),
    D("zoom", "smoothing", zoom_smoothing, 0.001, 1, "0.18"),
    B("cursor", "enabled", cursor, "true"),
    B("cursor", "highlight", cursor_highlight, "false"),
    I("cursor", "size", cursor_size, 1, 256, "24"),
    C("cursor", "color", cursor_color, "#ffcc33"),
    B("clicks", "enabled", clicks, "false"),
    I("clicks", "duration_ms", click_duration_ms, 50, 10000, "600"),
    I("clicks", "radius", click_radius, 1, 512, "40"),
    C("clicks", "left_color", click_left_color, "#ffcc33"),
    C("clicks", "right_color", click_right_color, "#3399ff"),
    C("clicks", "middle_color", click_middle_color, "#cc66ff"),
    B("clicks", "middle", click_middle, "false"),
    B("keys", "enabled", keys, "false"),
    E("keys", "mode", keys_mode, "shortcuts,all", "shortcuts"),
    E("keys", "position", keys_position, "top-left,top-right,bottom-left,bottom-right",
      "bottom-left"),
    I("keys", "font_size", keys_font_size, 8, 96, "24"),
    I("keys", "timeout_ms", keys_timeout_ms, 100, 30000, "1800"),
    C("keys", "color", keys_color, "#ffffff"),
    C("keys", "background", keys_background, "#20252b"),
    S("keys", "filter", keys_filter, "Super+Pause,Super+F9,Super+F10,Super+F11,Super+F12"),
    S("keys", "navigation", keys_navigation,
      "Left,Right,Up,Down,Home,End,Page_Up,Page_Down,Escape,Tab,Return,BackSpace,Delete"),
    B("annotations", "live_keys", annotations_live_keys, "true"),
    B("annotations", "live_clicks", annotations_live_clicks, "true"),
    B("annotations", "record_keys", annotations_record_keys, "true"),
    B("annotations", "record_clicks", annotations_record_clicks, "true"),
    B("audio", "mic", mic, "false"),
    B("audio", "desktop", desktop, "false"),
    S("audio", "mic_source", mic_source, ""),
    S("audio", "desktop_source", desktop_source, ""),
    D("audio", "mic_gain", mic_gain, 0, 4, "1"),
    D("audio", "desktop_gain", desktop_gain, 0, 4, "1"),
    B("audio", "virtual", virtual_audio, "false"),
    S("audio", "virtual_name", virtual_name, "cast-microphone"),
    S("record", "directory", record_dir, "."),
    S("record", "container", record_container, "matroska"),
    S("record", "video_codec", video_codec, "libx264"),
    S("record", "audio_codec", audio_codec, "aac"),
    I("record", "crf", record_crf, 0, 51, "23"),
    S("record", "preset", record_preset, "veryfast"),
    I("record", "countdown", record_countdown, 0, 60, "0"),
    I("record", "queue", record_queue, 1, 120, "8"),
    B("preview", "enabled", preview, "false"),
    E("preview", "target", preview_target, "live,record", "live"),
    S("ipc", "socket", socket_path, ""),
    I("ipc", "timeout_ms", ipc_timeout_ms, 100, 30000, "5000")};
#define NSET (sizeof settings / sizeof settings[0])
static int fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return -1;
}
static bool choice(const char *s, const char *list)
{
    size_t len = strlen(s);
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        if (n == len && !strncmp(s, list, n)) {
            return true;
        }
        list = end ? end + 1 : NULL;
    }
    return false;
}
static int assign(void *base, const Setting *s, const char *v, char *err, size_t n)
{
    char *p = (char *)base + s->offset, *end;
    double d;
    long i;
    switch (s->type) {
    case T_STRING:
    case T_ENUM:
        if (strlen(v) >= s->size) {
            return fail(err, n, "value too long for %s.%s", s->section, s->key);
        }
        if (s->type == T_ENUM && !choice(v, s->choices)) {
            return fail(err, n, "%s.%s expects one of %s", s->section, s->key, s->choices);
        }
        strcpy(p, v);
        break;
    case T_BOOL:
        if (!strcmp(v, "true") || !strcmp(v, "on")) {
            *(bool *)p = true;
        } else if (!strcmp(v, "false") || !strcmp(v, "off")) {
            *(bool *)p = false;
        } else {
            return fail(err, n, "%s.%s expects true or false", s->section, s->key);
        }
        break;
    case T_COLOR:
        if (strlen(v) != 7 || v[0] != '#') {
            return fail(err, n, "%s.%s expects #RRGGBB", s->section, s->key);
        }
        errno = 0;
        i = strtol(v + 1, &end, 16);
        if (errno || *end) {
            return fail(err, n, "invalid RGB color");
        }
        *(uint32_t *)p = (uint32_t)i;
        break;
    case T_INT:
        errno = 0;
        i = strtol(v, &end, 10);
        if (errno || end == v || *end || i < s->min || i > s->max) {
            return fail(err, n, "%s.%s expects integer %.0f..%.0f", s->section, s->key, s->min,
                        s->max);
        }
        *(int *)p = (int)i;
        break;
    case T_DOUBLE:
        errno = 0;
        d = strtod(v, &end);
        if (errno || end == v || *end || !isfinite(d) || d < s->min || d > s->max) {
            return fail(err, n, "%s.%s expects number %g..%g", s->section, s->key, s->min, s->max);
        }
        *(double *)p = d;
        break;
    }
    return 0;
}
void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    char e[128];
    for (size_t i = 0; i < NSET; i++) {
        assign(c, &settings[i], settings[i].def, e, sizeof e);
    }
    c->preset_count = 3;
    Preset *p = &c->presets[0];
    strcpy(p->name, "coding");
    strcpy(p->layout, "screen");
    p->mask = 1;
    p = &c->presets[1];
    strcpy(p->name, "demo");
    strcpy(p->layout, "overlay");
    strcpy(p->camera_shape, "circle");
    p->camera_width_percent = 18;
    p->mask = 1 | 2 | 4;
    p = &c->presets[2];
    strcpy(p->name, "conversation");
    strcpy(p->layout, "camera");
    p->mask = 1;
}
static int valid_list(const char *value, const char *allowed, char *err, size_t n)
{
    char buf[512], seen[512] = "";
    if (strlen(value) >= sizeof buf) {
        return fail(err, n, "list too long");
    }
    strcpy(buf, value);
    char *save = NULL, *tok = strtok_r(buf, ",", &save);
    int count = 0;
    while (tok) {
        if (!*tok || (allowed && !choice(tok, allowed)) || choice(tok, seen)) {
            return fail(err, n, "invalid/duplicate list item %s", tok);
        }
        if (count) {
            strcat(seen, ",");
        }
        strcat(seen, tok);
        count++;
        tok = strtok_r(NULL, ",", &save);
    }
    if (!count || value[0] == ',' || value[strlen(value) - 1] == ',' || strstr(value, ",,")) {
        return fail(err, n, "empty list element");
    }
    return 0;
}
int config_validate(const Config *c, char *err, size_t n)
{
    if (c->width % 2 || c->height % 2) {
        return fail(err, n, "output dimensions must be even for video formats");
    }
    if (c->zoom_min > c->zoom_max || c->zoom_factor < c->zoom_min || c->zoom_factor > c->zoom_max) {
        return fail(err, n, "zoom factor must lie between min and max");
    }
    if (!strcmp(c->capture_kind, "region") && (!c->region_w || !c->region_h)) {
        return fail(err, n, "region capture needs positive width and height");
    }
    if (c->margin * 2 >= c->width || c->margin * 2 >= c->height) {
        return fail(err, n, "camera margin leaves no canvas area");
    }
    if (c->camera_width_percent * c->width / 100 > c->width - 2 * c->margin) {
        return fail(err, n, "camera width plus margins exceeds canvas");
    }
    if (c->socket_path[0] && c->socket_path[0] != '/') {
        return fail(err, n, "ipc.socket must be an absolute path");
    }
    if (c->desktop && !c->desktop_source[0]) {
        return fail(err, n, "audio.desktop requires an explicit desktop_source (cast audio list)");
    }
    if (c->virtual_audio && !c->virtual_name[0]) {
        return fail(err, n, "virtual microphone name must not be empty");
    }
    if (valid_list(c->corner_order, "top-left,top-right,bottom-left,bottom-right", err, n) ||
        valid_list(c->layout_order, "overlay,split,screen,camera", err, n)) {
        return -1;
    }
    if (c->preset_order[0]) {
        char names[2048] = "";
        for (int i = 0; i < c->preset_count; i++) {
            if (i) {
                strcat(names, ",");
            }
            strcat(names, c->presets[i].name);
        }
        if (valid_list(c->preset_order, names, err, n)) {
            return -1;
        }
    }
    for (int i = 0; i < c->preset_count; i++) {
        const Preset *p = &c->presets[i];
        if ((p->mask & 4) && p->camera_width_percent * c->width / 100 > c->width - 2 * c->margin) {
            return fail(err, n, "preset.%s camera width exceeds canvas", p->name);
        }
        if ((p->mask & 32) && (p->zoom_factor < c->zoom_min || p->zoom_factor > c->zoom_max)) {
            return fail(err, n, "preset.%s zoom outside limits", p->name);
        }
    }
    return 0;
}
typedef struct {
    Config *c;
    const char *path;
    char *err;
    size_t n;
    char seen[512][128];
    int seen_count;
    int last_line;
} Parse;
static int preset_assign(Preset *p, const char *key, const char *value, char *e, size_t n)
{
    Setting s = {"preset", key, T_ENUM, 0, 0, 0, 0, NULL, NULL};
    unsigned bit = 0;
    if (!strcmp(key, "layout")) {
        s.offset = offsetof(Preset, layout);
        s.size = sizeof p->layout;
        s.choices = "overlay,split,screen,camera";
        bit = 1;
    } else if (!strcmp(key, "camera_shape")) {
        s.offset = offsetof(Preset, camera_shape);
        s.size = sizeof p->camera_shape;
        s.choices = "rectangle,rounded,circle";
        bit = 2;
    } else if (!strcmp(key, "camera_width_percent")) {
        s.offset = offsetof(Preset, camera_width_percent);
        s.type = T_DOUBLE;
        s.min = 1;
        s.max = 100;
        bit = 4;
    } else if (!strcmp(key, "split_ratio")) {
        s.offset = offsetof(Preset, split_ratio);
        s.type = T_DOUBLE;
        s.min = 5;
        s.max = 95;
        bit = 8;
    } else if (!strcmp(key, "camera_visible")) {
        s.offset = offsetof(Preset, camera_visible);
        s.type = T_BOOL;
        bit = 16;
    } else if (!strcmp(key, "zoom_factor")) {
        s.offset = offsetof(Preset, zoom_factor);
        s.type = T_DOUBLE;
        s.min = 1;
        s.max = 20;
        bit = 32;
    } else if (!strcmp(key, "camera_anchor")) {
        s.offset = offsetof(Preset, camera_anchor);
        s.size = sizeof p->camera_anchor;
        s.choices = "top-left,top-right,bottom-left,bottom-right,free";
        bit = 64;
    } else if (!strcmp(key, "camera_aspect")) {
        s.offset = offsetof(Preset, camera_aspect);
        s.size = sizeof p->camera_aspect;
        s.choices = "native,16:9,4:3,1:1";
        bit = 128;
    } else {
        return fail(e, n, "unknown preset setting %s", key);
    }
    if (assign(p, &s, value, e, n)) {
        return -1;
    }
    p->mask |= bit;
    return 0;
}
static int handler(void *u, const char *section, const char *key, const char *value, int line)
{
    Parse *p = u;
    p->last_line = line;
    char e[CAST_ERR] = "", full[128];
    Preset *preset = NULL;
    bool known = false;
    if (!strncmp(section, "preset.", 7)) {
        const char *name = section + 7;
        size_t len = strlen(name);
        known = len > 0 && len < 64;
        for (size_t i = 0; i < len; i++) {
            if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') {
                known = false;
            }
        }
        if (known) {
            for (int i = 0; i < p->c->preset_count; i++) {
                if (!strcmp(p->c->presets[i].name, name)) {
                    preset = &p->c->presets[i];
                }
            }
            if (!preset) {
                if (p->c->preset_count == CAST_MAX_PRESETS) {
                    known = false;
                } else {
                    preset = &p->c->presets[p->c->preset_count++];
                    memset(preset, 0, sizeof *preset);
                    strcpy(preset->name, name);
                }
            }
        }
    } else {
        for (size_t i = 0; i < NSET; i++) {
            if (!strcmp(section, settings[i].section)) {
                known = true;
            }
        }
    }
    if (!known) {
        fail(p->err, p->n, "%s:%d: unknown or invalid section [%s]", p->path, line, section);
        return 0;
    }
    if (!key) {
        return 1;
    }
    if (snprintf(full, sizeof full, "%s.%s", section, key) >= (int)sizeof full) {
        fail(p->err, p->n, "%s:%d: key too long", p->path, line);
        return 0;
    }
    for (int i = 0; i < p->seen_count; i++) {
        if (!strcmp(full, p->seen[i])) {
            fail(p->err, p->n, "%s:%d: duplicate key %s", p->path, line, full);
            return 0;
        }
    }
    if (p->seen_count == 512) {
        fail(p->err, p->n, "%s:%d: too many settings", p->path, line);
        return 0;
    }
    strcpy(p->seen[p->seen_count++], full);
    int rc = -1;
    if (preset) {
        rc = preset_assign(preset, key, value, e, sizeof e);
    } else {
        for (size_t i = 0; i < NSET; i++) {
            if (!strcmp(section, settings[i].section) && !strcmp(key, settings[i].key)) {
                rc = assign(p->c, &settings[i], value, e, sizeof e);
                break;
            }
        }
    }
    if (rc) {
        fail(p->err, p->n, "%s:%d: %s", p->path, line, e[0] ? e : "unknown setting");
        return 0;
    }
    return 1;
}
int config_load(Config *c, const char *path, bool explicit_path, char *err, size_t n)
{
    Config candidate;
    config_defaults(&candidate);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT && !explicit_path) {
            *c = candidate;
            return 0;
        }
        return fail(err, n, "%s: %s", path, strerror(errno));
    }
    Parse *p = calloc(1, sizeof *p);
    if (!p) {
        fclose(f);
        return fail(err, n, "out of memory reading configuration");
    }
    p->c = &candidate;
    p->path = path;
    p->err = err;
    p->n = n;
    err[0] = 0;
    int rc = ini_parse_file(f, handler, p);
    int last_line = p->last_line;
    if (ferror(f) && !rc) {
        rc = -1;
    }
    free(p);
    fclose(f);
    if (rc) {
        if (!err[0]) {
            fail(err, n, "%s:%d: invalid INI syntax or line too long", path, rc > 0 ? rc : 0);
        }
        return -1;
    }
    char e[CAST_ERR];
    if (config_validate(&candidate, e, sizeof e)) {
        return fail(err, n, "%s:%d: configuration conflict: %s", path, last_line, e);
    }
    *c = candidate;
    return 0;
}
void config_print_defaults(void)
{
    const char *section = "";
    for (size_t i = 0; i < NSET; i++) {
        if (strcmp(section, settings[i].section)) {
            section = settings[i].section;
            printf("\n[%s]\n", section);
        }
        printf("%s = %s\n", settings[i].key, settings[i].def);
    }
    puts("\n[preset.coding]\nlayout = screen\n\n[preset.demo]\nlayout = overlay\ncamera_shape = "
         "circle\ncamera_width_percent = 18\n\n[preset.conversation]\nlayout = camera");
}
