/* SPDX-License-Identifier: MIT */
#include "app.h"
#include "brand.h"
#include "cui/cui_styled_slider_events.h"
#include <math.h>
#include <stdio.h>

/* Reference: cast-app .swin: 1020x720, 38px chrome, 214px rail.
 * Backend forms are deliberately disabled until an acknowledged snapshot exists. */
static const char *category[] = {"Appearance", "Composition", "Recording", "Streaming",
    "Teleprompter", "Presets", "Licence", "Agent access", "Advanced", "About"};
static const char *icons[] = {"sliders", "layers", "rec", "stream", "teleprompter",
    "star", "star", "external", "gear", "info"};
typedef struct { cui_icon_state state; bool initialized; } SettingsIcon;
static void icon_collect(void *p) { cui_icon_state_collect(&((SettingsIcon *)p)->state); }
static void icon_destroy(void *p) { cui_icon_state_destroy(&((SettingsIcon *)p)->state); }
static void settings_icon(cui_context *ui,cui_id id,const cui_theme *t,const char *name,cui_color color) {
    SettingsIcon *icon=cui_resource_get(ui,id,UINT64_C(0x73657474696e6769),sizeof *icon,icon_collect,icon_destroy);
    if(!icon)return;
    if(!icon->initialized) {
        cui_icon_state_init(&icon->state);
        char path[64];snprintf(path,sizeof path,"cast-%s",name);
        if(!cui_icon_set_path(&icon->state,cui_string(path),cast_brand_icon_loader,NULL))return;
        icon->initialized=true;
    }
    cui_icon(ui,id,t,&icon->state,(cui_icon_options){.size=CUI_ICON_CUSTOM,.custom_size=14,
        .custom_color=true,.color=color});
}
typedef struct { const cui_theme *theme; const char *name,*label; cui_color color; cui_id id; } RailContent;
static void rail_content(cui_context *ui,void *user) {
    RailContent *r=user;
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
    settings_icon(ui,r->id,r->theme,r->name,r->color);
    cui_begin_box(ui,0,(cui_box){.width=-1});
    if(r->label)cui_text(ui,cui_string(r->label),12.5f,r->color);
    cui_end_box(ui);cui_end_box(ui);
}
static void close_content(cui_context *ui, void *user) {
    RailContent *icon = user;
    settings_icon(ui, icon->id, icon->theme, "close", icon->color);
}
static void init(cui_context *ui, AppState *a) {
    if (!a->settings_initialized) {
        cui_surface_init(&a->settings, cui_id_from(ui, "settings-dialog"), CUI_SURFACE_DIALOG);
        a->settings_initialized = true;
    }
}
void app_open_settings(cui_context *ui, AppState *a, cui_id trigger) {
    init(ui, a);
    cui_surface_open(ui, &a->settings, trigger);
}
static void txt(cui_context *ui, const char *s, float size, cui_color color) {
    cui_text(ui, cui_string(s), size, color);
}
static void group(cui_context *ui, const cui_theme *t, const char *name) {
    cui_begin_box(ui, cui_id_from(ui, name), (cui_box){.width=-1, .max_width=720, .background=t->surface,
        .border=t->border, .border_width=1});
    cui_begin_box(ui, 0, (cui_box){.width=-1, .height=35, .custom_padding=true, .padding_edges={.left=12,.right=12,.top=9,.bottom=9},
        .border=t->border, .custom_border_edges=true,.border_edges={.bottom=1}});
    cui_text_ex(ui, cui_string(name), 11, t->muted, 0,
        (cui_text_style){.weight=CUI_TEXT_WEIGHT_SEMIBOLD});
    cui_end_box(ui);
    cui_begin_box(ui, 0, (cui_box){.width=-1, .custom_padding=true,.padding_edges={.left=12,.right=12}});
}
static void group_end(cui_context *ui) { cui_end_box(ui); cui_end_box(ui); }
static void row_begin(cui_context *ui, const cui_theme *t, const char *label, const char *note, bool narrow) {
    cui_push_id(ui, label);
    cui_begin_box(ui, cui_id_from(ui, "row"), (cui_box){.width=-1, .row=!narrow,
        .custom_padding=true,.padding_edges={.top=8,.bottom=8}, .gap=10, .align_y=CUI_ALIGN_CENTER,
        .border=cast_design_tokens(t->dark).hair,.custom_border_edges=true,.border_edges={.bottom=1}});
    cui_begin_box(ui, 0, (cui_box){.width=-1, .gap=2});
    txt(ui, label, 12.5f, t->text);
    if (note && *note) txt(ui, note, 11, t->muted);
    cui_end_box(ui);
}
static void row_end(cui_context *ui) { cui_end_box(ui); cui_pop_id(ui); }
static void disabled_button(cui_context *ui, const cui_theme *t, const char *value, bool combo) {
    cui_button_ex(ui, cui_id_from(ui,"value"), t, cui_string(value),
        (cui_button_options){.variant=CUI_BUTTON_OUTLINE, .size=CUI_CONTROL_SMALL,
            .height=28, .width=160, .font_size=11, .disabled=true,
            .trailing_icon=combo ? cui_string("chevron-down") : cui_string("")});
}
static void field(cui_context *ui, const cui_theme *t, const char *label, const char *note,
                  const char *value, char kind, bool narrow) {
    row_begin(ui,t,label,note,narrow);
    if (kind=='s') {
        bool v=false;
        cui_switch_ex(ui,cui_id_from(ui,"value"),t,cui_string(""),&v,NULL,
            (cui_switch_options){.accessibility_label=cui_string(label),.disabled=true,.size=CUI_CONTROL_SMALL,.width=28});
    } else if (kind=='r') {
        const float value_width = cui_measure_text(ui,cui_string(value),11).width;
        const float track_width = fmaxf(1,160-8-value_width);
        cui_begin_box(ui,0,(cui_box){.width=160,.fixed_width=true,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
        cui_id id=cui_id_from(ui,"value");
        cui_slider_state *state=cui_resource_get(ui,id,UINT64_C(0x736574736c696465),sizeof *state,NULL,NULL);
        if(state) cui_slider_events(ui,id,t,cui_string(label),0,state,
            (cui_slider_options){.minimum=0,.maximum=100,.step=1,.disabled=true,
                .value_description=cui_string("Unavailable")},
            (cui_slider_style_options){.width=track_width,.track_height=4,.hit_height=24,.hide_label=true},
            (cui_component_slider_handler){0});
        txt(ui,value,11,t->muted);cui_end_box(ui);
    } else if (kind=='t') cui_text_ex(ui,cui_string(value),11,t->muted,0,(cui_text_style){.family=1,.monospace=true});
    else disabled_button(ui,t,value,kind=='c');
    row_end(ui);
}
static void segments(cui_context *ui,const cui_theme *t,const char *label,const char *note,
                     const char *const *labels,unsigned count,int selected,bool enabled,
                     AppState *a,bool narrow) {
    cui_id appearance_ids[3]={cui_id_from(ui,"appearance-dark"),cui_id_from(ui,"appearance-light"),cui_id_from(ui,"appearance-system")};
    row_begin(ui,t,label,note,narrow);
    cui_begin_box(ui,0,(cui_box){.row=true,.gap=0,.height=30,.border=t->border,.border_width=1,.background=t->background});
    for(unsigned i=0;i<count;i++) {
        cui_id id=enabled?appearance_ids[i]:cui_id_from(ui,labels[i]);
        bool active=selected==(int)i;
        CastDesignTokens tokens=cast_design_tokens(t->dark);
        bool hovered=enabled && cui_hovered(ui,id);
        float item_width=enabled?(i==2?60:45):cui_measure_text(ui,cui_string(labels[i]),11.5f).width+20;
        cui_color fg=active||hovered?t->text:t->muted;
        if(!enabled)fg.a=102;
        cui_begin_box(ui,id,(cui_box){.width=item_width,.height=28,.align_x=CUI_ALIGN_CENTER,.align_y=CUI_ALIGN_CENTER,
            .background=hovered?tokens.hover:active?tokens.selected:(cui_color){0},
            .border=t->border,.custom_border_edges=true,.border_edges={.right=i+1<count?1:0}});
        bool toggled=active;
        bool clicked=cui_base_toggle(ui,id,cui_string(labels[i]),&toggled,!enabled);
        cui_update_semantic(ui,id,active,0);
        cui_text(ui,cui_string(labels[i]),11.5f,fg);
        if(active) {
            cui_color accent=t->primary;if(!enabled)accent.a=102;
            cui_begin_absolute_box(ui,id^UINT64_C(0x7365747365676c69),
                (cui_box){.background=accent},(cui_rect){0,26,item_width,2});cui_end_box(ui);
        }
        if(clicked) {
            AppAppearance values[]={APP_DARK,APP_LIGHT,APP_SYSTEM};
            a->appearance=values[i];cui_invalidate(ui);
        }
        cui_end_box(ui);
    }
    cui_end_box(ui); row_end(ui);
}
#define FIELD(label,note,value,kind) field(ui,t,label,note,value,kind,narrow)
#define SEG(label,note,values,sel) segments(ui,t,label,note,values,sizeof(values)/sizeof(values[0]),sel,false,a,narrow)
static void content(cui_context *ui,AppState *a,const cui_theme *t,bool narrow) {
    unsigned c=a->settings_category;
    const bool connected = a->live.connected;
    char fps[32]; snprintf(fps, sizeof fps, "%d fps", a->live.fps);
    const char *titles[]={"Appearance","Composition defaults","Recording","Streaming",
        "Teleprompter & subtitles","Presets","Licence","Agent access","Advanced","About"};
    const char *subs[]={
        "The interface stays quiet; the orange is reserved for what matters. These are preferences, not per-take settings.",
        "What a new session starts with. Per-take changes stay in the left panel.",
        "Durable defaults for takes and projects. The take itself stays in the Record workspace.",
        "One destination. The key is read from a file at start and never displayed.",
        "The prompter is drawn on top of the stage, never inside the outgoing frame.",
        "A preset is a saved look: layout, camera geometry, background, logo and text. It is never output state.",
        "Community keeps recording, streaming and the existing zoom, free and MIT-licensed.",
        "Local agent access uses the daemon. This preview cannot start a server or grant permissions.",
        "Rarely needed. Everything here is a fallback, not a preference.",
        "SuperScreencast — a native desktop app for composing a screen and a camera into one feed."};
    cui_text_ex(ui,cui_string(titles[c]),17,t->text,0,(cui_text_style){.weight=CUI_TEXT_WEIGHT_SEMIBOLD});
    cui_begin_box(ui,0,(cui_box){.width=-1,.max_width=480});
    txt(ui,subs[c],12,t->muted);cui_end_box(ui);
    if(c!=0 && c!=9) txt(ui,connected ? "Read only · changes are not available in this workspace" : "Connect the daemon to inspect current settings",11,t->muted);
    const char *theme[]={"dark","light","system"};
    const char *density[]={"comfortable","compact"};
    const char *fit[]={"contain","cover"},*shape[]={"rect","rounded","circle"};
    const char *quality[]={"Low","Balanced","Sharp"},*anchor[]={"camera","float"};
    switch(c) {
    case 0:
        group(ui,t,"THEME");
        segments(ui,t,"Colour scheme","System follows the desktop portal.",theme,3,
            a->appearance==APP_DARK?0:a->appearance==APP_LIGHT?1:2,true,a,narrow);
        SEG("Interface density","Compact suits tiled windows and small screens.",density,0);
        group_end(ui);
        group(ui,t,"WINDOW");
        FIELD("Use system title bar","Let the window manager draw the frame. Recommended on tiling WMs.","",'s');
        FIELD("Window controls","Placement and glyphs follow the chosen platform.","Linux",'c');
        group_end(ui);
        group(ui,t,"MOTION");
        FIELD("Reduce motion","Stops panel slides and meter animation.","",'s');
        FIELD("Panel transition",NULL,"Not implemented",'t'); group_end(ui);
        txt(ui,"Only colour scheme is available in this preview. Other preferences are not yet implemented.",11,t->muted);break;
    case 1:
        group(ui,t,"FRAME"); FIELD("Default layout",NULL,connected?a->live.layout:"Unavailable",'c');
        SEG("Screen fit",NULL,fit,-1); SEG("Camera shape",NULL,shape,-1);
        FIELD("Mirror camera",NULL,"",'s'); group_end(ui);
        group(ui,t,"CAPTURE"); FIELD("Screen border","A hairline around the captured screen.","",'s');
        FIELD("Corner radius","0 for a hard native rectangle.","— px",'r'); group_end(ui);
        group(ui,t,"ANNOTATIONS"); FIELD("Cursor, clicks and keys","Per-output visibility lives in the panel matrix.","",'s');group_end(ui);break;
    case 2:
        group(ui,t,"OUTPUT"); FIELD("Container",NULL,connected?a->live.record_container:"Unavailable",'c');
        FIELD("Encoder","Hardware encoders depend on the backend.",connected?a->live.record_codec:"Unavailable",'c');
        FIELD("Frame rate",NULL,connected?fps:"— fps",'c'); group_end(ui);
        group(ui,t,"TAKE"); FIELD("Countdown","Before every start and every resume.","— s",'r');
        FIELD("Continue the same file after a cut","Off writes a new file per segment.","",'s');group_end(ui);
        group(ui,t,"PROJECT");FIELD("Save folder",NULL,"Not connected",'b');
        FIELD("Autosave the cut list","Editable recording projects are not implemented.","",'s');group_end(ui);break;
    case 3:
        group(ui,t,"DESTINATION"); FIELD("Name",NULL,connected?a->live.stream_service:"Not connected",'b');
        FIELD("Server URL",NULL,connected?"Hidden for privacy":"Not connected",'b'); FIELD("Stream-key file",NULL,"Never displayed",'b');group_end(ui);
        group(ui,t,"ENCODING");SEG("Quality preset",NULL,quality,-1);
        FIELD("Bitrate",NULL,"— kbps",'r');FIELD("Keyframe interval",NULL,"— s",'r');group_end(ui);
        group(ui,t,"SAFETY");FIELD("Start paused","Not a preference. Revealing is always separate.","Required",'t');
        FIELD("Reconnect attempts","0 means never retry automatically.","—",'r');group_end(ui);break;
    case 4:
        txt(ui,"Pro · requires a supported Pro build and licence",11,t->primary);
        group(ui,t,"SCRIPT");FIELD("Text size",NULL,"— px",'r');FIELD("Scroll speed",NULL,"—",'r');
        FIELD("Mirror text","For beam-splitter glass in front of the lens.","",'s');group_end(ui);
        group(ui,t,"PLACEMENT · PRO");SEG("Default anchor","At camera keeps the reading line on the lens.",anchor,-1);group_end(ui);
        group(ui,t,"SUBTITLES · PRO");FIELD("Burn subtitles into recordings","Requires local inference support and an installed model.","",'s');
        FIELD("Model",NULL,"Not loaded",'c');group_end(ui);break;
    case 5:
        group(ui,t,"SAVED LOOKS");txt(ui,"Connect the daemon to read your saved presets.",12,t->muted);
        FIELD("Apply preset",NULL,"Apply",'b');FIELD("Delete preset",NULL,"Delete",'b');group_end(ui);
        disabled_button(ui,t,"Save current look…",false);break;
    case 6:
        group(ui,t,"PRO");txt(ui,"Cinematic zoom, teleprompter, subtitles.",17,t->text);
        txt(ui,"Pro features require a supported build and a valid licence. Licence status is not available in this preview.",12,t->muted);group_end(ui);
        group(ui,t,"THIS MACHINE");FIELD("Edition",NULL,connected?(a->live.pro?"Pro build":"Community"):"Not connected",'t');FIELD("Key",NULL,"—",'t');
        FIELD("Updates included until",NULL,"—",'t');group_end(ui);break;
    case 7:
        group(ui,t,"SERVER");FIELD("MCP server","Off by default. No endpoint exists while it is disabled.","",'s');
        FIELD("Start with the daemon","The server shares the daemon lifecycle. Closing the panel changes nothing.","",'s');
        FIELD("Transport","stdio is for an agent that launches the server itself.","Unavailable",'c');
        FIELD("Status",NULL,"Not connected",'t');group_end(ui);
        group(ui,t,"ENDPOINT");FIELD("Socket path","User-only permissions are required.","Unavailable",'b');
        FIELD("Copy connection string",NULL,"Copy",'b');group_end(ui);
        group(ui,t,"WHAT AGENTS MAY DO");FIELD("Start and stop outputs","Without this, agents are read-only: state, presets, layout.","",'s');
        FIELD("Reveal a paused output","Off keeps the last step of every privacy action human.","",'s');
        FIELD("Read the recording folder","Lets an agent see take names and durations.","",'s');group_end(ui);
        group(ui,t,"CONNECTED AGENTS");txt(ui,"Agent status is unavailable until daemon integration.",12,t->muted);group_end(ui);break;
    case 8:
        group(ui,t,"DAEMON");FIELD("Connection",NULL,connected?"Connected · read only":"Not connected",'t');FIELD("Binary",NULL,"Unavailable",'b');
        FIELD("Disconnect","The panel closes nothing; outputs keep running.","Disconnect",'b');group_end(ui);
        group(ui,t,"CAPTURE BACKEND");FIELD("Backend",NULL,connected?a->live.backend:"Unavailable",'c');FIELD("Log level",NULL,"Unavailable",'c');group_end(ui);
        group(ui,t,"DIAGNOSTICS");FIELD("Copy diagnostics","Versions, backends and recent logs. No content.","Copy",'b');
        FIELD("Open config file",NULL,"Open",'b');group_end(ui);break;
    default:
        txt(ui,"cast · native workspace preview",16,t->text);
#ifdef __APPLE__
        const char *platform = "macOS · SDL3";
#else
        const char *platform = "Linux · SDL3";
#endif
        group(ui,t,"BUILD");FIELD("Edition",NULL,"Public UI preview",'t');FIELD("Platform",NULL,platform,'t');
        FIELD("Licence of the source",NULL,"MIT",'t');group_end(ui);
        group(ui,t,"CREDITS");txt(ui,"SDL3, Clay and CUI for the interface; FFmpeg for media; Inter under the SIL Open Font License.",12,t->muted);group_end(ui);break;
    }
}
void app_settings_frame(cui_context *ui,AppState *a,const cui_theme *t,float width,float height) {
    init(ui,a);
    if(a->settings_category>=10)a->settings_category=0;
    float w=fminf(1020,fmaxf(0,width-40)),h=fminf(720,fmaxf(0,height-40));
    cui_surface_options o={.viewport={0,0,width,height},.width=w,.height=h,.margin=20,
        .no_outside_dismiss=true,.hide_close=true};
    if(!cui_base_surface_begin(ui,&a->settings,cui_string("Settings"),cui_string("Workspace preferences"),o))return;
    cui_rect bounds=cui_surface_geometry(CUI_SURFACE_DIALOG,o);
    cui_begin_portal(ui,cui_id_from(ui,"settings-backdrop"),(cui_box){.background={16,17,19,158}},o.viewport,100);
    cui_end_box(ui);
    cui_begin_portal(ui,a->settings.overlay.id,(cui_box){.background=t->background,.border=t->border,.border_width=1,.clip=true},bounds,101);
    cui_drop_shadow shadow={.offset_y=28,.sigma=36,.color={16,17,19,t->dark?184:51}};
    (void)cui_set_drop_shadows(ui,&shadow,1,(cui_corner_radii){0});
    cui_begin_box(ui,cui_surface_part_id(a->settings.overlay.id,4),(cui_box){.width=-1,.height=38,.row=true,
        .custom_padding=true,.padding_edges={.left=12,.right=12},.gap=10,.align_y=CUI_ALIGN_CENTER,.background=t->surface,
        .border=t->border,.custom_border_edges=true,.border_edges={.bottom=1}});
    settings_icon(ui,cui_id_from(ui,"settings-header-icon"),t,"gear",t->muted);
    cui_begin_box(ui,0,(cui_box){.width=-1});
    cui_text_ex(ui,cui_string("Settings"),13,t->text,0,(cui_text_style){.weight=CUI_TEXT_WEIGHT_SEMIBOLD});cui_end_box(ui);
    RailContent close_icon={t,"close",NULL,t->muted,cui_id_from(ui,"settings-close-art")};
    if(cui_button_ex(ui,cui_surface_part_id(a->settings.overlay.id,1),t,cui_string("Close settings"),
        (cui_button_options){.variant=CUI_BUTTON_GHOST,.height=26,.size=CUI_CONTROL_SMALL,.width=26,.icon_only=true,.content=close_content,.content_user=&close_icon}))
        cui_surface_close(ui,&a->settings,CUI_SURFACE_CLOSED);
    cui_end_box(ui);
    float rail=w<620?132:214,body_h=fmaxf(0,h-40),pane_w=fmaxf(0,w-rail-2);
    cui_begin_box(ui,0,(cui_box){.width=-1,.height=-1,.row=true});
    cui_scroll_options ro={.width=rail,.height=body_h,.content_width=rail,.content_height=346,
        .vertical=true,.hide_scrollbars=true};
    cui_id rid=cui_id_from(ui,"settings-rail");
    cui_begin_box(ui,0,(cui_box){.width=rail,.height=-1,.background=t->surface,
        .border=t->border,.custom_border_edges=true,.border_edges={.right=1}});
    if(cui_scrollable_begin(ui,rid,t,&a->settings_rail_scroll,ro)) {
        cui_begin_box(ui,0,(cui_box){.width=rail,.padding=8,.gap=1,.background=t->surface});
        for(unsigned i=0;i<10;i++) {
            char id[40];snprintf(id,sizeof(id),"settings-category-%u",i);
            bool selected=a->settings_category==i;
            cui_theme_button_colors colors={.normal=selected?t->selection:t->surface,.hover=t->secondary_hover,
                .active=t->selection,.foreground=selected?t->text:cast_design_tokens(t->dark).secondary,
                .border=selected?cast_design_tokens(t->dark).selected_line:t->surface};
            RailContent rc={t,icons[i],category[i],selected?t->text:cast_design_tokens(t->dark).secondary,cui_id_from(ui,id)^UINT64_C(0x617274)};
            if(cui_button_ex(ui,cui_id_from(ui,id),t,cui_string(category[i]),(cui_button_options){
                .variant=CUI_BUTTON_CUSTOM,.custom_colors=true,.colors=colors,.width=-1,.height=32,.font_size=12.5f,
                .content=rail_content,.content_user=&rc,.selected=selected,
                .toggle_semantics=true,.toggled=selected})) {
                a->settings_category=i;a->settings_scroll.y=0;cui_invalidate(ui);
            }
        }
        cui_end_box(ui);cui_scrollable_end_ex(ui,rid,t,&a->settings_rail_scroll,ro);
    }
    cui_end_box(ui);
    cui_scroll_options po={.width=pane_w,.height=body_h,.content_width=pane_w,
        .vertical=true,.auto_measure_content=true,.hide_scrollbars=true};
    cui_id pid=cui_id_from(ui,"settings-pane");
    cui_theme pane_theme=*t;pane_theme.surface=t->background;
    if(cui_scrollable_begin(ui,pid,&pane_theme,&a->settings_scroll,po)) {
        cui_begin_box(ui,0,(cui_box){.width=pane_w,.custom_padding=true,.padding_edges={.left=20,.right=20,.top=23,.bottom=18},.gap=12});
        content(ui,a,t,pane_w<430);
        cui_begin_box(ui,0,(cui_box){.height=22});cui_end_box(ui);
        cui_end_box(ui);cui_scrollable_end_ex(ui,pid,&pane_theme,&a->settings_scroll,po);
    }
    cui_end_box(ui);cui_end_box(ui);
    cui_overlay_set_bounds(&a->settings.overlay,bounds);
    cui_base_surface_end(ui);
}
