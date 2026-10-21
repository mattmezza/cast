/* SPDX-License-Identifier: MIT */
#include "app.h"
#include "brand.h"
#include "workspace_extension.h"
#include "cui/cui_styled_slider_events.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
static bool record_settings_disabled(const AppState *a) {
    return !a->live.connected||!a->live.enabled||a->edit_requested||a->live.pending||a->live.snapshot_busy||
        a->live.recording||a->live.record_countdown_active||a->live.record_finalizing;
}
static void record_select(cui_context *ui,AppState *a,const cui_theme *t,const char *idname,const char *label,
                          const char *note,const char *current,const char *const *labels,const char *const *values,
                          unsigned count,AppEdit edit,bool narrow) {
    cui_id id=cui_id_from(ui,idname);
    unsigned index=edit==APP_EDIT_RECORD_CONTAINER?0:edit==APP_EDIT_RECORD_VIDEO_CODEC?1:2;
    cui_choice_popup *popup=&a->recording_selects[index];
    if(!a->recording_select_initialized[index]) {
        cui_choice_popup_init(popup,id^UINT64_C(0x706f707570));a->recording_select_initialized[index]=true;
    }
    row_begin(ui,t,label,note,narrow);
    cui_choice_item items[8];cui_id selected=0;
    for(unsigned i=0;i<count;++i) {
        items[i]=(cui_choice_item){id^(UINT64_C(0x6f7074696f6e)+i),cui_string(labels[i]),false};
        if(!strcmp(values[i],current))selected=items[i].id;
    }
    cui_rect viewport=cui_bounds(ui,cui_id_from(ui,"root"));
    /* Root ID is outside this row's identity scope. The settings overlay bounds
     * supplies the same host viewport when nested IDs cannot resolve it. */
    if(viewport.w<=0)viewport=a->settings.overlay.bounds;
    bool disabled=record_settings_disabled(a),was_open=popup->overlay.open;
    cui_base_choice_popup_trigger(ui,id,cui_string(label),disabled,items,count,selected,popup);
    bool open=popup->overlay.open;
    CastDesignTokens tokens=cast_design_tokens(t->dark);
    cui_color foreground=disabled?t->muted:t->text;
    cui_begin_box(ui,id,(cui_box){.width=160,.fixed_width=true,.height=36,.row=true,.gap=8,
        .custom_padding=true,.padding_edges={.left=10,.right=10},.align_y=CUI_ALIGN_CENTER,
        .background=t->background,.border_width=1,.border=open?tokens.selected_line:cui_hovered(ui,id)?cui_color_mix_oklab(t->border,t->text,.45f):t->border});
    const char *display=current[0]?current:"Unavailable";
    for(unsigned i=0;i<count;++i)if(selected==items[i].id)display=labels[i];
    cui_text_ellipsis(ui,id^UINT64_C(0x76616c7565),cui_string(display),
        (cui_text_ellipsis_options){.width=-1,.font_size=12.5f,.color=foreground});
    settings_icon(ui,id^UINT64_C(0x63686576726f6e),t,"down",foreground);
    cui_end_box(ui);
    if(open&&cui_overlay_begin(ui,&popup->overlay)) {
        if(!was_open) {
            popup->list=(cui_list_state){.active=selected?selected:items[0].id};
            cui_focus(ui,popup->list.active);
        }
        cui_rect anchor=cui_bounds(ui,id);
        cui_rect panel=cui_popup_place(anchor,(cui_size){160,count*32.f+8},viewport,CUI_POPUP_BELOW,4).bounds;
        cui_overlay_set_bounds(&popup->overlay,panel);
        cui_begin_portal(ui,popup->overlay.id,(cui_box){.padding=4,.background=t->surface,.border=t->border,.border_width=1},panel,220);
        cui_box_shadow shadow={.layer={.offset_y=16,.sigma=20,.color={16,17,19,t->dark?184:51}}};
        cui_set_box_shadows(ui,&shadow,1,(cui_corner_radii){0});
        cui_theme theme=*t;theme.font_size=12;theme.popover=t->surface;
        cui_id chosen=selected;
        if(cui_list(ui,id^UINT64_C(0x6c697374),&theme,items,count,&chosen,&popup->list,
            (cui_list_options){.height=32,.dt=.016f,.scroll_scope=popup->overlay.id})) {
            for(unsigned i=0;i<count;++i)if(chosen==items[i].id)app_request_edit(a,edit,values[i]);
            cui_overlay_close(ui,&popup->overlay);cui_focus(ui,id);cui_invalidate(ui);
        }
        cui_end_box(ui);cui_overlay_end(ui);
    }
    row_end(ui);
}
typedef struct {AppState *app;AppEdit edit;} RecordingNumber;
static void record_number_request(void *user,const cui_component_slider_request *event) {
    if(event->phase!=CUI_SLIDER_COMMIT)return;
    RecordingNumber *request=user;char value[24];snprintf(value,sizeof value,"%.0f",event->requested);
    app_request_edit(request->app,request->edit,value);
}
static void record_number(cui_context *ui,AppState *a,const cui_theme *t,const char *idname,const char *label,
                          const char *note,int value,int maximum,AppEdit edit,bool narrow) {
    cui_id id=cui_id_from(ui,idname);row_begin(ui,t,label,note,narrow);
    cui_begin_box(ui,0,(cui_box){.width=160,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
    cui_slider_state *state=cui_resource_get(ui,id,UINT64_C(0x7265636e756d6265),sizeof *state,NULL,NULL);
    RecordingNumber request={a,edit};
    if(state)cui_slider_events(ui,id,t,cui_string(label),value,state,
        (cui_slider_options){.minimum=0,.maximum=maximum,.step=1,.disabled=record_settings_disabled(a)},
        (cui_slider_style_options){.width=120,.track_height=4,.hit_height=24,.hide_label=true},
        (cui_component_slider_handler){record_number_request,&request});
    char display[24];snprintf(display,sizeof display,"%d%s",value,edit==APP_EDIT_RECORD_COUNTDOWN?" s":"");txt(ui,display,11,t->muted);
    cui_end_box(ui);row_end(ui);
}
typedef struct {cui_edit *edit;cui_input_state state;char buffer[4096],acknowledged[4096];uint64_t generation;bool connected,focused;} RecordingDirectory;
static void directory_destroy(void *user) {cui_edit_destroy(((RecordingDirectory *)user)->edit);}
static void record_directory(cui_context *ui,AppState *a,const cui_theme *t,bool narrow) {
    cui_id id=cui_id_from(ui,"record-directory");
    RecordingDirectory *state=cui_resource_get(ui,id,UINT64_C(0x7265636469726563),sizeof *state,NULL,directory_destroy);
    if(!state)return;
    if(!state->edit) {
        snprintf(state->buffer,sizeof state->buffer,"%s",a->live.record_directory);
        state->edit=cui_edit_create((cui_edit_config){.buffer=state->buffer,.capacity=sizeof state->buffer});
    }
    if(!state->edit)return;
    bool focused=cui_focused(ui)==id;
    bool reset=strcmp(state->acknowledged,a->live.record_directory)||state->generation!=a->live.generation||
        state->connected!=a->live.connected||(state->focused&&!focused);
    size_t count=0;const cui_event *events=cui_frame_events(ui,&count);
    for(size_t i=0;i<count;++i)if(focused&&!cui_event_consumed(ui,i)&&events[i].type==CUI_EVENT_KEY_DOWN&&events[i].key==CUI_KEY_ESCAPE) {
        reset=true;cui_consume_event(ui,i);
    }
    if(reset)cui_edit_set_content(state->edit,(cui_edit_content){.text=cui_string(a->live.record_directory)});
    snprintf(state->acknowledged,sizeof state->acknowledged,"%s",a->live.record_directory);
    state->generation=a->live.generation;state->connected=a->live.connected;state->focused=focused;
    row_begin(ui,t,"Save folder","Press Enter to apply. The daemon chooses each take name.",narrow);
    cui_input_result result=cui_input_ex(ui,id,t,cui_string("Recording save folder"),state->edit,&state->state,
        (cui_input_options){.width=160,.height=36,.disabled=record_settings_disabled(a)});
    if(result.submitted) {
        app_request_edit(a,APP_EDIT_RECORD_DIRECTORY,state->buffer);
        cui_edit_set_content(state->edit,(cui_edit_content){.text=cui_string(a->live.record_directory)});cui_invalidate(ui);
    }
    row_end(ui);
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
    if(c!=0 && c!=2 && c!=4 && c!=6 && c!=9) txt(ui,connected ? "Read only · changes are not available in this workspace" : "Connect the daemon to inspect current settings",11,t->muted);
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
    case 2: {
        const char *containers[]={"MP4","MKV","MOV","WebM"},*container_values[]={"mp4","matroska","mov","webm"};
        const char *encoders[]={"Automatic","H.264 · software","HEVC · software","VP9 · software","AV1 · software"},*encoder_values[]={"auto","libx264","libx265","libvpx-vp9","libaom-av1"};
        const char *audio[]={"AAC","Opus","PCM 16-bit","FLAC"},*audio_values[]={"aac","libopus","pcm_s16le","flac"};
        txt(ui,record_settings_disabled(a)?"Recording settings are locked while a take is active or the daemon is unavailable.":"Changes apply to the next take in this daemon session.",11,t->muted);
        if(a->live.edit_error[0])txt(ui,a->live.edit_error,11,t->danger);
        group(ui,t,"OUTPUT");
        record_select(ui,a,t,"record-container","Container",NULL,a->live.record_container,containers,container_values,4,APP_EDIT_RECORD_CONTAINER,narrow);
        record_select(ui,a,t,"record-video-codec","Encoder","Available encoders depend on the backend.",a->live.record_codec,encoders,encoder_values,5,APP_EDIT_RECORD_VIDEO_CODEC,narrow);
        record_select(ui,a,t,"record-audio-codec","Audio codec",NULL,a->live.record_audio_codec,audio,audio_values,4,APP_EDIT_RECORD_AUDIO_CODEC,narrow);
        record_number(ui,a,t,"record-crf","Quality (CRF)","Lower values improve quality and increase file size.",a->live.record_crf,51,APP_EDIT_RECORD_CRF,narrow);
        FIELD("Frame rate","Requires restarting the capture daemon.",connected?fps:"— fps",'t');group_end(ui);
        group(ui,t,"TAKE");record_number(ui,a,t,"record-countdown","Countdown","Before each start and resume from a cut.",a->live.record_countdown,60,APP_EDIT_RECORD_COUNTDOWN,narrow);
        FIELD("Continue the same file after a cut","Cuts omit media time; resume continues the same file.","Always",'t');group_end(ui);
        group(ui,t,"PROJECT");record_directory(ui,a,t,narrow);
        FIELD("Autosave the cut list","Editable recording projects are unavailable.","Unavailable",'t');group_end(ui);break;
    }
    case 3:
        group(ui,t,"DESTINATION"); FIELD("Name",NULL,connected?a->live.stream_service:"Not connected",'b');
        FIELD("Server URL",NULL,connected?"Hidden for privacy":"Not connected",'b'); FIELD("Stream-key file",NULL,"Never displayed",'b');group_end(ui);
        group(ui,t,"ENCODING");SEG("Quality preset",NULL,quality,-1);
        FIELD("Bitrate",NULL,"— kbps",'r');FIELD("Keyframe interval",NULL,"— s",'r');group_end(ui);
        group(ui,t,"SAFETY");FIELD("Start paused","Not a preference. Revealing is always separate.","Required",'t');
        FIELD("Reconnect attempts","0 means never retry automatically.","—",'r');group_end(ui);break;
    case 4:
#if defined(WITH_PRO) && defined(WITH_CUI)
        if(cast_workspace_settings(ui,a,t,4,narrow))break;
#endif
        txt(ui,a->live.notes_available?"Available · entitled by the connected daemon":"Requires a connected daemon with a teleprompter entitlement",11,t->primary);
        group(ui,t,"PROMPTER");
        if(cui_button_ex(ui,cui_id_from(ui,"settings-prompter-open"),t,
            cui_string(a->live.notes_window_open?"Show controls in Prompter":"Open Prompter"),
            (cui_button_options){.variant=CUI_BUTTON_OUTLINE,.height=28,.font_size=11,
                .disabled=!connected||!a->live.notes_available||a->live.pending}))
            app_request_edit(a,APP_EDIT_NOTES_ACTION,"open");
        txt(ui,"Use More in the Prompter window for script, mode and playback controls. Workspace preferences below are not implemented yet.",11,t->muted);
        group_end(ui);
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
#if defined(WITH_PRO) && defined(WITH_CUI)
        if(cast_workspace_settings(ui,a,t,6,narrow))break;
#endif
        group(ui,t,"PRO");txt(ui,"Cinematic zoom, teleprompter, subtitles.",17,t->text);
        txt(ui,"Licence status comes from the connected daemon. Features also depend on build and platform support.",12,t->muted);group_end(ui);
        group(ui,t,"THIS MACHINE");FIELD("Edition",NULL,connected?(a->live.pro?"Pro build":"Community"):"Not connected",'t');FIELD("Licence",NULL,connected?(a->live.license_valid?"Valid":"Not valid"):"Not connected",'t');FIELD("Key",NULL,connected&&a->live.license_id[0]?a->live.license_id:"—",'t');
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
#if defined(WITH_PRO) && defined(WITH_CUI)
    cast_workspace_settings_sync(ui,a);
#endif
    if(!a->settings.overlay.open||!a->live.connected||a->recording_select_generation!=a->live.generation||a->settings_category!=2)
        for(unsigned i=0;i<3;++i)if(a->recording_select_initialized[i])cui_overlay_close(ui,&a->recording_selects[i].overlay);
    a->recording_select_generation=a->live.generation;
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
