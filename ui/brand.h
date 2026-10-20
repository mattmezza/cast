#ifndef CAST_BRAND_H
#define CAST_BRAND_H
#include "cui/cui.h"
#include "cui/cui_theme.h"
typedef struct {
    cui_color control, hover, selected, selected_line, secondary;
    cui_color stage_line, live, live_ink, hair;
} CastDesignTokens;
CastDesignTokens cast_design_tokens(bool dark);
bool cast_brand_icon_loader(void *user, cui_str path, cui_str *svg);
void cast_brand_apply(cui_theme *theme);
/* Canonical app mark, backed by a static 36px RGBA bitmap; size is logical pixels. */
void cast_brand_logo(cui_context *ui, cui_id id, float size);
#endif
