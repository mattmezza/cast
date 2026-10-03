#ifndef CAST_PREVIEW_TEXT_H
#define CAST_PREVIEW_TEXT_H
#include "cast.h"
typedef struct PreviewText PreviewText;
/* Local UI only: each context owns a bundled Inter face and stays on its thread. */
PreviewText *preview_text_create(void);
void preview_text_destroy(PreviewText *);
int preview_text_width(PreviewText *, const char *, int pixels);
int preview_text_draw(PreviewText *, Frame *, const char *, int pixels, int x, int baseline,
                      uint32_t color);
#endif
