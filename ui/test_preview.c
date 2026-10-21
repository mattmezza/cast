/* SPDX-License-Identifier: MIT */
#include "app.h"
#ifdef CAST_TEST_NATIVE_FONTS
#include "cui/cui_text.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef CAST_TEST_NATIVE_FONTS
static cui_font *geometry_font,*geometry_symbol,*geometry_variants[6];
static void native_fonts_close(void) {
    cui_font_close(geometry_font);
    cui_font_close(geometry_symbol);
    for(unsigned i=0;i<6;++i)cui_font_close(geometry_variants[i]);
}
static void native_fonts_open(void) {
    const char *directory=getenv("CAST_TEST_FONT_DIR");assert(directory);
    char path[4096];snprintf(path,sizeof path,"%s/Inter-Regular.otf",directory);
    geometry_font=cui_font_open(path);assert(geometry_font);
    const char *names[]={"Inter-Bold.otf","Inter-Medium.ttf","Inter-SemiBold.ttf",
        "IBMPlexMono-Regular.ttf","IBMPlexMono-Medium.ttf","IBMPlexMono-SemiBold.ttf"};
    const cui_text_style styles[]={
        {.weight=CUI_TEXT_WEIGHT_BOLD},{.weight=CUI_TEXT_WEIGHT_MEDIUM},{.weight=CUI_TEXT_WEIGHT_SEMIBOLD},
        {.family=1,.monospace=true},{.family=1,.monospace=true,.weight=CUI_TEXT_WEIGHT_MEDIUM},
        {.family=1,.monospace=true,.weight=CUI_TEXT_WEIGHT_SEMIBOLD}};
    for(unsigned i=0;i<6;++i) {
        snprintf(path,sizeof path,"%s/%s",directory,names[i]);
        geometry_variants[i]=cui_font_open(path);assert(geometry_variants[i]);
        assert(cui_font_set_style(geometry_font,styles[i],geometry_variants[i]));
    }
    snprintf(path,sizeof path,"%s/Inter-Regular.otf",directory);
    geometry_symbol=cui_font_open(path);assert(geometry_symbol);
    assert(cui_font_set_fallbacks(geometry_font,&geometry_symbol,1));
    atexit(native_fonts_close);
}
#endif
typedef struct {
    cui_context *ui;
    AppState app;
    float width, height;
    unsigned live_bitmap_count;
    cui_draw_list draw;
} Preview;

#ifndef CAST_TEST_NATIVE_FONTS
static cui_size measure(void *user, cui_str text, float size) {
    (void)user;
    return (cui_size){text.length * size * .6f, size * 1.2f};
}
#endif
static cui_id id(Preview *p, const char *name) { return cui_id_from(p->ui, name); }
static cui_rect bounds(Preview *p, const char *name) { return cui_bounds(p->ui, id(p, name)); }
static void frame(Preview *p) {
    bool composition_blocked = p->app.edit_requested || p->app.live.pending ||
        (p->app.live.enabled && !p->app.live.connected);
    assert(cui_begin_frame(p->ui, p->width, p->height, .016f));
    app_frame(p->ui, &p->app, p->width, p->height);
    size_t count = 0;
    (void)cui_frame_events(p->ui, &count);
    for (size_t i = 0; i < count; ++i) {
        cui_id accepted = cui_event_accepted_activation(p->ui, i);
        if (p->app.workspace != APP_RECORD || !p->app.live.enabled ||
            !p->app.live.connected || composition_blocked || p->app.live.record_finalizing)
            assert(accepted != id(p, "output-start"));
        const char *disabled_output[] = {"menu-output-item-0", "menu-output-item-3", "menu-output-item-5"};
        for (size_t j = 0; j < sizeof disabled_output / sizeof disabled_output[0]; ++j)
            assert(accepted != id(p, disabled_output[j]));
        if (!p->app.live.enabled || !p->app.live.connected || composition_blocked || p->app.live.record_finalizing) {
            assert(accepted != id(p, "menu-output-item-1"));
            assert(accepted != id(p, "menu-output-item-2"));
        }
        if (composition_blocked) {
            const char *readonly[] = {"layout-overlay", "layout-split", "layout-screen",
                "layout-camera", "layout-stage", "fit-contain", "fit-cover",
                "anchor-0", "anchor-1", "anchor-2", "anchor-3", "anchor-4",
                "anchor-5", "anchor-6", "anchor-7", "anchor-8",
                "camera-size-0", "camera-size-1", "camera-size-2",
                "director-framing-0", "director-framing-1", "director-framing-2",
                "stagebar-layout-overlay", "stagebar-layout-split", "stagebar-layout-screen",
                "stagebar-layout-camera", "stagebar-layout-stage",
                "menu-composition-item-0", "menu-composition-item-1", "menu-composition-item-2",
                "menu-composition-item-3", "menu-composition-item-4"};
            for (size_t j = 0; j < sizeof readonly / sizeof readonly[0]; ++j)
                assert(accepted != id(p, readonly[j]));
        }
    }
    cui_draw_list draw = cui_end_frame(p->ui);
    p->draw = draw;
    assert(draw.count > 0);
    p->live_bitmap_count = 0;
    for (size_t i = 0; i < draw.count; ++i)
        if (draw.commands[i].type == CUI_DRAW_IMAGE &&
            draw.commands[i].bitmap.bitmap.pixels == p->app.live.frame.pixels &&
            p->app.live.frame.pixels)
            ++p->live_bitmap_count;
    if (cui_last_error(p->ui) != CUI_OK) {
        cui_diagnostic diagnostic;
        if (cui_first_diagnostic(p->ui, &diagnostic))
            fprintf(stderr, "CUI %s: %s (id=%llu, used=%zu, limit=%zu)\n",
                diagnostic.operation, diagnostic.message, (unsigned long long)diagnostic.id,
                diagnostic.used, diagnostic.limit);
    }
    assert(cui_last_error(p->ui) == CUI_OK);
}
static void settle(Preview *p) { frame(p); frame(p); }
static Preview create(float width, float height) {
    Preview p = {.width = width, .height = height};
    #ifdef CAST_TEST_NATIVE_FONTS
    p.ui=cui_create((cui_config){.width=width,.height=height,.measure=cui_font_measure,
        .measure_styled=cui_font_measure_styled,.text_geometry=cui_font_text_geometry,
        .text_geometry_line=cui_font_text_geometry_line,.font_metrics=cui_font_get_metrics,
        .measure_user=geometry_font});
#else
    p.ui = cui_create((cui_config){.width = width, .height = height, .measure = measure});
#endif
    assert(p.ui);
    app_init(&p.app);
    settle(&p);
    return p;
}
static void event(Preview *p, cui_event e) {
    assert(cui_queue_event(p->ui, e));
    frame(p);
}
static void key(Preview *p, cui_key k, unsigned modifiers) {
    event(p, (cui_event){.type = CUI_EVENT_KEY_DOWN, .key = k, .modifiers = modifiers});
    event(p, (cui_event){.type = CUI_EVENT_KEY_UP, .key = k, .modifiers = modifiers});
    settle(p);
}
static void click(Preview *p, const char *name) {
    cui_rect r = bounds(p, name);
    if (!(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 &&
          r.x + r.w <= p->width + 1 && r.y + r.h <= p->height + 1)) {
        fprintf(stderr, "Unreachable %s at %.1f %.1f %.1f %.1f in %.0fx%.0f\n",
                name, r.x, r.y, r.w, r.h, p->width, p->height);
        assert(false);
    }
    float x = r.x + r.w / 2, y = r.y + r.h / 2;
    event(p, (cui_event){.type = CUI_EVENT_POINTER_DOWN, .x = x, .y = y});
    event(p, (cui_event){.type = CUI_EVENT_POINTER_UP, .x = x, .y = y});
    settle(p);
}
static void inside(cui_rect child, cui_rect parent) {
    assert(child.w > 0 && child.h > 0);
    assert(child.x >= parent.x - 1 && child.y >= parent.y - 1);
    assert(child.x + child.w <= parent.x + parent.w + 1);
    assert(child.y + child.h <= parent.y + parent.h + 1);
}
static bool overlap(cui_rect a, cui_rect b) {
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 &&
           a.x < b.x + b.w - 1 && b.x < a.x + a.w - 1 &&
           a.y < b.y + b.h - 1 && b.y < a.y + a.h - 1;
}
static void same(cui_rect a, cui_rect b) {
    assert(fabsf(a.x - b.x) < 1 && fabsf(a.y - b.y) < 1);
    assert(fabsf(a.w - b.w) < 1 && fabsf(a.h - b.h) < 1);
}
static void geometry(Preview *p) {
    cui_rect window = {0, 0, p->width, p->height};
    cui_rect root = bounds(p, "root"), workspace = bounds(p, "workspace");
    inside(workspace, root);
    inside(workspace, window);
    inside(bounds(p, "statusbar"), root);
    inside(bounds(p, "statusbar"), window);
    inside(bounds(p, "center-header"), bounds(p, "center"));
    inside(bounds(p, "stagebar"), bounds(p, "center"));
    cui_rect stage = bounds(p, "composition-stage");
    inside(stage, bounds(p, "stage-area"));
    inside(stage, bounds(p, "center"));
    inside(stage, bounds(p, "workspace"));
    inside(stage, (cui_rect){0, 0, p->width, p->height});
    assert(fabsf(stage.w / stage.h - 16.f / 9) < .02f);
    if (p->width >= 1000 && p->app.show_sources) {
        cui_rect sources = bounds(p, "sources-panel");
        assert(sources.w > 0 && !overlap(sources, stage));
        inside(sources, bounds(p, "workspace"));
    }
    if (p->width >= 800 && p->app.show_inspector) {
        cui_rect output = bounds(p, "inspector-panel");
        assert(output.w > 0 && !overlap(output, stage));
        inside(output, bounds(p, "workspace"));
    }
}
static void workspaces(void) {
    Preview p = create(1360, 820);
    const char *tabs[] = {"workspace-camera", "workspace-record", "workspace-stream"};
    cui_rect stage = bounds(&p, "composition-stage");
    for (unsigned i = 0; i < 3; ++i) {
        click(&p, tabs[i]);
        assert(p.app.workspace == (AppWorkspace)i);
        same(stage, bounds(&p, "composition-stage"));
    }
    for (unsigned i = 0; i < 3; ++i) {
        key(&p, (cui_key)(CUI_KEY_1 + i), 0);
        assert(p.app.workspace == (AppWorkspace)i);
        same(stage, bounds(&p, "composition-stage"));
    }
    key(&p, CUI_KEY_1, CUI_MOD_CTRL);
    assert(p.app.workspace == APP_STREAM);
    click(&p, "workspace-camera");
    key(&p, CUI_KEY_RIGHT, 0);
    assert(p.app.workspace == APP_RECORD);
    assert(cui_focused(p.ui) == id(&p, "workspace-record"));
    key(&p, CUI_KEY_RIGHT, 0);
    assert(p.app.workspace == APP_STREAM);
    key(&p, CUI_KEY_LEFT, 0);
    assert(p.app.workspace == APP_RECORD);
    cui_destroy(p.ui);
}
static void compositions(void) {
    Preview p = create(1360, 1000);
    const char *layouts[] = {"layout-overlay", "layout-split", "layout-screen", "layout-camera", "layout-stage"};
    cui_rect stage = bounds(&p, "composition-stage");
    for (unsigned i = 0; i < APP_LAYOUT_COUNT; ++i) {
        click(&p, layouts[i]);
        assert(p.app.layout == (AppLayout)i);
        same(stage, bounds(&p, "composition-stage"));
        for (unsigned j = 0; j < 3; ++j) {
            key(&p, (cui_key)(CUI_KEY_1 + j), 0);
            assert(p.app.layout == (AppLayout)i);
            same(stage, bounds(&p, "composition-stage"));
        }
        key(&p, CUI_KEY_1, 0);
    }
    click(&p, "layout-overlay");
    click(&p, "fit-cover");
    assert(p.app.fit_cover);
    click(&p, "fit-contain");
    assert(!p.app.fit_cover);
    /* Use real wheel routing when the camera controls extend below the viewport. */
    for (unsigned n = 0; n < 24; ++n) {
        cui_rect r = bounds(&p, "camera-size-2"), scroll = bounds(&p, "sources-scroll");
        if (r.y >= scroll.y && r.y + r.h <= scroll.y + scroll.h) break;
        event(&p, (cui_event){.type = CUI_EVENT_SCROLL, .x = scroll.x + scroll.w / 2,
              .y = scroll.y + scroll.h / 2, .dy = -1});
        settle(&p);
    }
    click(&p, "anchor-0");
    assert(p.app.camera_anchor == 0);
    cui_rect first = bounds(&p, "stage-camera");
    click(&p, "anchor-8");
    assert(p.app.camera_anchor == 8);
    cui_rect last = bounds(&p, "stage-camera");
    assert(last.x > first.x && last.y > first.y);
    click(&p, "camera-size-0");
    assert(p.app.camera_size == 20);
    cui_rect wide = bounds(&p, "stage-camera");
    click(&p, "camera-size-2");
    assert(p.app.camera_size == 38);
    assert(bounds(&p, "stage-camera").w > wide.w);
    cui_destroy(p.ui);
}
static void panels(void) {
    Preview p = create(1360, 820);
    cui_rect before = bounds(&p, "composition-stage");
    click(&p, "toggle-sources");
    assert(!p.app.show_sources);
    inside(bounds(&p, "left-reopen-rail"), bounds(&p, "center"));
    geometry(&p);
    assert(bounds(&p, "center").w > before.w);
    click(&p, "toggle-sources");
    assert(p.app.show_sources);
    same(before, bounds(&p, "composition-stage"));
    click(&p, "toggle-inspector");
    assert(!p.app.show_inspector);
    inside(bounds(&p, "right-reopen-rail"), bounds(&p, "center"));
    geometry(&p);
    key(&p, CUI_KEY_RIGHT_BRACKET, 0);
    assert(p.app.show_inspector);
    key(&p, CUI_KEY_LEFT_BRACKET, 0);
    assert(!p.app.show_sources);
    key(&p, CUI_KEY_LEFT_BRACKET, 0);
    assert(p.app.show_sources);
    same(before, bounds(&p, "composition-stage"));
    click(&p, "output-start"); /* frame() rejects any accepted disabled activation. */
    assert(!cui_accessibility_action_supported(p.ui, id(&p, "output-start"), CUI_ACCESSIBILITY_ACTIVATE));
    assert(!cui_queue_accessibility_action(p.ui, id(&p, "output-start"), CUI_ACCESSIBILITY_ACTIVATE));
    cui_destroy(p.ui);
    p = create(480, 820);
    const char *toggles[] = {"toggle-sources", "toggle-inspector"};
    cui_key keys[] = {CUI_KEY_LEFT_BRACKET, CUI_KEY_RIGHT_BRACKET};
    for (unsigned i = 0; i < 2; ++i) {
        click(&p, toggles[i]);
        assert(p.app.panel_sheet.overlay.open && p.app.drawer_kind == i + 1);
        click(&p, "workspace-stream");
        assert(p.app.workspace == APP_CAMERA);
        /* An outside click may dismiss a sheet, but cannot activate a tab below it. */
        if (!p.app.panel_sheet.overlay.open) click(&p, toggles[i]);
        key(&p, CUI_KEY_TAB, 0);
        assert(cui_focus_within(p.ui, p.app.panel_sheet.overlay.id));
        key(&p, CUI_KEY_TAB, CUI_MOD_SHIFT);
        assert(cui_focus_within(p.ui, p.app.panel_sheet.overlay.id));
        key(&p, CUI_KEY_3, 0);
        assert(p.app.workspace == APP_CAMERA);
        key(&p, CUI_KEY_ESCAPE, 0);
        assert(!p.app.panel_sheet.overlay.open);
        if (cui_focused(p.ui) != id(&p, toggles[i]))
            fprintf(stderr, "Drawer %u focus %llu expected %llu\n", i, (unsigned long long)cui_focused(p.ui), (unsigned long long)id(&p, toggles[i]));
        assert(cui_focused(p.ui) == id(&p, toggles[i]));
        key(&p, keys[i], 0);
        assert(p.app.panel_sheet.overlay.open && p.app.drawer_kind == i + 1);
        key(&p, CUI_KEY_ESCAPE, 0);
        assert(!p.app.panel_sheet.overlay.open);
        key(&p, keys[i], 0);
        assert(p.app.panel_sheet.overlay.open);
        p.width = 1360;
        settle(&p);
        assert(!p.app.panel_sheet.overlay.open);
        assert(bounds(&p, i == 0 ? "sources-panel" : "inspector-panel").w > 0);
        geometry(&p); /* Duplicate mounted panel IDs also fail frame's CUI error check. */
        p.width = 480;
        settle(&p);
    }
    cui_destroy(p.ui);
}
static void settings(void) {
    Preview p = create(1360, 820);
    click(&p, "settings-open");
    assert(p.app.settings.overlay.open);
    cui_rect dialog = cui_bounds(p.ui, p.app.settings.overlay.id);
    assert(fabsf(dialog.w - 1020) < 1 && fabsf(dialog.h - 720) < 1);
    inside(dialog, (cui_rect){0, 0, p.width, p.height});
    cui_rect header = cui_bounds(p.ui, cui_surface_part_id(p.app.settings.overlay.id, 4));
    assert(fabsf(header.h - 38) < 1);
    assert(fabsf(bounds(&p, "settings-rail").w - 214) < 1);
    inside(header, dialog);
    inside(bounds(&p, "settings-rail"), dialog);
    inside(bounds(&p, "settings-pane"), dialog);
    click(&p, "workspace-stream");
    assert(p.app.workspace == APP_CAMERA && p.app.settings.overlay.open);
    for (unsigned i = 0; i < 16; ++i) {
        key(&p, CUI_KEY_TAB, i < 8 ? 0 : CUI_MOD_SHIFT);
        assert(cui_focus_within(p.ui, p.app.settings.overlay.id));
    }
    const char *categories[] = {"settings-category-0", "settings-category-1",
        "settings-category-2", "settings-category-3", "settings-category-4", "settings-category-5", "settings-category-6",
        "settings-category-7", "settings-category-8", "settings-category-9"};
    for (unsigned i = 0; i < 10; ++i) {
        click(&p, categories[i]);
        assert(p.app.settings_category == i);
    }
    click(&p, "settings-category-0");
    assert(p.app.settings_category == 0);
    key(&p, CUI_KEY_3, 0);
    assert(p.app.workspace == APP_CAMERA);
    click(&p, "appearance-light");
    assert(p.app.appearance == APP_LIGHT && !app_is_dark(&p.app));
    click(&p, "appearance-dark");
    assert(p.app.appearance == APP_DARK && app_is_dark(&p.app));
    click(&p, "appearance-system");
    assert(p.app.appearance == APP_SYSTEM);
    p.app.system_dark = false;
    settle(&p);
    assert(!app_is_dark(&p.app));
    p.app.system_dark = true;
    settle(&p);
    assert(app_is_dark(&p.app));
    key(&p, CUI_KEY_ESCAPE, 0);
    assert(!p.app.settings.overlay.open);
    assert(cui_focused(p.ui) == id(&p, "settings-open"));
    key(&p, CUI_KEY_3, 0);
    assert(p.app.workspace == APP_STREAM);
    cui_destroy(p.ui);
}
static void settings_controls_fit(void) {
    const float widths[] = {480, 800, 1360};
    const unsigned expected_sliders[] = {0, 1, 2, 3, 2, 0, 0, 0, 0, 0};
    for (size_t w = 0; w < sizeof widths / sizeof widths[0]; ++w) {
        Preview p = create(widths[w], 820);
        click(&p, "settings-open");
        for (unsigned category = 0; category < 10; ++category) {
            char name[40]; snprintf(name, sizeof name, "settings-category-%u", category);
            click(&p, name);
            const cui_rect pane = bounds(&p, "settings-pane");
            const cui_rect dialog = cui_bounds(p.ui, p.app.settings.overlay.id);
            size_t count = 0; unsigned sliders = 0;
            const cui_semantic *semantics = cui_semantics(p.ui, &count);
            for (size_t i = 0; i < count; ++i) {
                const cui_semantic *slider = &semantics[i];
                if (slider->role != CUI_ROLE_SLIDER) continue;
                /* The disabled Director slider remains below the modal, so check
                 * semantic ancestry before applying settings geometry assertions. */
                cui_id parent = slider->id;
                bool settings_child = false;
                for (unsigned depth = 0; parent && depth < 32; ++depth) {
                    if (parent == p.app.settings.overlay.id) { settings_child = true; break; }
                    parent = cui_semantic_parent(p.ui, parent);
                }
                if (!settings_child) continue;
                ++sliders;
                assert(slider->disabled && slider->name.length > 0);
                assert(slider->bounds.w > 0 && slider->bounds.w <= 160);
                assert(slider->bounds.x >= pane.x && slider->bounds.x + slider->bounds.w <= pane.x + pane.w);
                assert(slider->bounds.x >= dialog.x && slider->bounds.x + slider->bounds.w <= dialog.x + dialog.w);
                cui_rect track = cui_bounds(p.ui, slider->id ^ UINT64_C(0x7b1a0ed95e63cf28));
                assert(fabsf(track.x - slider->bounds.x) < 1 && fabsf(track.w - slider->bounds.w) < 1);
                assert(!cui_accessibility_action_supported(p.ui, slider->id, CUI_ACCESSIBILITY_ACTIVATE));
            }
            assert(sliders == expected_sliders[category]);
            cui_rect close = cui_bounds(p.ui, cui_surface_part_id(p.app.settings.overlay.id, 1));
            cui_rect glyph = bounds(&p, "settings-close-art");
            inside(glyph, close);
            assert(fabsf((glyph.x + glyph.w / 2) - (close.x + close.w / 2)) < 1);
            assert(fabsf((glyph.y + glyph.h / 2) - (close.y + close.h / 2)) < 1);
        }
        cui_rect close = cui_bounds(p.ui, cui_surface_part_id(p.app.settings.overlay.id, 1));
        event(&p, (cui_event){.type = CUI_EVENT_POINTER_DOWN, .x = close.x + close.w / 2, .y = close.y + close.h / 2});
        event(&p, (cui_event){.type = CUI_EVENT_POINTER_UP, .x = close.x + close.w / 2, .y = close.y + close.h / 2});
        settle(&p);
        assert(!p.app.settings.overlay.open && cui_focused(p.ui) == id(&p, "settings-open"));
        cui_destroy(p.ui);
    }
}
static void menus(void) {
    Preview p = create(1360, 820);
    const char *triggers[] = {"menu-file", "menu-edit", "menu-view",
        "menu-composition", "menu-output", "menu-help"};
    for (unsigned i = 0; i < 6; ++i) {
        click(&p, triggers[i]);
        assert(p.app.menus[i].overlay.open);
        if (i == 0) {
            cui_draw_list draw = p.draw;
            cui_rect label = {0}, shortcut = {0};
            for (size_t c = 0; c < draw.count; ++c) {
                const cui_draw_command *command = &draw.commands[c];
                if (command->type != CUI_DRAW_TEXT) continue;
                if (command->text.length == 11 && !memcmp(command->text.data, "New project", 11))
                    label = command->bounds;
                if (command->text.length == 6 && !memcmp(command->text.data, "Ctrl+N", 6))
                    shortcut = command->bounds;
            }
            assert(label.w > 0 && shortcut.w > 0);
            assert(shortcut.x > label.x + label.w);
            assert(fabsf(label.y + label.h / 2 - shortcut.y - shortcut.h / 2) < 1);
        }
        for (unsigned j = 0; j < 6; ++j)
            assert(p.app.menus[j].overlay.open == (i == j));
        key(&p, CUI_KEY_ESCAPE, 0);
        assert(!p.app.menus[i].overlay.open);
        cui_focus(p.ui, id(&p, triggers[i]));
        key(&p, CUI_KEY_ENTER, 0);
        assert(p.app.menus[i].overlay.open);
        key(&p, CUI_KEY_ESCAPE, 0);
        assert(!p.app.menus[i].overlay.open);
    }
    click(&p, "menu-view");
    click(&p, "menu-view-item-0");
    assert(!p.app.show_sources);
    click(&p, "menu-view");
    click(&p, "menu-view-item-0");
    assert(p.app.show_sources);
    click(&p, "menu-view");
    click(&p, "menu-view-item-1");
    assert(!p.app.show_inspector);
    click(&p, "menu-view");
    click(&p, "menu-view-item-1");
    assert(p.app.show_inspector);
    for (unsigned i = 0; i < APP_LAYOUT_COUNT; ++i) {
        char name[48]; snprintf(name, sizeof name, "menu-composition-item-%u", i);
        click(&p, "menu-composition");
        click(&p, name);
        assert(p.app.layout == (AppLayout)i);
    }
    click(&p, "menu-output");
    const unsigned output_items[] = {0, 1, 2, 3, 5};
    for (unsigned i = 0; i < sizeof output_items / sizeof output_items[0]; ++i) {
        char name[40]; snprintf(name, sizeof name, "menu-output-item-%u", output_items[i]);
        click(&p, name);
        assert(p.app.menus[4].overlay.open);
        assert(!p.app.quit_requested);
        assert(!cui_accessibility_action_supported(p.ui, id(&p, name), CUI_ACCESSIBILITY_ACTIVATE));
    }
    key(&p, CUI_KEY_ENTER, 0);
    assert(p.app.menus[4].overlay.open && !p.app.quit_requested);
    key(&p, CUI_KEY_ESCAPE, 0);
    click(&p, "menu-help");
    /* Outside pointer dismissal must not activate the disabled output action. */
    click(&p, "output-start");
    assert(!p.app.menus[5].overlay.open);
    p.app.live.enabled = true;
    AppLayout acknowledged = p.app.layout;
    click(&p, "menu-composition");
    click(&p, "menu-composition-item-0");
    assert(p.app.layout == acknowledged && p.app.menus[3].overlay.open);
    key(&p, CUI_KEY_ESCAPE, 0);
    click(&p, "menu-file");
    click(&p, "menu-file-item-8");
    assert(p.app.quit_requested);
    cui_destroy(p.ui);
}
static void responsive(void) {
    const float widths[] = {480, 800, 1000, 1360};
    const float heights[] = {360, 820};
    for (unsigned w = 0; w < 4; ++w)
        for (unsigned h = 0; h < 2; ++h) {
            Preview p = create(widths[w], heights[h]);
            for (unsigned i = 0; i < 3; ++i) {
                key(&p, (cui_key)(CUI_KEY_1 + i), 0);
                geometry(&p);
            }
            cui_destroy(p.ui);
        }
}
static void live_composition_edits(void) {
    Preview p = create(1360, 1200);
    p.app.live = (AppLive){.enabled = true, .connected = true,
        .width = 640, .height = 480, .fps = 30, .camera_width_percent = 26};
    snprintf(p.app.live.layout, sizeof p.app.live.layout, "split");
    snprintf(p.app.live.fit, sizeof p.app.live.fit, "cover");
    snprintf(p.app.live.anchor, sizeof p.app.live.anchor, "bottom-right");
    settle(&p);
    const AppLive acknowledged = p.app.live;
    /* Pointer activation emits exactly one request without changing acknowledged selection. */
    click(&p, "layout-overlay");
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_LAYOUT && !strcmp(p.app.edit_value, "overlay"));
    assert(p.app.layout == APP_SPLIT && !memcmp(&p.app.live, &acknowledged, sizeof acknowledged));
    click(&p, "fit-contain");
    assert(p.app.edit == APP_EDIT_LAYOUT && !strcmp(p.app.edit_value, "overlay") && p.app.fit_cover);
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "38"));
    /* Host submits once; bridge remains pending until an acknowledged config arrives. */
    p.app.edit_requested = false;
    p.app.live.pending = true;
    settle(&p);
    click(&p, "camera-size-2");
    assert(!p.app.edit_requested && p.app.camera_size == 26);
    p.app.live.pending = false;
    snprintf(p.app.live.layout, sizeof p.app.live.layout, "overlay");
    settle(&p);
    assert(p.app.layout == APP_OVERLAY);
    /* Keyboard activation follows the same controlled intent contract. */
    cui_focus(p.ui, id(&p, "fit-contain"));
    key(&p, CUI_KEY_ENTER, 0);
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_FIT && !strcmp(p.app.edit_value, "contain"));
    assert(p.app.fit_cover);
    p.app.edit_requested = false;
    snprintf(p.app.live.fit, sizeof p.app.live.fit, "contain");
    settle(&p);
    assert(!p.app.fit_cover);
    click(&p, "anchor-1");
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_ANCHOR && !strcmp(p.app.edit_value, "top"));
    assert(p.app.camera_anchor == 8);
    p.app.edit_requested = false;
    snprintf(p.app.live.anchor, sizeof p.app.live.anchor, "top");
    settle(&p);
    assert(p.app.camera_anchor == 1);
    click(&p, "anchor-4");
    assert(!p.app.edit_requested && p.app.camera_anchor == 1);
    assert(!cui_accessibility_action_supported(p.ui, id(&p, "anchor-4"), CUI_ACCESSIBILITY_ACTIVATE));
    assert(!app_request_edit(&p.app, APP_EDIT_ANCHOR, "center"));
    click(&p, "camera-size-2");
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_SIZE && !strcmp(p.app.edit_value, "38"));
    assert(p.app.camera_size == 26);
    p.app.edit_requested = false;
    p.app.live.camera_width_percent = 38;
    settle(&p);
    assert(p.app.camera_size == 38);
    click(&p, "director-framing-0");
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_SIZE && !strcmp(p.app.edit_value, "20"));
    assert(p.app.camera_size == 38);
    p.app.edit_requested = false;
    settle(&p);
    click(&p, "stagebar-layout-screen");
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_LAYOUT && !strcmp(p.app.edit_value, "screen"));
    assert(p.app.layout == APP_OVERLAY);
    p.app.edit_requested = false;
    settle(&p);
    click(&p, "menu-composition");
    click(&p, "menu-composition-item-4");
    assert(p.app.edit_requested && p.app.edit == APP_EDIT_LAYOUT && !strcmp(p.app.edit_value, "stage"));
    assert(p.app.layout == APP_OVERLAY);
    p.app.edit_requested = false;
    p.app.live.connected = false;
    settle(&p);
    click(&p, "layout-camera");
    cui_focus(p.ui, id(&p, "fit-cover"));
    key(&p, CUI_KEY_SPACE, 0);
    assert(!p.app.edit_requested && p.app.layout == APP_OVERLAY && !p.app.fit_cover);
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "20"));
    /* Invalid values never leave the UI, even while connected. */
    p.app.live.connected = true;
    assert(!app_request_edit(&p.app, APP_EDIT_LAYOUT, "unknown"));
    assert(!app_request_edit(&p.app, APP_EDIT_FIT, "stretch"));
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "nan"));
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "101"));
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "20px"));
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "1e2"));
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, " 20"));
    assert(!app_request_edit(&p.app, APP_EDIT_SIZE, "20.0.0"));
    assert(!p.app.edit_requested);
    cui_destroy(p.ui);
}
static void live_preview_target_and_aspect(void) {
    static const unsigned char pixels[] = {20, 40, 60, 255};
    Preview p = create(1360, 820);
    p.app.live = (AppLive){.enabled = true, .connected = true, .has_frame = true,
        .target = APP_CAMERA, .width = 640, .height = 480, .fps = 30,
        .camera_width_percent = 26,
        .frame = {.pixels = pixels, .width = 1, .height = 1, .stride = 4, .revision = 1}};
    snprintf(p.app.live.layout, sizeof p.app.live.layout, "overlay");
    snprintf(p.app.live.fit, sizeof p.app.live.fit, "contain");
    snprintf(p.app.live.anchor, sizeof p.app.live.anchor, "bottom-right");
    settle(&p);
    cui_rect stage = bounds(&p, "composition-stage");
    assert(fabsf(stage.w / stage.h - 4.f / 3) < .02f);
    inside(stage, bounds(&p, "stage-area"));
    assert(p.live_bitmap_count == 1);
    key(&p, CUI_KEY_2, 0);
    assert(p.app.workspace == APP_RECORD && p.live_bitmap_count == 0);
    p.app.live.target = APP_RECORD;
    settle(&p);
    assert(p.live_bitmap_count == 1);
    click(&p, "workspace-stream");
    assert(p.app.workspace == APP_STREAM && p.live_bitmap_count == 0);
    p.app.live.target = APP_STREAM;
    settle(&p);
    assert(p.live_bitmap_count == 1);
    p.app.live.has_frame = false;
    settle(&p);
    assert(p.live_bitmap_count == 0);
    p.app.live.has_frame = true;
    p.app.live.connected = false;
    settle(&p);
    assert(p.live_bitmap_count == 0);
    cui_destroy(p.ui);
}
static void picker_reply(Preview *p, AppDeviceKind kind, unsigned count) {
    p->app.device_request_requested = false;
    p->app.live.devices = (AppDevices){.kind = kind, .count = count,
                                      .generation = p->app.live.generation};
    for (unsigned i = 0; i < count; ++i) {
        snprintf(p->app.live.devices.choices[i].id, APP_DEVICE_ID_MAX, "device-%u", i);
        snprintf(p->app.live.devices.choices[i].label, 256, "Device %u", i);
    }
    settle(p);
}
static void picker_live(Preview *p) {
    p->app.live.enabled = p->app.live.connected = true;
    p->app.live.can_select_monitor = p->app.live.can_select_camera = true;
    p->app.live.generation = 42;
    p->app.live.camera_enabled = true;
    snprintf(p->app.live.screen_id, sizeof p->app.live.screen_id, "device-0");
    snprintf(p->app.live.camera_id, sizeof p->app.live.camera_id, "device-0");
    snprintf(p->app.live.screen_name, sizeof p->app.live.screen_name, "Device 0");
    snprintf(p->app.live.camera_name, sizeof p->app.live.camera_name, "Device 0");
    settle(p);
}
static void source_pickers(void) {
    Preview p = create(1360, 1000);
    click(&p, "source-choose");
    assert(!p.app.source_picker.popup.overlay.open); /* Fixture never invents devices. */
    picker_live(&p);
    assert(bounds(&p, "source-choose").h == 36);
    click(&p, "source-choose");
    assert(p.app.source_picker.popup.overlay.open && p.app.device_request_requested);
    assert(p.app.device_request_kind == APP_DEVICE_SCREEN);
    assert(bounds(&p, "source-picker-status").h == 32);
    picker_reply(&p, APP_DEVICE_SCREEN, 12);
    p.app.live.snapshot_busy = true;
    settle(&p);
    assert(p.app.source_picker.popup.overlay.open);
    assert(!app_request_edit(&p.app, APP_EDIT_LAYOUT, "split"));
    p.app.live.snapshot_busy = false;
    settle(&p);
    inside(bounds(&p, "source-picker-popup"), (cui_rect){0, 0, p.width, p.height});
    assert(bounds(&p, "source-picker-popup").h <= 266);
    assert(cui_focused(p.ui) == id(&p, "source-picker-screen-item-0"));
    key(&p, CUI_KEY_UP, 0);
    assert(cui_focused(p.ui) == id(&p, "source-picker-screen-item-0"));
    key(&p, CUI_KEY_END, 0);
    assert(cui_focused(p.ui) == id(&p, "source-picker-screen-item-11"));
    key(&p, CUI_KEY_DOWN, 0);
    assert(cui_focused(p.ui) == id(&p, "source-picker-screen-item-11"));
    assert(bounds(&p, "source-picker-screen-item-11").h == 32);
    key(&p, CUI_KEY_ENTER, 0);
    assert(!p.app.source_picker.popup.overlay.open && p.app.source_choice_requested);
    assert(p.app.source_choice_kind == APP_DEVICE_SCREEN);
    assert(!strcmp(p.app.source_choice_value, "device-11"));
    assert(!strcmp(p.app.live.screen_id, "device-0")); /* Selection waits for acknowledgement. */
    p.app.source_choice_requested = false;
    settle(&p);
    click(&p, "source-choose");
    p.app.device_request_requested = false;
    settle(&p);
    key(&p, CUI_KEY_ESCAPE, 0);
    assert(!p.app.source_picker.popup.overlay.open);
    assert(cui_focused(p.ui) == id(&p, "source-choose"));
    key(&p, CUI_KEY_DOWN, 0); /* Arrow opens a closed combo. */
    assert(p.app.source_picker.popup.overlay.open);
    p.app.device_request_requested = false;
    settle(&p);
    click(&p, "workspace-stream");
    assert(!p.app.source_picker.popup.overlay.open && p.app.workspace == APP_CAMERA);
    click(&p, "source-choose");
    picker_reply(&p, APP_DEVICE_SCREEN, 0);
    snprintf(p.app.live.devices.error, sizeof p.app.live.devices.error, "Discovery failed");
    settle(&p);
    click(&p, "source-picker-retry");
    assert(p.app.device_request_requested && p.app.source_picker.popup.overlay.open);
    p.app.live.devices.loading = true;
    p.app.device_request_requested = false;
    settle(&p);
    assert(bounds(&p, "source-picker-status").h == 32);
    picker_reply(&p, APP_DEVICE_SCREEN, 1);
    click(&p, "camera-choose"); /* Direct switch between the two reference combos. */
    assert(p.app.source_picker.popup.overlay.open && p.app.source_picker.kind == APP_DEVICE_CAMERA);
    assert(p.app.device_request_requested && p.app.device_request_kind == APP_DEVICE_CAMERA);
    picker_reply(&p, APP_DEVICE_CAMERA, 2);
    key(&p, CUI_KEY_HOME, 0);
    assert(cui_focused(p.ui) == id(&p, "source-picker-camera-none"));
    key(&p, CUI_KEY_ENTER, 0);
    assert(p.app.source_choice_requested && p.app.source_choice_kind == APP_DEVICE_CAMERA);
    assert(!p.app.source_choice_value[0] && p.app.live.camera_enabled);
    p.app.source_choice_requested = false;
    settle(&p);
    click(&p, "camera-choose");
    picker_reply(&p, APP_DEVICE_CAMERA, 0);
    assert(bounds(&p, "source-picker-camera-none").h == 32);
    assert(bounds(&p, "source-picker-status").h == 32);
    p.app.live.generation++;
    settle(&p);
    assert(!p.app.source_picker.popup.overlay.open);
    p.app.live.can_select_monitor = false;
    click(&p, "source-choose");
    assert(!p.app.source_picker.popup.overlay.open);
    p.app.live.can_reselect_portal = true;
    settle(&p);
    click(&p, "source-choose");
    key(&p, CUI_KEY_ENTER, 0);
    assert(p.app.source_choice_requested && p.app.source_choice_kind == APP_DEVICE_SCREEN);
    assert(!p.app.source_choice_value[0]);
    cui_destroy(p.ui);

    p = create(480, 820);
    picker_live(&p);
    click(&p, "toggle-sources");
    click(&p, "source-choose");
    picker_reply(&p, APP_DEVICE_SCREEN, 2);
    key(&p, CUI_KEY_ESCAPE, 0);
    assert(!p.app.source_picker.popup.overlay.open && p.app.panel_sheet.overlay.open);
    assert(cui_focused(p.ui) == id(&p, "source-choose"));
    key(&p, CUI_KEY_ESCAPE, 0);
    assert(!p.app.panel_sheet.overlay.open);
    cui_destroy(p.ui);
}
static void look_edits(void) {
    Preview p=create(1360,820);
    p.app.live.enabled=p.app.live.connected=true;
    p.app.live.zoom_min=1;p.app.live.zoom_max=2;p.app.live.zoom_factor=1;
    assert(!app_request_edit(&p.app,APP_EDIT_GRADIENT_FROM,"#nothex"));
    assert(!app_request_edit(&p.app,APP_EDIT_ZOOM_FACTOR,"3"));
    assert(app_request_edit(&p.app,APP_EDIT_SCREEN_BACKGROUND,"gradient"));
    assert(!strcmp(p.app.live.screen_background,"blurred"));
    assert(p.app.edit_requested && p.app.edit==APP_EDIT_SCREEN_BACKGROUND);
    assert(!app_request_edit(&p.app,APP_EDIT_ZOOM_FACTOR,"1.5"));
    p.app.edit_requested=false;p.app.live.pending=true;
    assert(!app_request_edit(&p.app,APP_EDIT_ZOOM_FACTOR,"1.5"));
    p.app.live.pending=false;p.app.live.connected=false;
    assert(!app_request_edit(&p.app,APP_EDIT_ZOOM_FACTOR,"1.5"));
    p.app.live.connected=true;
    p.app.source_sections[0]=p.app.source_sections[1]=false;p.app.source_sections[2]=true;
    snprintf(p.app.live.screen_background,16,"gradient");settle(&p);
    click(&p,"background-from");
    key(&p,CUI_KEY_A,CUI_MOD_CTRL);
    event(&p,(cui_event){.type=CUI_EVENT_TEXT,.text="#"});
    const cui_key digits[]={CUI_KEY_1,CUI_KEY_2,CUI_KEY_3};
    for(unsigned i=0;i<3;++i) {
        key(&p,digits[i],0);
        cui_event typed={.type=CUI_EVENT_TEXT};typed.text[0]=(char)('1'+i);
        event(&p,typed);
        assert(p.app.workspace==APP_CAMERA);
    }
    bool sources=p.app.show_sources,inspector=p.app.show_inspector;
    key(&p,CUI_KEY_LEFT_BRACKET,0);key(&p,CUI_KEY_RIGHT_BRACKET,0);
    assert(p.app.show_sources==sources&&p.app.show_inspector==inspector);
    event(&p,(cui_event){.type=CUI_EVENT_TEXT,.text="456"});
    key(&p,CUI_KEY_ENTER,0);
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_GRADIENT_FROM&&!strcmp(p.app.edit_value,"#123456"));
    p.app.edit_requested=false;settle(&p);
    click(&p,"stage-zoom");
    assert(p.app.zoom_popup.open);
    key(&p,CUI_KEY_ESCAPE,0);
    assert(!p.app.zoom_popup.open);
    cui_destroy(p.ui);
}
static void record_fixture(Preview *p, const char *mode) {
    p->app.workspace=APP_RECORD;p->app.workspace_tab=id(p,"workspace-record");
    p->app.live.enabled=p->app.live.connected=true;p->app.live.generation=1;
    p->app.live.width=640;p->app.live.height=480;p->app.live.fps=15;
    p->app.live.mic_gain=p->app.live.desktop_gain=100;p->app.live.can_select_audio=true;
    snprintf(p->app.live.backend,sizeof p->app.live.backend,"xorg");
    snprintf(p->app.live.record_container,sizeof p->app.live.record_container,"matroska");
    snprintf(p->app.live.record_codec,sizeof p->app.live.record_codec,"libx264");
    snprintf(p->app.live.record_audio_codec,sizeof p->app.live.record_audio_codec,"aac");
    snprintf(p->app.live.mic_name,sizeof p->app.live.mic_name,"Microphone off");
    snprintf(p->app.live.desktop_name,sizeof p->app.live.desktop_name,"Desktop audio off");
    p->app.live.record_crf=23;
    if(!strcmp(mode,"recording")||!strcmp(mode,"paused")||!strcmp(mode,"cut")) {
        p->app.live.recording=true;p->app.live.record_paused=!strcmp(mode,"paused");p->app.live.record_cut=!strcmp(mode,"cut");
        const char *path=getenv("CAST_TEST_RECORD_PATH");
        snprintf(p->app.live.record_path,sizeof p->app.live.record_path,"%s",path?path:"/tmp/cast-recording-test.mkv");
    }
    if(!strcmp(mode,"countdown")) {p->app.live.record_countdown_active=true;p->app.live.record_countdown_remaining_ns=UINT64_C(2000000000);}
    if(!strcmp(mode,"finalizing"))p->app.live.record_finalizing=true;
    settle(p);
    if(!strncmp(mode,"settings",8)) {
        app_open_settings(p->ui,&p->app,0);p->app.settings_category=2;settle(p);
        if(!strcmp(mode,"settings-container"))click(p,"record-container");
    }
}
static void audio_pickers(void) {
    Preview p=create(1360,1000);record_fixture(&p,"idle");
    click(&p,"audio-mic-source");
    assert(p.app.source_picker.popup.overlay.open&&p.app.device_request_requested);
    assert(p.app.device_request_kind==APP_DEVICE_MIC);picker_reply(&p,APP_DEVICE_MIC,2);
    key(&p,CUI_KEY_HOME,0);key(&p,CUI_KEY_ENTER,0);
    assert(p.app.source_choice_requested&&p.app.source_choice_kind==APP_DEVICE_MIC);
    assert(!strcmp(p.app.source_choice_value,"device-0"));
    assert(!p.app.live.mic&&!p.app.live.desktop&&!p.app.live.recording&&!p.app.live.stream_active);
    p.app.source_choice_requested=false;settle(&p);
    click(&p,"audio-desktop-source");
    assert(p.app.device_request_requested&&p.app.device_request_kind==APP_DEVICE_DESKTOP);
    picker_reply(&p,APP_DEVICE_DESKTOP,2);key(&p,CUI_KEY_HOME,0);key(&p,CUI_KEY_DOWN,0);key(&p,CUI_KEY_ENTER,0);
    assert(p.app.source_choice_requested&&p.app.source_choice_kind==APP_DEVICE_DESKTOP);
    assert(!strcmp(p.app.source_choice_value,"device-1"));
    assert(!p.app.live.mic&&!p.app.live.desktop&&!p.app.live.recording&&!p.app.live.stream_active);
    p.app.source_choice_requested=false;settle(&p);
    click(&p,"audio-mic-source");picker_reply(&p,APP_DEVICE_MIC,1);p.app.live.generation++;settle(&p);
    assert(!p.app.source_picker.popup.overlay.open&&!p.app.source_choice_requested);
    click(&p,"audio-desktop-source");picker_reply(&p,APP_DEVICE_DESKTOP,1);p.app.live.connected=false;settle(&p);
    assert(!p.app.source_picker.popup.overlay.open&&!p.app.source_choice_requested);cui_destroy(p.ui);
}
static void prompter_entitlement(void) {
    Preview p=create(1360,820);
    p.app.live.enabled=p.app.live.connected=p.app.live.pro=true;
    settle(&p);click(&p,"menu-prompter");assert(!p.app.edit_requested);
    key(&p,CUI_KEY_T,0);assert(!p.app.edit_requested);
    p.app.live.notes_available=true;settle(&p);click(&p,"menu-prompter");
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_NOTES_ACTION&&!strcmp(p.app.edit_value,"open"));
    assert(!p.app.live.notes_window_open); /* No optimistic daemon state. */
    p.app.edit_requested=false;p.app.live.notes_window_open=true;settle(&p);
    key(&p,CUI_KEY_T,0);
    assert(p.app.edit_requested&&!strcmp(p.app.edit_value,"close"));
    p.app.edit_requested=false;p.app.live.connected=false;settle(&p);
    key(&p,CUI_KEY_T,0);assert(!p.app.edit_requested);
    cui_destroy(p.ui);
}
static void recording_edits(void) {
    Preview p=create(1360,1000);record_fixture(&p,"idle");click(&p,"output-start");
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_RECORD_ACTION&&!strcmp(p.app.edit_value,"start"));
    assert(!p.app.live.recording);
    assert(!app_request_edit(&p.app,APP_EDIT_RECORD_ACTION,"start"));
    p.app.edit_requested=false;p.app.live.pending=true;settle(&p);click(&p,"output-start");assert(!p.app.edit_requested);
    p.app.live.pending=false;record_fixture(&p,"recording");click(&p,"record-pause");
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_RECORD_ACTION&&!strcmp(p.app.edit_value,"pause"));assert(!p.app.live.record_paused);
    p.app.edit_requested=false;record_fixture(&p,"paused");click(&p,"record-resume");
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_RECORD_ACTION&&!strcmp(p.app.edit_value,"resume"));
    p.app.edit_requested=false;p.app.live.record_paused=false;record_fixture(&p,"recording");click(&p,"record-cut");
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_RECORD_ACTION&&!strcmp(p.app.edit_value,"cut"));assert(!p.app.live.record_cut);
    p.app.edit_requested=false;record_fixture(&p,"cut");click(&p,"record-resume");assert(p.app.edit_requested&&!strcmp(p.app.edit_value,"resume"));
    p.app.edit_requested=false;p.app.live.record_cut=false;record_fixture(&p,"recording");click(&p,"output-start");
    assert(p.app.edit_requested&&!strcmp(p.app.edit_value,"stop"));
    p.app.edit_requested=false;p.app.live.recording=false;record_fixture(&p,"countdown");click(&p,"record-cancel");
    assert(p.app.edit_requested&&!strcmp(p.app.edit_value,"cancel"));
    p.app.edit_requested=false;p.app.live.record_countdown_active=false;record_fixture(&p,"finalizing");click(&p,"output-start");assert(!p.app.edit_requested);
    p.app.live.record_finalizing=false;p.app.live.connected=false;settle(&p);click(&p,"output-start");assert(!p.app.edit_requested);
    assert(!app_request_edit(&p.app,APP_EDIT_AUDIO_MIC_GAIN,"nan"));p.app.live.connected=true;
    assert(!app_request_edit(&p.app,APP_EDIT_RECORD_CONTAINER,"unsupported"));
    assert(!app_request_edit(&p.app,APP_EDIT_RECORD_CRF,"52"));assert(!app_request_edit(&p.app,APP_EDIT_RECORD_COUNTDOWN,"61"));
    record_fixture(&p,"settings");click(&p,"record-container");key(&p,CUI_KEY_HOME,0);key(&p,CUI_KEY_ENTER,0);
    assert(p.app.edit_requested&&p.app.edit==APP_EDIT_RECORD_CONTAINER&&!strcmp(p.app.edit_value,"mp4"));
    assert(!strcmp(p.app.live.record_container,"matroska"));
    p.app.edit_requested=false;p.app.live.recording=true;settle(&p);click(&p,"record-container");
    assert(!p.app.edit_requested&&!p.app.recording_selects[0].overlay.open);cui_destroy(p.ui);
}
int main(int argc,char **argv) {
#ifdef CAST_TEST_NATIVE_FONTS
    native_fonts_open();
#endif
    if(argc==3&&!strcmp(argv[1],"--record-layout")) {
        assert(!strcmp(argv[2],"idle")||!strcmp(argv[2],"recording")||!strcmp(argv[2],"paused")||
            !strcmp(argv[2],"cut")||!strcmp(argv[2],"countdown")||!strcmp(argv[2],"settings")||!strcmp(argv[2],"settings-container"));
        Preview p=create(1360,1000);record_fixture(&p,argv[2]);
        cui_set_layout_debug(p.ui,true);settle(&p);
        size_t required=0;cui_debug_layout_export(p.ui,CUI_DEBUG_LAYOUT_JSON,NULL,0,&required);
        char *dump=malloc(required);assert(dump);
        assert(cui_debug_layout_export(p.ui,CUI_DEBUG_LAYOUT_JSON,dump,required,&required));
        puts(dump);free(dump);cui_destroy(p.ui);return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--look-layout")) {
        assert(!strcmp(argv[2],"blurred")||!strcmp(argv[2],"gradient")||!strcmp(argv[2],"solid")||!strcmp(argv[2],"default")||!strcmp(argv[2],"camera")||!strcmp(argv[2],"zoom"));
        Preview p=create(1360,820);
        p.app.live.enabled=p.app.live.connected=true;
        p.app.live.generation=1;
        bool original=!strcmp(argv[2],"default"),camera=!strcmp(argv[2],"camera"),zoom=!strcmp(argv[2],"zoom");
        if(!original&&!camera&&!zoom)snprintf(p.app.live.screen_background,sizeof p.app.live.screen_background,"%s",argv[2]);
        if(!original)p.app.source_sections[0]=false;
        if(!original&&!camera)p.app.source_sections[1]=false;
        p.app.source_sections[2]=!original&&!camera;
        if(zoom) {settle(&p);click(&p,"stage-zoom");}
        cui_set_layout_debug(p.ui,true);settle(&p);
        size_t required=0;cui_debug_layout_export(p.ui,CUI_DEBUG_LAYOUT_JSON,NULL,0,&required);
        char *dump=malloc(required);assert(dump);
        assert(cui_debug_layout_export(p.ui,CUI_DEBUG_LAYOUT_JSON,dump,required,&required));
        puts(dump);free(dump);cui_destroy(p.ui);return 0;
    }
    prompter_entitlement();
    audio_pickers();
    recording_edits();
    look_edits();
    workspaces();
    compositions();
    panels();
    settings();
    settings_controls_fit();
    menus();
    responsive();
    source_pickers();
    live_composition_edits();
    live_preview_target_and_aspect();
    puts("Workspace preview: pointer, keyboard, composition, panels, modal settings and responsive bounds passed");
    return 0;
}
