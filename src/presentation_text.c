/* Styled output text. Only font faces are cached; frames and expanded messages are transient. */
#include "presentation_text.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPANDED_TEXT 1536
#define FONT_FALLBACKS 8
#define TEXT_LINES 64
typedef struct {
    FT_Face face;
    FcPattern *pattern;
    char name[256];
    FT_Face fallback[FONT_FALLBACKS];
    unsigned fallback_count;
} TextFont;
struct PresentationText {
    FT_Library library;
    TextFont pause, blur, overlay;
    Frame static_text;
    char static_content[256];
    int static_size, static_width, static_height, static_margin_x, static_margin_y;
    uint32_t static_color;
};
typedef struct {
    const char *start;
    size_t bytes;
    int left, width, ascent, height;
} TextLine;
typedef struct {
    TextLine lines[TEXT_LINES];
    unsigned count;
    int width, height;
} TextBlock;
static int static_raster(PresentationText *, TextFont *, const Config *, Frame *, char *, size_t);
static int fail(char *error, size_t n, const char *format, ...)
{
    if (error && n) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, n, format, args);
        va_end(args);
    }
    return -1;
}
static int utf8_next(const char **cursor, uint32_t *codepoint)
{
    const unsigned char *s = (const unsigned char *)*cursor;
    unsigned count;
    uint32_t value, minimum;
    if (*s < 0x80) {
        *codepoint = *s;
        *cursor += !!*s;
        return 0;
    }
    if (*s >= 0xc2 && *s <= 0xdf) {
        count = 2;
        value = *s & 0x1f;
        minimum = 0x80;
    } else if (*s >= 0xe0 && *s <= 0xef) {
        count = 3;
        value = *s & 0x0f;
        minimum = 0x800;
    } else if (*s >= 0xf0 && *s <= 0xf4) {
        count = 4;
        value = *s & 7;
        minimum = 0x10000;
    } else {
        return -1;
    }
    for (unsigned i = 1; i < count; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return -1;
        }
        value = (value << 6) | (s[i] & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return -1;
    }
    *codepoint = value;
    *cursor += count;
    return 0;
}
static int utf8_validate(const char *s, char *error, size_t n)
{
    uint32_t codepoint;
    while (*s) {
        if (utf8_next(&s, &codepoint)) {
            return fail(error, n, "presentation text must be valid UTF-8");
        }
        if ((codepoint < 32 && codepoint != '\n' && codepoint != '\t') || codepoint == 127) {
            return fail(error, n, "presentation text contains an unsupported control character");
        }
    }
    return 0;
}
static int format_validate(const char *format, size_t *maximum, char *error, size_t n)
{
    size_t total = 0;
    while (*format) {
        if (*format++ != '%') {
            total++;
            continue;
        }
        unsigned width = 0;
        while (*format && (strchr("_-0^#", *format) || isdigit((unsigned char)*format))) {
            if (isdigit((unsigned char)*format)) {
                while (isdigit((unsigned char)*format)) {
                    width = width * 10 + (unsigned)(*format++ - '0');
                    if (width >= EXPANDED_TEXT) {
                        return fail(error, n,
                                    "date/time field width exceeds its bounded expansion");
                    }
                }
                continue;
            }
            format++;
        }
        if (*format == 'E' || *format == 'O') {
            format++;
        }
        if (!*format || !strchr("aAbBcCdDeFgGhHIjklmMnprRStTuUVwWxXyYzZs+%", *format)) {
            return fail(error, n,
                        "invalid date/time format: expected a strftime conversion after %%");
        }
        /* Reserve room for future timestamps as well as explicit padding. Locale
         * names/composite formats receive a generous budget; validation below
         * also expands against the active locale before accepting configuration. */
        size_t field = strchr("aAbBchprxXZ+", *format) ? 128 : 12;
        if (strchr("nt%", *format)) {
            field = 1;
        } else if (*format == 's') {
            field = 20;
        } else if (*format == 'F') {
            field = 24;
        }
        if (field < width) {
            field = width;
        }
        total += field;
        if (total >= EXPANDED_TEXT) {
            return fail(error, n, "date/time format exceeds its bounded expansion");
        }
        format++;
    }
    *maximum = total;
    return 0;
}
static int append(char *output, size_t capacity, size_t *used, const char *text, size_t bytes,
                  char *error, size_t n)
{
    if (bytes >= capacity - *used) {
        return fail(error, n, "expanded presentation text exceeds %zu bytes", capacity - 1);
    }
    if (output) {
        memcpy(output + *used, text, bytes);
    }
    *used += bytes;
    if (output) {
        output[*used] = 0;
    }
    return 0;
}
int presentation_template_expand(const char *input, const struct tm *when, char *output,
                                 size_t capacity, char *error, size_t n)
{
    if (!input || (output && !capacity)) {
        return fail(error, n, "presentation template and a bounded output buffer are required");
    }
    if (utf8_validate(input, error, n)) {
        return -1;
    }
    if (!output) {
        capacity = EXPANDED_TEXT;
    }
    size_t used = 0;
    if (output) {
        output[0] = 0;
    }
    for (const char *s = input; *s;) {
        if ((*s == '{' && s[1] == '{') || (*s == '}' && s[1] == '}')) {
            if (append(output, capacity, &used, s, 1, error, n)) {
                return -1;
            }
            s += 2;
        } else if (*s == '}') {
            return fail(error, n,
                        "unmatched } in presentation template; use }} for a literal brace");
        } else if (*s != '{') {
            const char *start = s++;
            while (*s && *s != '{' && *s != '}') {
                s++;
            }
            if (append(output, capacity, &used, start, (size_t)(s - start), error, n)) {
                return -1;
            }
        } else {
            const char *start = ++s, *end = strchr(s, '}');
            if (!end || memchr(s, '{', (size_t)(end - s))) {
                return fail(error, n,
                            "unmatched { in presentation template; use {{ for a literal brace");
            }
            const char *colon = memchr(start, ':', (size_t)(end - start));
            size_t name_size = (size_t)((colon ? colon : end) - start);
            const char *default_format;
            if (name_size == 4 && !memcmp(start, "date", 4)) {
                default_format = "%Y-%m-%d";
            } else if (name_size == 4 && !memcmp(start, "time", 4)) {
                default_format = "%H:%M:%S";
            } else if (name_size == 8 && !memcmp(start, "datetime", 8)) {
                default_format = "%Y-%m-%d %H:%M:%S";
            } else {
                return fail(error, n,
                            "unknown placeholder {%.*s}; use {date}, {time}, or {datetime}",
                            (int)name_size, start);
            }
            char format[256];
            size_t length = colon ? (size_t)(end - colon - 1) : strlen(default_format);
            if (!length || length >= sizeof format) {
                return fail(error, n, "date/time format must contain 1..255 bytes");
            }
            memcpy(format, colon ? colon + 1 : default_format, length);
            format[length] = 0;
            size_t maximum;
            if (format_validate(format, &maximum, error, n)) {
                return -1;
            }
            if (output) {
                if (!when) {
                    return fail(error, n,
                                "date/time template expansion requires a captured local time");
                }
                char expanded[EXPANDED_TEXT];
                /* Formats are validated above; the indirection also keeps compiler
                 * literal-format diagnostics from treating configuration as C code. */
                size_t (*format_time)(char *, size_t, const char *, const struct tm *) = strftime;
                size_t bytes = format_time(expanded, sizeof expanded, format, when);
                if (!bytes) {
                    return fail(error, n, "date/time expansion exceeds its bounded buffer");
                }
                if (append(output, capacity, &used, expanded, bytes, error, n)) {
                    return -1;
                }
            } else if (append(NULL, capacity, &used, NULL, maximum, error, n)) {
                return -1;
            }
            s = end + 1;
        }
    }
    return 0;
}
int presentation_template_validate(const char *input, char *error, size_t n)
{
    if (presentation_template_expand(input, NULL, NULL, 0, error, n)) {
        return -1;
    }
    time_t now = time(NULL);
    struct tm captured;
    char expanded[EXPANDED_TEXT];
    if (!localtime_r(&now, &captured)) {
        return fail(error, n, "cannot capture local time for presentation template validation");
    }
    if (presentation_template_expand(input, &captured, expanded, sizeof expanded, error, n)) {
        return -1;
    }
    return utf8_validate(expanded, error, n);
}
static void font_clear(TextFont *font)
{
    for (unsigned i = 0; i < font->fallback_count; i++) {
        FT_Done_Face(font->fallback[i]);
    }
    if (font->face) {
        FT_Done_Face(font->face);
    }
    if (font->pattern) {
        FcPatternDestroy(font->pattern);
    }
    memset(font, 0, sizeof *font);
}
static int font_load(FT_Library library, const char *name, TextFont *font, char *error, size_t n)
{
    FcPattern *pattern = FcNameParse((const FcChar8 *)name);
    if (!pattern) {
        return fail(error, n, "cannot parse Fontconfig font '%s'", name);
    }
    FcChar8 *specified_file = NULL;
    FcPatternGetString(pattern, FC_FILE, 0, &specified_file);
    if (specified_file) {
        int index = 0;
        FcPatternGetInteger(pattern, FC_INDEX, 0, &index);
        FT_Error rc = FT_New_Face(library, (const char *)specified_file, index, &font->face);
        if (rc) {
            FcPatternDestroy(pattern);
            return fail(error, n, "cannot load configured font file for '%s' (FreeType error %d)",
                        name, rc);
        }
        font->pattern = pattern;
        snprintf(font->name, sizeof font->name, "%s", name);
        return 0;
    }
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcChar8 *file = NULL;
    int index = 0;
    if (!match || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
        if (match) {
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
        return fail(error, n, "no installed font matches '%s'; inspect fc-match", name);
    }
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    FT_Error rc = FT_New_Face(library, (const char *)file, index, &font->face);
    FcPatternDestroy(match);
    if (rc) {
        FcPatternDestroy(pattern);
        return fail(error, n, "cannot load installed font '%s' (FreeType error %d)", name, rc);
    }
    font->pattern = pattern;
    snprintf(font->name, sizeof font->name, "%s", name);
    return 0;
}
PresentationText *presentation_text_create(void)
{
    PresentationText *text = calloc(1, sizeof *text);
    if (text && FT_Init_FreeType(&text->library)) {
        free(text);
        return NULL;
    }
    return text;
}
void presentation_text_destroy(PresentationText *text)
{
    if (text) {
        font_clear(&text->pause);
        font_clear(&text->blur);
        font_clear(&text->overlay);
        frame_free(&text->static_text);
        FT_Done_FreeType(text->library);
        free(text);
    }
}
int presentation_text_prepare(PresentationText *text, const Config *cfg, char *error, size_t n)
{
    if (!text || !cfg) {
        return fail(error, n, "presentation text renderer is unavailable");
    }
    if (presentation_template_validate(cfg->pause_text, error, n) ||
        presentation_template_validate(cfg->pause_subtitle, error, n) ||
        presentation_template_validate(cfg->pause_footer, error, n) ||
        presentation_template_validate(cfg->blur_title, error, n) ||
        presentation_template_validate(cfg->blur_subtitle, error, n) ||
        presentation_template_validate(cfg->blur_footer, error, n) ||
        utf8_validate(cfg->text_content, error, n)) {
        return -1;
    }
    const char *pause = cfg->pause_font[0] ? cfg->pause_font : "Noto Sans";
    const char *blur = cfg->blur_font[0] ? cfg->blur_font : "Noto Sans";
    const char *overlay = cfg->text_font[0] ? cfg->text_font : "Noto Sans";
    bool replace_pause = !text->pause.face || strcmp(pause, text->pause.name);
    bool replace_blur = !text->blur.face || strcmp(blur, text->blur.name);
    bool replace_overlay = !text->overlay.face || strcmp(overlay, text->overlay.name);
    bool replace_mask =
        replace_overlay || strcmp(cfg->text_content, text->static_content) ||
        cfg->text_size != text->static_size || cfg->text_color != text->static_color ||
        cfg->width != text->static_width || cfg->height != text->static_height ||
        cfg->text_margin_x != text->static_margin_x || cfg->text_margin_y != text->static_margin_y;
    TextFont candidate_pause = {0}, candidate_blur = {0}, candidate_overlay = {0};
    Frame candidate_mask = {0};
    if ((replace_pause && font_load(text->library, pause, &candidate_pause, error, n)) ||
        (replace_blur && font_load(text->library, blur, &candidate_blur, error, n)) ||
        (replace_overlay && font_load(text->library, overlay, &candidate_overlay, error, n)) ||
        (replace_mask && static_raster(text, replace_overlay ? &candidate_overlay : &text->overlay,
                                       cfg, &candidate_mask, error, n))) {
        font_clear(&candidate_pause);
        font_clear(&candidate_blur);
        font_clear(&candidate_overlay);
        frame_free(&candidate_mask);
        return -1;
    }
    if (replace_pause) {
        font_clear(&text->pause);
        text->pause = candidate_pause;
    }
    if (replace_blur) {
        font_clear(&text->blur);
        text->blur = candidate_blur;
    }
    if (replace_overlay) {
        font_clear(&text->overlay);
        text->overlay = candidate_overlay;
    }
    if (replace_mask) {
        frame_free(&text->static_text);
        text->static_text = candidate_mask;
        snprintf(text->static_content, sizeof text->static_content, "%s", cfg->text_content);
        text->static_size = cfg->text_size;
        text->static_color = cfg->text_color;
        text->static_width = cfg->width;
        text->static_height = cfg->height;
        text->static_margin_x = cfg->text_margin_x;
        text->static_margin_y = cfg->text_margin_y;
    }
    return 0;
}
const Frame *presentation_text_overlay(const PresentationText *text)
{
    return text ? &text->static_text : NULL;
}
static FT_Face glyph_face(PresentationText *text, TextFont *font, uint32_t codepoint, int size)
{
    FT_Face face = font->face;
    if (!FT_Get_Char_Index(face, codepoint)) {
        for (unsigned i = 0; i < font->fallback_count; i++) {
            if (FT_Get_Char_Index(font->fallback[i], codepoint)) {
                face = font->fallback[i];
                break;
            }
        }
        if (face == font->face && font->fallback_count < FONT_FALLBACKS) {
            FcPattern *pattern = FcPatternDuplicate(font->pattern);
            FcCharSet *characters = FcCharSetCreate();
            if (pattern && characters) {
                FcCharSetAddChar(characters, codepoint);
                FcPatternDel(pattern, FC_CHARSET);
                FcPatternAddCharSet(pattern, FC_CHARSET, characters);
                FcResult result;
                FcPattern *match = FcFontMatch(NULL, pattern, &result);
                FcChar8 *file;
                int index = 0;
                FT_Face fallback = NULL;
                if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
                    FcPatternGetInteger(match, FC_INDEX, 0, &index);
                    if (!FT_New_Face(text->library, (const char *)file, index, &fallback)) {
                        if (FT_Get_Char_Index(fallback, codepoint)) {
                            font->fallback[font->fallback_count++] = fallback;
                            face = fallback;
                        } else {
                            FT_Done_Face(fallback);
                        }
                    }
                }
                if (match) {
                    FcPatternDestroy(match);
                }
            }
            if (characters) {
                FcCharSetDestroy(characters);
            }
            if (pattern) {
                FcPatternDestroy(pattern);
            }
        }
    }
    FT_Set_Pixel_Sizes(face, 0, (unsigned)size);
    return face;
}
static int block_measure(PresentationText *text, TextFont *font, const char *message, int size,
                         TextBlock *block, char *error, size_t n)
{
    memset(block, 0, sizeof *block);
    if (!*message) {
        return 0;
    }
    for (const char *s = message; *s;) {
        if (block->count == TEXT_LINES) {
            return fail(error, n, "presentation text exceeds %d lines", TEXT_LINES);
        }
        TextLine *line = &block->lines[block->count++];
        line->start = s;
        int x = 0, right = 0, descent = 0;
        FT_Face previous_face = NULL;
        FT_UInt previous = 0;
        while (*s && *s != '\n') {
            uint32_t codepoint;
            if (utf8_next(&s, &codepoint)) {
                return fail(error, n, "expanded presentation text is not valid UTF-8");
            }
            if (codepoint == '\t') {
                codepoint = ' ';
            }
            FT_Face face = glyph_face(text, font, codepoint, size);
            FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
            if (face == previous_face && previous && glyph && FT_HAS_KERNING(face)) {
                FT_Vector kerning;
                FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning);
                x += (int)(kerning.x >> 6);
            }
            if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER)) {
                return fail(error, n, "cannot rasterize presentation text glyph U+%04X", codepoint);
            }
            FT_GlyphSlot slot = face->glyph;
            int left = x + slot->bitmap_left;
            if (left < line->left) {
                line->left = left;
            }
            if (left + (int)slot->bitmap.width > right) {
                right = left + (int)slot->bitmap.width;
            }
            if (slot->bitmap_top > line->ascent) {
                line->ascent = slot->bitmap_top;
            }
            int below = (int)slot->bitmap.rows - slot->bitmap_top;
            if (below > descent) {
                descent = below;
            }
            x += (int)(slot->advance.x >> 6);
            previous_face = face;
            previous = glyph;
        }
        line->bytes = (size_t)(s - line->start);
        line->width = right - line->left;
        line->height = line->ascent + descent;
        if (!line->height) {
            line->ascent = size;
            line->height = size;
        }
        if (line->width > block->width) {
            block->width = line->width;
        }
        block->height += line->height;
        if (*s == '\n') {
            s++;
            if (*s) {
                block->height += size / 3 + 1;
            }
        }
    }
    return 0;
}
static void blend(Frame *frame, int x, int y, uint32_t color, unsigned alpha)
{
    if (x < 0 || y < 0 || x >= frame->width || y >= frame->height) {
        return;
    }
    uint8_t *pixel = frame->data + (size_t)y * frame->stride + (size_t)x * 4;
    unsigned previous = pixel[3];
    unsigned combined = alpha + (previous * (255 - alpha) + 127) / 255;
    if (!combined) {
        return;
    }
    for (int ch = 0; ch < 3; ch++) {
        unsigned value = (color >> (16 - ch * 8)) & 255;
        pixel[ch] = (uint8_t)((value * alpha + (pixel[ch] * previous * (255 - alpha) + 127) / 255 +
                               combined / 2) /
                              combined);
    }
    pixel[3] = (uint8_t)combined;
}
static void block_draw(PresentationText *text, TextFont *font, const TextBlock *block, int size,
                       Frame *frame, int top, int margin, int clip_top, int clip_bottom,
                       uint32_t color)
{
    for (unsigned i = 0; i < block->count; i++) {
        const TextLine *line = &block->lines[i];
        int x = (frame->width - line->width) / 2 - line->left;
        int baseline = top + line->ascent;
        const char *s = line->start, *end = s + line->bytes;
        FT_Face previous_face = NULL;
        FT_UInt previous = 0;
        while (s < end) {
            uint32_t codepoint;
            if (utf8_next(&s, &codepoint)) {
                break;
            }
            if (codepoint == '\t') {
                codepoint = ' ';
            }
            FT_Face face = glyph_face(text, font, codepoint, size);
            FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
            if (face == previous_face && previous && glyph && FT_HAS_KERNING(face)) {
                FT_Vector kerning;
                FT_Get_Kerning(face, previous, glyph, FT_KERNING_DEFAULT, &kerning);
                x += (int)(kerning.x >> 6);
            }
            if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER)) {
                break;
            }
            FT_GlyphSlot slot = face->glyph;
            FT_Bitmap *bitmap = &slot->bitmap;
            for (unsigned y = 0; y < bitmap->rows; y++) {
                int row = bitmap->pitch < 0 ? (int)bitmap->rows - 1 - (int)y : (int)y;
                const uint8_t *data = bitmap->buffer + row * abs(bitmap->pitch);
                for (unsigned bx = 0; bx < bitmap->width; bx++) {
                    unsigned alpha = bitmap->pixel_mode == FT_PIXEL_MODE_MONO
                                         ? ((data[bx / 8] & (0x80 >> (bx % 8))) ? 255 : 0)
                                         : data[bx];
                    int px = x + slot->bitmap_left + (int)bx;
                    int py = baseline - slot->bitmap_top + (int)y;
                    if (px >= margin && px < frame->width - margin && py >= clip_top &&
                        py < clip_bottom) {
                        blend(frame, px, py, color, alpha);
                    }
                }
            }
            x += (int)(slot->advance.x >> 6);
            previous_face = face;
            previous = glyph;
        }
        top += line->height + size / 3 + 1;
    }
}
static int static_raster(PresentationText *text, TextFont *font, const Config *cfg, Frame *out,
                         char *error, size_t n)
{
    if (!cfg->text_content[0]) {
        return 0;
    }
    int width = (int)fmax(1, cfg->width - 2 * cfg->text_margin_x);
    int height = (int)fmax(1, cfg->height - 2 * cfg->text_margin_y);
    int size = cfg->text_size > 0 ? cfg->text_size : 28;
    TextBlock block;
    for (;;) {
        if (block_measure(text, font, cfg->text_content, size, &block, error, n)) {
            return -1;
        }
        if ((block.width <= width && block.height <= height) || size == 1) {
            break;
        }
        double ratio =
            fmin((double)width / fmax(1, block.width), (double)height / fmax(1, block.height));
        size = (int)fmax(1, fmin(size - 1, floor(size * ratio)));
    }
    int w = (int)fmax(1, fmin(width, block.width));
    int h = (int)fmax(1, fmin(height, block.height));
    if (frame_alloc(out, w, h)) {
        return fail(error, n, "cannot allocate bounded static text overlay");
    }
    block_draw(text, font, &block, size, out, 0, 0, 0, h, cfg->text_color);
    /* Anchor the visible ink, including fonts with positive leading bearings. */
    int left = w, right = -1, top = h, bottom = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (out->data[(size_t)y * out->stride + x * 4 + 3]) {
                left = x < left ? x : left;
                right = x > right ? x : right;
                top = y < top ? y : top;
                bottom = y > bottom ? y : bottom;
            }
        }
    }
    if (right < left) {
        frame_free(out);
    } else if (left || top || right != w - 1 || bottom != h - 1) {
        Frame cropped = {0};
        if (frame_alloc(&cropped, right - left + 1, bottom - top + 1)) {
            return fail(error, n, "cannot prepare static text ink bounds");
        }
        for (int y = 0; y < cropped.height; y++) {
            memcpy(cropped.data + (size_t)y * cropped.stride,
                   out->data + (size_t)(y + top) * out->stride + left * 4,
                   (size_t)cropped.width * 4);
        }
        frame_free(out);
        *out = cropped;
    }
    return 0;
}
int presentation_text_draw(PresentationText *text, const Config *cfg, bool blur, Frame *frame,
                           char *error, size_t n)
{
    if (presentation_text_prepare(text, cfg, error, n)) {
        return -1;
    }
    time_t captured = time(NULL);
    struct tm local;
    if (!localtime_r(&captured, &local)) {
        return fail(error, n, "cannot obtain local date/time for presentation text");
    }
    char title[EXPANDED_TEXT], subtitle[EXPANDED_TEXT], footer[EXPANDED_TEXT];
    if (presentation_template_expand(blur ? cfg->blur_title : cfg->pause_text, &local, title,
                                     sizeof title, error, n) ||
        presentation_template_expand(blur ? cfg->blur_subtitle : cfg->pause_subtitle, &local,
                                     subtitle, sizeof subtitle, error, n) ||
        presentation_template_expand(blur ? cfg->blur_footer : cfg->pause_footer, &local, footer,
                                     sizeof footer, error, n)) {
        return -1;
    }
    if (!*title && !*subtitle && !*footer) {
        return 0;
    }
    TextFont *font = blur ? &text->blur : &text->pause;
    int title_size = blur ? cfg->blur_title_size : cfg->pause_title_size;
    int subtitle_size = blur ? cfg->blur_subtitle_size : cfg->pause_subtitle_size;
    int footer_size = blur ? cfg->blur_footer_size : cfg->pause_footer_size;
    int configured_gap = blur ? cfg->blur_text_gap : cfg->pause_text_gap;
    title_size = title_size > 0 ? title_size : 48;
    subtitle_size = subtitle_size > 0 ? subtitle_size : 24;
    footer_size = footer_size > 0 ? footer_size : 18;
    int margin = (int)fmin(48, fmin(frame->width, frame->height) / 12);
    int available_w = frame->width - 2 * margin, available_h = frame->height - 2 * margin;
    TextBlock title_block, subtitle_block, footer_block;
    int gap = configured_gap, height, separation = margin;
    for (;;) {
        if (block_measure(text, font, title, title_size, &title_block, error, n) ||
            block_measure(text, font, subtitle, subtitle_size, &subtitle_block, error, n) ||
            block_measure(text, font, footer, footer_size, &footer_block, error, n)) {
            return -1;
        }
        height = title_block.height + subtitle_block.height + (*title && *subtitle ? gap : 0);
        int width =
            title_block.width > subtitle_block.width ? title_block.width : subtitle_block.width;
        width = width > footer_block.width ? width : footer_block.width;
        /* Leave the central block at the canvas center. A footer needs twice its
         * height below that center, plus separation from the central block. */
        int required_h = height;
        if (*footer) {
            required_h =
                height ? height + 2 * (footer_block.height + separation) : footer_block.height;
        }
        if ((width <= available_w && required_h <= available_h) ||
            (title_size == 1 && subtitle_size == 1 && footer_size == 1 && gap == 0)) {
            break;
        }
        double ratio =
            fmin((double)available_w / fmax(1, width), (double)available_h / fmax(1, required_h));
        int new_title = (int)floor(title_size * ratio),
            new_subtitle = (int)floor(subtitle_size * ratio),
            new_footer = (int)floor(footer_size * ratio);
        title_size = (int)fmax(1, fmin(title_size - 1, new_title));
        subtitle_size = (int)fmax(1, fmin(subtitle_size - 1, new_subtitle));
        footer_size = (int)fmax(1, fmin(footer_size - 1, new_footer));
        gap = (int)floor(gap * ratio);
        separation = (int)floor(separation * ratio);
    }
    gap = *title && *subtitle ? gap : 0;
    int top = (frame->height - height) / 2;
    int footer_top = frame->height - margin - footer_block.height;
    int main_bottom = frame->height - margin;
    if (*footer && height) {
        /* Pathological multiline text can remain taller than a tiny canvas even
         * at one pixel. Separate clipping regions keep the blocks from overlap. */
        footer_top = (int)fmax(footer_top, margin + available_h / 2);
        main_bottom = footer_top - separation;
        top = (int)fmax(margin, fmin(top, main_bottom - height));
    }
    uint32_t foreground = blur ? cfg->blur_foreground : cfg->pause_foreground;
    /* Zeroed synthetic configurations used by embedders retain a visible default. */
    if (!cfg->pause_font[0] && !cfg->blur_font[0] && !foreground) {
        foreground = 0xffffff;
    }
    block_draw(text, font, &title_block, title_size, frame, top, margin, margin, main_bottom,
               foreground);
    block_draw(text, font, &subtitle_block, subtitle_size, frame, top + title_block.height + gap,
               margin, margin, main_bottom, foreground);
    block_draw(text, font, &footer_block, footer_size, frame, footer_top, margin,
               (int)fmax(margin, footer_top), frame->height - margin, foreground);
    return 0;
}
