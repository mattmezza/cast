#include "cast_theme.h"
#include "brand.h"

#define RGB(r, g, b) ((cui_color){r, g, b, 255})

static cui_color mix(cui_color base, cui_color ink, float amount)
{
    return cui_color_mix_oklab(base, ink, amount);
}

/* Percentages follow ui/build/cast-app.css; mix's amount is the second color. */
CastDesignTokens cast_design_tokens(bool dark)
{
    const cui_color ink = RGB(16, 17, 19), paper = RGB(248, 249, 251);
    const cui_color surface = RGB(24, 26, 29), muted = RGB(151, 158, 168);
    const cui_color orange = RGB(254, 108, 17), line = RGB(68, 73, 81);
    const cui_color live = RGB(131, 221, 182);
    cui_color hair = line;
    hair.a = 158; /* CSS color-mix(line 62%, transparent). */
    return (CastDesignTokens){
        .control = dark ? mix(surface, paper, .26f) : mix(paper, muted, .24f),
        .hover = dark ? mix(surface, paper, .38f) : mix(muted, paper, .74f),
        .selected = dark ? mix(orange, surface, .80f) : mix(orange, paper, .82f),
        .selected_line = dark ? mix(orange, line, .48f) : mix(orange, ink, .42f),
        .secondary = dark ? mix(paper, muted, .28f) : mix(ink, muted, .32f),
        .stage_line = dark ? mix(paper, line, .48f) : mix(paper, line, .24f),
        .live = live,
        .live_ink = dark ? live : mix(live, ink, .60f),
        .hair = hair,
    };
}

cui_theme cast_theme(bool dark)
{
    cui_theme t = dark ? cui_theme_dark() : cui_theme_light();
    const cui_color ink = RGB(16, 17, 19), paper = RGB(248, 249, 251);
    const cui_color orange = RGB(254, 108, 17), live = RGB(131, 221, 182);
    const cui_color danger = dark ? mix(orange, paper, .48f) : mix(orange, ink, .52f);
    const CastDesignTokens design = cast_design_tokens(dark);
    const cui_color muted = RGB(151, 158, 168), line = RGB(68, 73, 81);
    t.dark = dark;
    t.background = dark ? ink : paper;
    t.text = dark ? paper : ink;
    t.surface = dark ? RGB(24, 26, 29) : mix(paper, muted, .10f);
    t.muted = dark ? muted : mix(ink, muted, .58f);
    t.border = dark ? line : mix(line, paper, .66f);
    t.primary = orange;
    t.on_primary = ink;
    /* CUI intent colors also carry text on a surface, so light mode needs ink tones. */
    t.danger = danger;
    t.on_danger = dark ? ink : paper;
    t.success = dark ? live : mix(live, ink, .60f);
    t.on_success = dark ? ink : paper;
    t.warning = dark ? orange : RGB(175, 80, 33);
    t.on_warning = dark ? ink : paper;
    t.info = t.success;
    t.on_info = t.on_success;
    t.focus = dark ? orange : mix(orange, ink, .28f);
    t.font_size = 13;
    t.padding = 10;
    t.gap = 8;
    t.radius = t.radius_lg = 0;
    t.control_height = 40;

    t.secondary = design.control;
    t.secondary_hover = design.hover;
    t.secondary_foreground = t.text;
    t.accent = orange;
    t.accent_foreground = ink;
    t.input = t.border;
    t.input_background = t.surface;
    t.link = t.focus;
    t.link_hover = mix(t.link, t.text, .15f);
    t.link_active = mix(t.link, t.text, .25f);
    t.popover = t.surface;
    t.popover_foreground = t.text;
    t.tab_foreground = t.muted;
    t.tab_active_foreground = t.text;
    t.tab_active = t.surface;
    t.tab_bar = t.background;
    t.tab_bar_segmented = t.secondary;
    t.switch_background = t.border;
    t.switch_thumb = t.text;
    t.list_hover = t.secondary_hover;
    t.selection = design.selected;
    t.list_active = t.selection;
    t.list_active_highlight = true;
    t.description_list_label = t.secondary;
    t.description_list_label_foreground = t.muted;
    t.status_bar = t.background;
    t.status_bar_border = t.border;
    t.title_bar = t.background;
    t.title_bar_border = t.border;
    t.sidebar = t.surface;
    t.sidebar_foreground = t.text;
    t.sidebar_border = t.border;
    t.sidebar_accent = t.selection;
    t.sidebar_accent_foreground = t.text;
    t.caret = t.text;
    t.group_box = t.surface;
    t.group_box_foreground = t.text;
    t.muted_background = t.secondary;
    t.chart_1 = orange;
    t.chart_2 = live;
    t.chart_3 = danger;
    t.chart_4 = t.muted;
    t.chart_5 = mix(orange, live, .5f);
    t.chart_bullish = live;
    t.chart_bearish = danger;
    t.chart_grid = t.border;
    t.scrollbar.custom_thumb_color = true;
    t.scrollbar.custom_track_color = true;
    t.scrollbar.thumb_color = t.border;
    t.scrollbar.track_color = t.background;
    t.scrollbar.thumb_radius = t.scrollbar.track_radius = 0;

    const cui_color backgrounds[CUI_THEME_BUTTON_COUNT] = {
        design.control, orange, t.secondary, t.danger, t.warning, t.success, t.info};
    for (size_t i = 0; i < CUI_THEME_BUTTON_COUNT; ++i) {
        const bool neutral = i == CUI_THEME_BUTTON_DEFAULT ||
                             i == CUI_THEME_BUTTON_SECONDARY;
        const cui_color normal = backgrounds[i];
        const cui_color tone = neutral ? t.text : ink;
        const cui_color outline_ink = neutral ? t.text :
            i == CUI_THEME_BUTTON_PRIMARY || i == CUI_THEME_BUTTON_WARNING ? t.focus :
            mix(t.text, normal, dark ? .7f : .25f);
        t.buttons[i] = (cui_theme_button_colors){
            .normal = normal,
            .hover = neutral ? t.secondary_hover : mix(normal, tone, .08f),
            .active = mix(normal, tone, .16f),
            .foreground = neutral ? t.text :
                i == CUI_THEME_BUTTON_DANGER ? t.on_danger :
                i == CUI_THEME_BUTTON_WARNING ? t.on_warning :
                i == CUI_THEME_BUTTON_SUCCESS ? t.on_success :
                i == CUI_THEME_BUTTON_INFO ? t.on_info : ink,
            .outline_normal = t.surface,
            .outline_hover = mix(t.surface, normal, neutral ? .1f : .12f),
            .outline_active = mix(t.surface, normal, neutral ? .2f : .22f),
            .border = neutral ? t.border : normal,
            .outline_border = neutral ? t.border : outline_ink,
            .outline_foreground = outline_ink,
        };
    }
    cast_brand_apply(&t);
    return t;
}
