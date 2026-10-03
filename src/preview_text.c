#include "preview_text.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>

extern const unsigned char cast_panel_font_data[], cast_panel_font_end[];
struct PreviewText {
    FT_Library library;
    FT_Face face;
    int size;
};
PreviewText *preview_text_create(void)
{
    PreviewText *text = calloc(1, sizeof *text);
    if (!text) {
        return NULL;
    }
    if (FT_Init_FreeType(&text->library) ||
        FT_New_Memory_Face(text->library, cast_panel_font_data,
                           (FT_Long)(cast_panel_font_end - cast_panel_font_data), 0, &text->face)) {
        preview_text_destroy(text);
        return NULL;
    }
    return text;
}
void preview_text_destroy(PreviewText *text)
{
    if (!text) {
        return;
    }
    if (text->face) {
        FT_Done_Face(text->face);
    }
    if (text->library) {
        FT_Done_FreeType(text->library);
    }
    free(text);
}
static int font_size(PreviewText *text, int pixels)
{
    if (!text || pixels < 1 || pixels > 192) {
        return -1;
    }
    if (text->size != pixels) {
        if (FT_Set_Pixel_Sizes(text->face, 0, (FT_UInt)pixels)) {
            return -1;
        }
        text->size = pixels;
    }
    return 0;
}
/* UI strings are bounded UTF-8 labels, never the configured presentation text. */
static uint32_t codepoint(const unsigned char **input)
{
    const unsigned char *p = *input;
    uint32_t value = *p++;
    unsigned count = value < 128 ? 0 : value < 224 ? 1 : value < 240 ? 2 : 3;
    if (count) {
        value &= (1u << (6 - count)) - 1;
        for (unsigned i = 0; i < count; i++) {
            if (!*p || (*p & 0xc0) != 0x80) {
                *input = p;
                return '?';
            }
            value = (value << 6) | (*p++ & 0x3f);
        }
    }
    *input = p;
    return value;
}
static int render(PreviewText *text, Frame *frame, const char *label, int pixels, int origin,
                  int baseline, uint32_t color)
{
    if (font_size(text, pixels)) {
        return -1;
    }
    int x = origin;
    FT_UInt previous = 0;
    const unsigned char *input = (const unsigned char *)label;
    while (*input) {
        FT_UInt glyph = FT_Get_Char_Index(text->face, codepoint(&input));
        if (previous && glyph && FT_HAS_KERNING(text->face)) {
            FT_Vector adjustment;
            if (!FT_Get_Kerning(text->face, previous, glyph, FT_KERNING_DEFAULT, &adjustment)) {
                x += (int)(adjustment.x >> 6);
            }
        }
        if (FT_Load_Glyph(text->face, glyph, FT_LOAD_DEFAULT) ||
            (frame && FT_Render_Glyph(text->face->glyph, FT_RENDER_MODE_NORMAL))) {
            return -1;
        }
        FT_GlyphSlot slot = text->face->glyph;
        if (frame) {
            const FT_Bitmap *bitmap = &slot->bitmap;
            for (unsigned y = 0; y < bitmap->rows; y++) {
                int destination_y = baseline - slot->bitmap_top + (int)y;
                if (destination_y < 0 || destination_y >= frame->height) {
                    continue;
                }
                const unsigned char *row =
                    bitmap->buffer +
                    (bitmap->pitch >= 0 ? (int)y : (int)bitmap->rows - 1 - (int)y) *
                        abs(bitmap->pitch);
                for (unsigned column = 0; column < bitmap->width; column++) {
                    int destination_x = x + slot->bitmap_left + (int)column;
                    if (destination_x < 0 || destination_x >= frame->width) {
                        continue;
                    }
                    uint8_t *pixel =
                        frame->data + (size_t)destination_y * frame->stride + destination_x * 4;
                    unsigned coverage = row[column];
                    for (int channel = 0; channel < 3; channel++) {
                        unsigned foreground = (color >> (16 - channel * 8)) & 255;
                        pixel[channel] = (uint8_t)((foreground * coverage +
                                                    pixel[channel] * (255 - coverage) + 127) /
                                                   255);
                    }
                    pixel[3] = 255;
                }
            }
        }
        x += (int)(slot->advance.x >> 6);
        previous = glyph;
    }
    return x - origin;
}
int preview_text_width(PreviewText *text, const char *label, int pixels)
{
    return render(text, NULL, label, pixels, 0, 0, 0);
}
int preview_text_draw(PreviewText *text, Frame *frame, const char *label, int pixels, int x,
                      int baseline, uint32_t color)
{
    return render(text, frame, label, pixels, x, baseline, color);
}
