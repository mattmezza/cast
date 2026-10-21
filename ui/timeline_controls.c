#include "timeline_controls.h"
#include "brand.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cui_edit *edit;
    cui_input_state input;
    char buffer[4096], acknowledged[4096];
    bool disabled, focused;
} TimelineField;
typedef struct {
    uint64_t selected, revision, generation, cancelled_drag;
    cui_scroll_state scroll, pan;
    float zoom;
    struct {uint64_t start,span;float idle;bool initialized,dirty;} window;
    struct {uint64_t id, revision, generation; float x,y; unsigned mode; bool moved;} drag;
} TimelineView;
typedef struct { float fraction; cui_color color; } Playhead;
static bool paint_playhead(const cui_prepaint_info *info, const void *payload, cui_prepaint_writer *writer)
{
    const Playhead *p = payload;
    if (!info->modal_allowed || info->inert || info->visible_bounds.w <= 0 || info->visible_bounds.h <= 0)
        return true;
    float x = info->bounds.x + p->fraction * info->bounds.w;
    if (x < info->visible_bounds.x || x >= info->visible_bounds.x + info->visible_bounds.w)
        return true;
    cui_draw_command line = {.type = CUI_DRAW_RECT, .color = p->color,
        .bounds = {x, info->visible_bounds.y, 1, info->visible_bounds.h}};
    return cui_prepaint_emit(writer, &line, true);
}
typedef struct {unsigned bins;float peak[CAST_PROJECT_ASSET_BINS];cui_color color;} WavePaint;
static bool paint_wave(const cui_prepaint_info *info,const void *payload,cui_prepaint_writer *writer)
{
    const WavePaint *wave=payload;
    if(info->inert||!info->modal_allowed||!wave->bins||info->visible_bounds.w<=0)return true;
    unsigned bars=wave->bins<64?wave->bins:64;
    for(unsigned i=0;i<bars;++i) {
        float peak=0;
        for(unsigned bin=i*wave->bins/bars;bin<(i+1)*wave->bins/bars;++bin)
            if(isfinite(wave->peak[bin])&&wave->peak[bin]>peak)peak=wave->peak[bin];
        if(peak<=0)continue;
        if(peak>1)peak=1;
        float height=peak*info->bounds.h;
        cui_draw_command bar={.type=CUI_DRAW_RECT,.color=wave->color,
            .bounds={info->bounds.x+(float)i/bars*info->bounds.w,
                info->bounds.y+(info->bounds.h-height)/2,fmaxf(1,info->bounds.w/bars),height}};
        if(!cui_prepaint_emit(writer,&bar,true))return false;
    }
    return true;
}
static const CastProjectAssetView *clip_asset(const CastProjectSnapshot *p,const CastTimelineClip *clip)
{
    for(unsigned i=0;i<p->asset_count&&i<CAST_PROJECT_ASSET_MAX;++i) {
        const CastProjectAssetView *a=&p->assets[i];
        if(a->clip_id==clip->id&&a->media_id==clip->media_id&&a->revision==p->revision&&
            a->source_start_ns>=clip->source_in_ns&&a->source_start_ns-clip->source_in_ns<clip->duration_ns&&
            a->span_ns<=clip->duration_ns-(a->source_start_ns-clip->source_in_ns)&&a->generation&&!a->missing)return a;
    }
    return NULL;
}
static void destroy_field(void *user) { cui_edit_destroy(((TimelineField *)user)->edit); }
static bool send(AppTimelineSubmit submit, void *user, const char *domain, const char *action,
                  const char *a, const char *b)
{
    const char *argv[] = {domain, action, a, b};
    return submit && submit(user, b ? 4 : a ? 3 : 2, argv);
}
static void timecode(char *out, size_t n, uint64_t ns)
{
    uint64_t ms = ns / 1000000;
    snprintf(out, n, "%02llu:%02llu.%03llu", (unsigned long long)(ms / 60000),
             (unsigned long long)(ms / 1000 % 60), (unsigned long long)(ms % 1000));
}
static const char *field_mode(cui_context *ui, const cui_theme *t, const char *name,
                         const char *label, const char *ack, float width, bool disabled,
                         bool *submitted, bool draft)
{
    cui_id id = cui_id_from(ui, name);
    TimelineField *s = cui_resource_get(ui, id, UINT64_C(0x74696d656669656c), sizeof *s,
                                      NULL, destroy_field);
    if (!s) return "";
    if (!s->edit) {
        snprintf(s->buffer, sizeof s->buffer, "%s", ack);
        s->edit = cui_edit_create((cui_edit_config){.buffer = s->buffer, .capacity = sizeof s->buffer});
        if (!s->edit) return "";
    }
    bool focused = cui_focused(ui) == id;
    if ((strcmp(s->acknowledged, ack) && !focused) || (disabled && !s->disabled) || (!draft && s->focused && !focused))
        cui_edit_set_content(s->edit, (cui_edit_content){.text = cui_string(ack)});
    snprintf(s->acknowledged, sizeof s->acknowledged, "%s", ack);
    s->disabled = disabled; s->focused = focused;
    cui_input_result result = cui_input_ex(ui, id, t, cui_string(label), s->edit, &s->input,
        (cui_input_options){.width = width, .height = 28, .disabled = disabled});
    if (submitted) *submitted = result.submitted;
    return s->buffer;
}
static const char *field(cui_context *ui,const cui_theme *t,const char *name,
    const char *label,const char *ack,float width,bool disabled,bool *submitted)
{return field_mode(ui,t,name,label,ack,width,disabled,submitted,false);}
static const CastTimelineClip *selected(const CastProjectSnapshot *, uint64_t);
static bool seconds_value(const char *text,uint64_t *ns) {
    char *end;double value=strtod(text,&end);
    if(!text[0] || *end || !isfinite(value) || value<0 || value>604800)return false;
    *ns=(uint64_t)llround(value*1000000000.0);return true;
}
static bool button(cui_context *ui, const cui_theme *t, const char *id, const char *label,
                    const char *icon, bool disabled)
{
    return cui_button_ex(ui, cui_id_from(ui, id), t, cui_string(label),
        (cui_button_options){.height = 28, .font_size = 11, .variant = CUI_BUTTON_GHOST,
            .icon = cui_string(icon), .disabled = disabled});
}
typedef struct {uint64_t selected; bool range; cui_scroll_state scroll;} MediaBinView;
static MediaBinView *media_bin(cui_context *ui,const cui_theme *t,const CastProjectSnapshot *p,
                               bool blocked,bool privacy,const CastLiveMediaStatus *live,AppTimelineSubmit submit,void *user)
{
    MediaBinView *view=cui_resource_get(ui,cui_id_from(ui,"media-bin-state"),
        UINT64_C(0x6d6564696162696e),sizeof *view,NULL,NULL);
    if(!view)return NULL;
    const float bin_width=210;
    bool present=false;
    for(unsigned i=0;i<p->media_count;++i)if(p->media[i].id==view->selected)present=true;
    if(!present)view->selected=0;
    cui_begin_box(ui,cui_id_from(ui,"media-bin"),(cui_box){.width=-1,.gap=6,
        .border=cast_design_tokens(t->dark).hair,.custom_border_edges=true,.border_edges={.top=1},
        .custom_padding=true,.padding_edges={.top=10,.bottom=4}});
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.align_y=CUI_ALIGN_CENTER,.gap=6});
    cui_text_ex(ui,cui_string("MEDIA BIN"),10,t->muted,0,(cui_text_style){.weight=CUI_TEXT_WEIGHT_SEMIBOLD});
    if(button(ui,t,"media-bin-import","Add source…","cast-folder",blocked||p->busy))
        send(submit,user,"file","source",NULL,NULL);
    cui_end_box(ui);
    if(!p->media_count)cui_text(ui,cui_string("Add video or audio sources.\nSources stay outside edit clips."),11,t->muted);
    else {
        cui_id scroll_id=cui_id_from(ui,"media-bin-list");
        cui_scroll_options options={.width=-1,.height=fminf(128,p->media_count*32.f),
            .content_height=p->media_count*32.f,.vertical=true};
        if(cui_scrollable_begin(ui,scroll_id,t,&view->scroll,options)) {
            for(unsigned i=0;i<p->media_count&&i<CAST_TIMELINE_MEDIA_MAX;++i) {
                const CastProjectMediaView *m=&p->media[i];
                char name[48],time[32];snprintf(name,sizeof name,"media-bin-source-%llu",(unsigned long long)m->id);
                timecode(time,sizeof time,m->duration_ns);
                cui_id id=cui_id_from(ui,name);
                cui_interaction item=cui_base_button(ui,id,cui_string(m->label),blocked||p->busy);
                cui_update_semantic(ui,id,view->selected==m->id,0);
                cui_begin_box(ui,id,(cui_box){.width=-1,.height=32,.fixed_height=true,.row=true,.gap=6,
                    .custom_padding=true,.padding_edges={.left=6,.right=6},.align_y=CUI_ALIGN_CENTER,
                    .background=view->selected==m->id?cast_design_tokens(t->dark).selected:
                        item.hovered?cast_design_tokens(t->dark).hover:t->surface,
                    .border=cast_design_tokens(t->dark).hair,.custom_border_edges=true,.border_edges={.bottom=1}});
                cui_text(ui,cui_string(m->kind==CAST_TIMELINE_AUDIO?"A":m->kind==CAST_TIMELINE_IMAGE?"I":"V"),10,t->muted);
                cui_text_ellipsis(ui,id^UINT64_C(0x6c6162656c),cui_string(m->label),
                    (cui_text_ellipsis_options){.width=100,.font_size=11,.color=m->missing?t->danger:t->text});
                cui_text_ex(ui,cui_string(m->missing?"Missing":time),9,t->muted,0,(cui_text_style){.family=1,.monospace=true});
                cui_end_box(ui);
                if(item.activated){view->selected=m->id;cui_invalidate(ui);}
            }
            cui_scrollable_end_ex(ui,scroll_id,t,&view->scroll,options);
        }
    }
    const CastProjectMediaView *selected_source=NULL;
    for(unsigned i=0;i<p->media_count;++i)if(p->media[i].id==view->selected)selected_source=&p->media[i];
    bool starting=live->state==CAST_LIVE_STARTING;
    bool active=starting||live->state==CAST_LIVE_PLAYING||live->state==CAST_LIVE_PAUSED;
    bool valid_range=true;char range[80]={0};
    if(selected_source) {
        char scope[32];snprintf(scope,sizeof scope,"%llu",(unsigned long long)selected_source->id);cui_push_id(ui,scope);
        cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=8,.align_y=CUI_ALIGN_CENTER});
        cui_text(ui,cui_string("Use source range"),11,t->muted);
        cui_switch_ex(ui,cui_id_from(ui,"media-bin-range"),t,cui_string(""),&view->range,NULL,
            (cui_switch_options){.size=CUI_CONTROL_SMALL,.width=28,.disabled=blocked||starting,
                .accessibility_label=cui_string("Use source range")});
        cui_end_box(ui);
        if(view->range) {
            char duration[64];snprintf(duration,sizeof duration,"%.3f",(double)selected_source->duration_ns/1e9);
            bool submitted=false;
            cui_text(ui,cui_string("In · seconds"),11,t->muted);
            const char *in=field_mode(ui,t,"media-bin-source-in","Source in seconds","0.000",-1,blocked||starting,&submitted,true);
            uint64_t in_ns=0,duration_ns=0;
            valid_range=seconds_value(in,&in_ns);
            cui_text(ui,cui_string("Duration · seconds"),11,t->muted);
            const char *length=field_mode(ui,t,"media-bin-duration","Source duration seconds",duration,-1,blocked||starting,&submitted,true);
            valid_range=seconds_value(length,&duration_ns)&&duration_ns>0&&valid_range&&
                (!selected_source->duration_ns||(in_ns<=selected_source->duration_ns&&duration_ns<=selected_source->duration_ns-in_ns));
            snprintf(range,sizeof range,"%llu,%llu",(unsigned long long)in_ns,(unsigned long long)duration_ns);
            if(!valid_range)cui_text(ui,cui_string("Choose a positive range\ninside the source."),11,t->danger);
        }
        cui_pop_id(ui);
    }
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=4});
    if(button(ui,t,"media-bin-play","Play","",blocked||privacy||starting||!selected_source||selected_source->missing||!valid_range)) {
        char id[32];snprintf(id,sizeof id,"%llu",(unsigned long long)selected_source->id);
        send(submit,user,"media","play",id,view->range?range:NULL);
    }
    if(live->state==CAST_LIVE_PAUSED) {
        if(button(ui,t,"media-bin-resume","Resume","",blocked||privacy))send(submit,user,"media","resume",NULL,NULL);
    } else if(button(ui,t,"media-bin-pause","Pause","",blocked||live->state!=CAST_LIVE_PLAYING))send(submit,user,"media","pause",NULL,NULL);
    if(button(ui,t,"media-bin-stop","Stop","",blocked||!active))send(submit,user,"media","stop",NULL,NULL);
    cui_end_box(ui);
    const char *state=live->state==CAST_LIVE_STARTING?"Starting":live->state==CAST_LIVE_PLAYING?"Playing":
        live->state==CAST_LIVE_PAUSED?"Paused":live->state==CAST_LIVE_ENDED?"Ended":live->state==CAST_LIVE_FAILED?"Failed":"Off";
    char position[32],duration[32],status[96];timecode(position,sizeof position,live->position_ns);timecode(duration,sizeof duration,live->duration_ns);
    snprintf(status,sizeof status,"%s · %s / %s",state,position,duration);
    cui_text(ui,cui_string(status),10,t->muted);
    if(live->error[0])cui_text_ellipsis(ui,cui_id_from(ui,"media-bin-error"),cui_string(live->error),
        (cui_text_ellipsis_options){.width=fmaxf(100,bin_width),.font_size=11,.color=t->danger});
    if(privacy)cui_text(ui,cui_string("Global privacy blocks\nPlay and Resume."),11,t->muted);
    cui_text(ui,cui_string("Video replaces the full stage.\nStop or End restores the stage.\nSource audio joins the mix.\nVirtual audio uses its own switch."),10,t->muted);
    cui_end_box(ui);return view;
}
void app_project_controls(cui_context *ui, const cui_theme *t, const CastProjectSnapshot *p,
                          bool blocked, bool privacy, const CastLiveMediaStatus *live, AppTimelineSubmit submit, void *user)
{
    cui_begin_box(ui, cui_id_from(ui, "project-controls"), (cui_box){.width = -1, .gap = 8});
    cui_text_ex(ui, cui_string(p->open ? p->name : "No project open"), 13, t->text, 0,
                (cui_text_style){.weight = CUI_TEXT_WEIGHT_SEMIBOLD});
    char detail[96], duration[32]; timecode(duration, sizeof duration, p->duration_ns);
    snprintf(detail, sizeof detail, "%s · %u clips · %s", duration, p->clip_count,
             p->busy ? "working" : p->dirty ? "unsaved" : "saved");
    cui_text(ui, cui_string(detail), 11, t->muted);
    bool submitted = false;
    const char *path = field(ui, t, "project-file", "Project path", p->path, -1, blocked || p->busy, &submitted);
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 6});
    if (button(ui, t, "project-open", "Open", "cast-folder", blocked || p->busy || !path[0]) || submitted)
        send(submit, user, "project", "open", path, NULL);
    if (button(ui, t, "project-save", "Save", "cast-save", blocked || p->busy || !p->open || !path[0]))
        send(submit, user, "project", "save", path, NULL);
    if (button(ui, t, "project-new", "New", "file", blocked || p->busy || p->recording))
        send(submit, user, "project", "new", NULL, NULL);
    cui_end_box(ui);
    cui_begin_box(ui, 0, (cui_box){.width = -1, .row = true, .gap = 6});
    if(button(ui,t,"project-browse","Browse…","cast-folder",blocked || p->busy || p->recording))send(submit,user,"file","open",NULL,NULL);
    if(button(ui,t,"project-save-as","Save as…","cast-save",blocked || p->busy || !p->open))send(submit,user,"file","save",NULL,NULL);
    cui_end_box(ui);
    media_bin(ui,t,p,blocked,privacy,live,submit,user);
    if(p->missing_count) {
        cui_text(ui,cui_string("Missing media"),12,t->danger);
        for(unsigned i=0;i<p->media_count && i<CAST_TIMELINE_MEDIA_MAX;++i)if(p->media[i].missing) {
            char media_id[32],button_id[64];snprintf(media_id,sizeof media_id,"%llu",(unsigned long long)p->media[i].id);
            snprintf(button_id,sizeof button_id,"project-relink-%s",media_id);
            cui_text(ui,cui_string(p->media[i].label),11,t->muted);
            if(button(ui,t,button_id,"Locate file…","cast-folder",blocked || p->busy || p->recording))
                send(submit,user,"file","relink",media_id,NULL);
        }
    }
    TimelineView *view=cui_resource_find(ui,cui_id_from(ui,"timeline-view-state"),UINT64_C(0x74696d656c696e65));
    const CastTimelineClip *clip=view?selected(p,view->selected):NULL;
    if(clip) {
        char id[32],ack[80],value[96];snprintf(id,sizeof id,"%llu",(unsigned long long)clip->id);
        bool locked=blocked || p->busy || clip->capture_locked,edit=false;
        cui_push_id(ui,id);
        cui_text(ui,cui_string(clip->capture_locked?"Recording clip · locked":"Selected clip"),12,t->text);
        cui_text(ui,cui_string("Start on timeline (seconds)"),11,t->muted);
        snprintf(ack,sizeof ack,"%.3f",(double)clip->start_ns/1e9);
        const char *start=field(ui,t,"clip-start","Start in seconds",ack,-1,locked,&edit);
        bool submitted_start=edit;edit=false;
        cui_text(ui,cui_string("Track (0–15)"),11,t->muted);
        snprintf(ack,sizeof ack,"%u",clip->track);
        const char *track=field(ui,t,"clip-track","Track",ack,-1,locked,&edit);
        if(edit || submitted_start) {
            char *end;unsigned long number=strtoul(track,&end,10);uint64_t ns;
            if(track[0] && !*end && number<16 && seconds_value(start,&ns)) {
                snprintf(value,sizeof value,"%lu,%llu",number,(unsigned long long)ns);send(submit,user,"timeline","move",id,value);
            }
        }
        cui_text(ui,cui_string("Source in (seconds)"),11,t->muted);edit=false;
        snprintf(ack,sizeof ack,"%.3f",(double)clip->source_in_ns/1e9);
        const char *source=field(ui,t,"clip-source-in","Source in seconds",ack,-1,locked,&edit);
        bool submitted_source=edit;edit=false;
        cui_text(ui,cui_string("Duration (seconds)"),11,t->muted);
        snprintf(ack,sizeof ack,"%.3f",(double)clip->duration_ns/1e9);
        const char *duration=field(ui,t,"clip-duration","Duration in seconds",ack,-1,locked,&edit);
        if(edit || submitted_source) {
            uint64_t in,length;if(seconds_value(source,&in) && seconds_value(duration,&length) && length) {
                snprintf(value,sizeof value,"%llu,%llu",(unsigned long long)in,(unsigned long long)length);send(submit,user,"timeline","trim",id,value);
            }
        }
        cui_text(ui,cui_string("Audio gain (0–4)"),11,t->muted);edit=false;
        snprintf(ack,sizeof ack,"%.3f",clip->gain);
        const char *gain=field(ui,t,"clip-gain","Audio gain",ack,-1,locked,&edit);
        if(edit) {
            char *end;double level=strtod(gain,&end);
            if(gain[0] && !*end && isfinite(level) && level>=0 && level<=4) {
                snprintf(value,sizeof value,"%.4f,%d",level,clip->muted);send(submit,user,"timeline","gain",id,value);
            }
        }
        cui_pop_id(ui);
    }
    if (p->error[0]) cui_text(ui, cui_string(p->error), 11, t->danger);
    else if(p->preview_error[0])cui_text(ui,cui_string(p->preview_error),11,t->danger);
    else if (p->status[0]) cui_text(ui, cui_string(p->status), 11, t->muted);
    cui_end_box(ui);
}
static const CastTimelineClip *selected(const CastProjectSnapshot *p, uint64_t id)
{
    for (unsigned i = 0; i < p->clip_count; ++i) if (p->clips[i].id == id) return &p->clips[i];
    return NULL;
}
static const CastProjectMediaView *media(const CastProjectSnapshot *p, uint64_t id)
{
    for (unsigned i = 0; i < p->media_count; ++i) if (p->media[i].id == id) return &p->media[i];
    return NULL;
}
static bool ruler_button(cui_context *ui,const cui_theme *t,const char *name,const char *label,
                         float width,bool disabled,bool mono)
{
    cui_id id=cui_id_from(ui,name);
    cui_interaction interaction=cui_base_button(ui,id,cui_string(label),disabled);
    cui_begin_box(ui,id,(cui_box){.width=width,.fixed_width=true,.height=22,.fixed_height=true,
        .row=true,.align_x=CUI_ALIGN_CENTER,.align_y=CUI_ALIGN_CENTER,
        .background=interaction.hovered&&!disabled?cast_design_tokens(t->dark).control:t->surface,
        .border=interaction.focus_visible?t->primary:cast_design_tokens(t->dark).hair,
        .custom_border_edges=true,.border_edges={.left=1}});
    cui_text_ex(ui,cui_string(label),mono?9.5f:10,disabled?t->muted:t->text,0,
        (cui_text_style){.family=mono?1:0,.monospace=mono});
    cui_end_box(ui);
    return interaction.activated;
}
static float lane_height(const CastProjectSnapshot *p,unsigned track)
{
    bool audio=false;
    for(unsigned i=0;i<p->clip_count;++i) {
        if(p->clips[i].track!=track)continue;
        const CastProjectMediaView *m=media(p,p->clips[i].media_id);
        if(!m||m->kind!=CAST_TIMELINE_AUDIO)return 40;
        audio=true;
    }
    return audio?34:40;
}
static unsigned moved_track(const CastProjectSnapshot *p,unsigned original,float dy)
{
    float center=0;
    for(unsigned i=0;i<original;++i)center+=lane_height(p,i);
    center+=lane_height(p,original)/2+dy;
    float bottom=0;
    for(unsigned i=0;i<CAST_TIMELINE_TRACK_MAX;++i) {
        bottom+=lane_height(p,i);
        if(center<bottom)return i;
    }
    return CAST_TIMELINE_TRACK_MAX-1;
}
void app_timeline_dock(cui_context *ui, const cui_theme *t, const CastProjectSnapshot *p,
                       AppTimelineOptions o, AppTimelineSubmit submit, void *user)
{
    const bool blocked = o.blocked || !o.connected || p->busy;
    TimelineView *v = cui_resource_get(ui, cui_id_from(ui, "timeline-view-state"),
        UINT64_C(0x74696d656c696e65), sizeof *v, NULL, NULL);
    if (!v) return;
    if (!selected(p, v->selected)) v->selected = 0;
    if(v->zoom<1)v->zoom=1;
    const bool editing_locked=blocked||p->recording||o.recording||o.countdown||o.finalizing||p->playing||p->exporting;
    if(editing_locked||v->generation!=o.generation||!selected(p,v->drag.id)||v->drag.revision!=p->revision)
        {if(v->drag.id)v->cancelled_drag=v->drag.id;v->drag.id=0;}
    if(v->generation!=o.generation)memset(&v->window,0,sizeof v->window);
    v->generation=o.generation;
    CastDesignTokens tokens = cast_design_tokens(t->dark);
    cui_begin_box(ui, cui_id_from(ui, "timeline-dock"), (cui_box){.width = -1,
        .height = o.height, .fixed_height = true, .clip = true, .background = t->surface,
        .border = t->border, .custom_border_edges = true, .border_edges = {.top = 1}});
    cui_begin_box(ui, cui_id_from(ui, "timeline-transport"), (cui_box){.width = -1,
        .height = 43, .row = true, .padding = 7, .gap = 8, .align_y = CUI_ALIGN_CENTER,
        .border = tokens.hair, .custom_border_edges = true, .border_edges = {.bottom = 1}});
    if(button(ui,t,"timeline-play",p->playing?"Pause":"Play","film",blocked || p->recording || !p->clip_count || p->missing_count))
        send(submit,user,"timeline",p->playing?"pause":"play",NULL,NULL);
    if (button(ui, t, "timeline-record", o.recording ? "Stop" : o.countdown ? "Cancel" : "Record",
               "cast-rec", blocked || o.finalizing))
        send(submit, user, "record", o.countdown ? "cancel" : o.recording ? "stop" : "start", NULL, NULL);
    if (button(ui, t, "timeline-cut", o.cut ? "Resume" : "Cut", "cast-cut", blocked || !o.recording))
        send(submit, user, "record", o.cut ? "resume" : "cut", NULL, NULL);
    char elapsed[32]; timecode(elapsed, sizeof elapsed, p->playhead_ns);
    cui_text_ex(ui, cui_string(elapsed), 14, t->text, 0, (cui_text_style){.family = 1, .monospace = true});
    cui_begin_box(ui, 0, (cui_box){.width = -1, .height = 1}); cui_end_box(ui);
    if (o.width > 560) cui_text(ui, cui_string(p->recording ? "writing" : p->preview_busy ? "seeking" : p->reviewing ? "review" : p->busy ? "working" : "idle"), 11, t->muted);
    if(p->reviewing && button(ui,t,"timeline-live","Live","cast-cam",blocked))send(submit,user,"timeline","live",NULL,NULL);
    if(o.width>720) {
        if(button(ui,t,"timeline-export",p->exporting?"Cancel export":"Export…","cast-save",blocked || (!p->exporting && (!p->clip_count || p->recording || p->missing_count)))) {
            if(p->exporting)send(submit,user,"timeline","export","cancel",NULL);
            else send(submit,user,"file","export",NULL,NULL);
        }
        if(p->exporting && p->export.duration_ns) {
            char progress[32];snprintf(progress,sizeof progress,"%.0f%%",100.0*(double)p->export.completed_ns/(double)p->export.duration_ns);
            cui_text(ui,cui_string(progress),11,t->muted);
        }
    }
    if (button(ui, t, "timeline-save", "Save", "cast-save", blocked || !p->open || !p->path[0]))
        send(submit, user, "project", "save", NULL, NULL);
    cui_end_box(ui);
    bool audio_status_visible=p->playing||p->playback_audio_error[0];
    if(audio_status_visible) {
        const char *audio_status=p->playback_audio_error[0]?p->playback_audio_error:
            p->playback_audio_running?"Review audio · default output":
            p->playback_audio_requested?"Review audio starting…":"Review audio off";
        cui_begin_box(ui,0,(cui_box){.width=-1,.height=16,.padding=2});
        cui_text_ellipsis(ui,cui_id_from(ui,"timeline-review-audio"),cui_string(audio_status),
            (cui_text_ellipsis_options){.width=fmaxf(1,o.width-4),.font_size=10,.color=p->playback_audio_error[0]?t->danger:t->muted});
        cui_end_box(ui);
    }
    const uint64_t extent = p->duration_ns > UINT64_C(10000000000) ? p->duration_ns : UINT64_C(10000000000);
    const float track_width = fmaxf(1, o.width - 86);
    const float content_width=track_width*v->zoom;
    v->pan.x=fmaxf(0,fminf(v->pan.x,content_width-track_width));
    const double visible=(double)extent/v->zoom;
    const double origin=(double)v->pan.x/content_width*extent;
    uint64_t window_start=(uint64_t)origin,window_span=(uint64_t)visible;
    if(!v->window.initialized) {
        v->window.start=window_start;v->window.span=window_span;v->window.initialized=true;
    } else if(v->window.start!=window_start||v->window.span!=window_span) {
        v->window.start=window_start;v->window.span=window_span;v->window.idle=0;v->window.dirty=true;
    }
    if(o.assets_window_requests&&v->window.dirty) {
        v->window.idle+=cui_frame_delta(ui);
        if(v->window.idle>=.2f&&!editing_locked) {
            char start[32],span[32];snprintf(start,sizeof start,"%llu",(unsigned long long)window_start);
            snprintf(span,sizeof span,"%llu",(unsigned long long)window_span);
            const char *args[]={"timeline","assets","window",start,span};
            if(submit&&submit(user,5,args))v->window.dirty=false;
        }
        if(!editing_locked)cui_invalidate(ui);
    }
    uint64_t completed_drag=0;
    float pointer_x=0,pointer_y=0;
    bool pointer_valid=cui_pointer_position(ui,&pointer_x,&pointer_y);
    if(v->cancelled_drag) {
        size_t count=0;const cui_event *events=cui_frame_events(ui,&count);
        for(size_t e=0;e<count;++e)if(events[e].type==CUI_EVENT_POINTER_UP) {
            completed_drag=v->cancelled_drag;v->cancelled_drag=0;cui_cancel_event_activation(ui,e);break;
        }
    }
    if(v->drag.id&&pointer_valid) {
        if(fabsf(pointer_x-v->drag.x)>4||fabsf(pointer_y-v->drag.y)>4)v->drag.moved=true;
        size_t event_count=0;const cui_event *events=cui_frame_events(ui,&event_count);
        for(size_t e=0;e<event_count;++e) {
            if(events[e].type==CUI_EVENT_CANCEL||(events[e].type==CUI_EVENT_KEY_DOWN&&events[e].key==CUI_KEY_ESCAPE)) {v->cancelled_drag=v->drag.id;v->drag.id=0;break;}
            if(events[e].type!=CUI_EVENT_POINTER_UP||events[e].pointer_button!=CUI_POINTER_PRIMARY)continue;
            const CastTimelineClip *c=selected(p,v->drag.id);
            if(v->drag.moved) {
                completed_drag=v->drag.id;
                cui_cancel_event_activation(ui,e);
                if(c&&!editing_locked&&!c->capture_locked) {
                    double delta=(pointer_x-v->drag.x)/track_width*visible;
                    char id[32],value[80];snprintf(id,sizeof id,"%llu",(unsigned long long)c->id);
                    if(v->drag.mode==2) {
                        double minimum=c->start_ns>c->source_in_ns?(double)(c->start_ns-c->source_in_ns):0;
                        uint64_t start=(uint64_t)fmax(minimum,fmin((double)(c->start_ns+c->duration_ns)-1000000,(double)c->start_ns+delta));
                        snprintf(value,sizeof value,"%llu",(unsigned long long)start);
                        send(submit,user,"timeline","trim-left",id,value);
                    } else if(v->drag.mode==1) {
                        uint64_t duration=(uint64_t)fmax(1000000,fmin((double)CAST_TIMELINE_LIMIT_NS,(double)c->duration_ns+delta));
                        snprintf(value,sizeof value,"%llu,%llu",(unsigned long long)c->source_in_ns,(unsigned long long)duration);
                        send(submit,user,"timeline","trim",id,value);
                    } else {
                        uint64_t start=(uint64_t)fmax(0,fmin((double)CAST_TIMELINE_LIMIT_NS-c->duration_ns,(double)c->start_ns+delta));
                        unsigned track=moved_track(p,c->track,pointer_y-v->drag.y);
                        snprintf(value,sizeof value,"%u,%llu",track,(unsigned long long)start);
                        send(submit,user,"timeline","move",id,value);
                    }
                }
            }
            v->drag.id=0;break;
        }
    }
    cui_begin_box(ui, cui_id_from(ui, "timeline-ruler"), (cui_box){.width = -1,
        .height = 22, .row = true, .border = tokens.hair, .border_width = 1});
    cui_begin_box(ui, 0, (cui_box){.width=74,.fixed_width=true,.row=true,.gap=1});
    if(ruler_button(ui,t,"timeline-zoom-fit","Fit",28,editing_locked,false)) {v->zoom=1;v->pan.x=0;cui_invalidate(ui);}
    if(ruler_button(ui,t,"timeline-zoom-out","−",21,editing_locked||v->zoom<=1,false)) {v->zoom=fmaxf(1,v->zoom/2);cui_invalidate(ui);}
    if(ruler_button(ui,t,"timeline-zoom-in","+",21,editing_locked||v->zoom>=32,false)) {v->zoom=fminf(32,v->zoom*2);cui_invalidate(ui);}
    cui_end_box(ui);
    unsigned tick_count=o.width<500?3:6;
    for (unsigned i = 0; i < tick_count; ++i) {
        char label[32], id[32]; timecode(label, sizeof label, (uint64_t)(origin+visible*i/(tick_count-1)));
        snprintf(id, sizeof id, "timeline-tick-%u", i);
        if (ruler_button(ui,t,id,label,track_width/tick_count,blocked||!p->open,true)) {
            uint64_t position_ns=(uint64_t)(origin+visible*i/(tick_count-1));if(position_ns>p->duration_ns)position_ns=p->duration_ns;
            char position[32]; snprintf(position, sizeof position, "%llu", (unsigned long long)position_ns);
            send(submit, user, "timeline", "seek", position, NULL);
        }
    }
    cui_end_box(ui);
    cui_scroll_options pan_options={.width=o.width,.height=12,.content_width=content_width+74,
        .content_height=1,.horizontal=true,.disabled=editing_locked,.bar_thickness=8};
    cui_id pan_id=cui_id_from(ui,"timeline-pan");
    if(cui_scrollable_begin(ui,pan_id,t,&v->pan,pan_options)) {
        cui_begin_box(ui,0,(cui_box){.width=content_width+74,.height=1});cui_end_box(ui);
        cui_scrollable_end_ex(ui,pan_id,t,&v->pan,pan_options);
    }
    unsigned tracks = 3;
    for (unsigned i = 0; i < p->clip_count; ++i) if (p->clips[i].track > tracks) tracks = p->clips[i].track;
    float lanes_height = fmaxf(40, o.height - 113 - (audio_status_visible?16:0));
    float content_height=0;for(unsigned i=0;i<=tracks;++i)content_height+=lane_height(p,i);
    cui_id scroll_id = cui_id_from(ui, "timeline-tracks");
    cui_scroll_options scroll_options = {.width = o.width, .height = lanes_height,
        .content_height = content_height, .content_width = o.width,
        .vertical = true, .hide_scrollbars = false};
    if (cui_scrollable_begin(ui, scroll_id, t, &v->scroll, scroll_options)) {
        for (unsigned track = 0; track <= tracks; ++track) {
            float height=lane_height(p,track),clip_height=height-10;
            char lane_id[40], label[32]; snprintf(lane_id, sizeof lane_id, "timeline-track-%u", track);
            snprintf(label, sizeof label, "%s %u", track == 0 ? "TAKES" : "TRACK", track == 0 ? 1 : track);
            cui_begin_box(ui, cui_id_from(ui, lane_id), (cui_box){.width = -1, .height = height, .row = true,
                .border = tokens.hair, .custom_border_edges = true, .border_edges = {.bottom = 1}});
            cui_begin_box(ui, 0, (cui_box){.width = 74, .fixed_width = true, .height = height,
                .padding = 8, .align_y = CUI_ALIGN_CENTER, .border = tokens.hair,
                .custom_border_edges = true, .border_edges = {.right = 1}});
            cui_text(ui, cui_string(label), 10, t->muted); cui_end_box(ui);
            cui_begin_box(ui, cui_id_from(ui,lane_id)^UINT64_C(0x6c616e65), (cui_box){.width = -1, .height = height, .clip = true});
            Playhead head = {.fraction = (float)(((double)p->playhead_ns-origin) / visible), .color = t->primary};
            cui_set_prepaint(ui,(cui_prepaint_options){.callback=paint_playhead,.payload=&head,
                .payload_bytes=sizeof head,.command_budget=3,.deferred_layer=20,.hover_hitbox=false});
            for (unsigned i = 0; i < p->clip_count; ++i) {
                const CastTimelineClip *c = &p->clips[i];
                if (c->track != track) continue;
                const CastProjectMediaView *m = media(p, c->media_id);
                float x = (float)(((double)c->start_ns-origin) / visible * track_width);
                float width = fmaxf(1, (float)((double)c->duration_ns / visible * track_width));
                char id[40]; snprintf(id, sizeof id, "timeline-clip-%llu", (unsigned long long)c->id);
                bool chosen = v->selected == c->id;
                cui_theme colors = *t;
                colors.secondary = chosen ? tokens.selected : tokens.control;
                colors.border = chosen ? tokens.selected_line : t->border;
                cui_begin_absolute_box(ui, cui_id_from(ui, id) ^ 1, (cui_box){.clip = true}, (cui_rect){x, 5, width, clip_height});
                cui_interaction body=cui_base_button(ui,cui_id_from(ui,id),
                    cui_string(m?m->label:"Missing media"),!o.connected);
                cui_update_semantic(ui,cui_id_from(ui,id),chosen,0);
                cui_begin_box(ui,cui_id_from(ui,id),(cui_box){.width=width,.height=clip_height,
                    .background=body.hovered?cui_color_mix_oklab(colors.secondary,t->text,.06f):colors.secondary,
                    .border=colors.border,.border_width=1,.clip=true});
                const CastProjectAssetView *asset=o.connected&&!c->capture_locked?clip_asset(p,c):NULL;
                if(asset&&asset->thumbnail_ready&&width>=4) {
                    char name[56];snprintf(name,sizeof name,"timeline-thumbnail-%llu",(unsigned long long)c->id);
                    cui_begin_absolute_box(ui,cui_id_from(ui,name)^1,(cui_box){.clip=true},(cui_rect){1,1,fminf(width-2,clip_height*80.f/45),clip_height-2});
                    cui_paint_bitmap(ui,cui_id_from(ui,name),(cui_bitmap_paint){.bitmap={
                        .pixels=asset->rgba,.width=80,.height=45,.stride=80*4,.revision=asset->generation^asset->clip_id},.opacity=.8f},
                        (cui_box){.width=fminf(width-2,clip_height*80.f/45),.height=clip_height-2});
                    cui_end_box(ui);
                }
                if(asset&&asset->waveform_ready&&asset->bins&&asset->bins<=CAST_PROJECT_ASSET_BINS&&asset->span_ns) {
                    char name[56];snprintf(name,sizeof name,"timeline-waveform-%llu",(unsigned long long)c->id);
                    WavePaint wave={.bins=asset->bins,.color=t->secondary};wave.color.a=140;
                    memcpy(wave.peak,asset->peak,sizeof wave.peak);
                    float span=(float)((double)asset->span_ns/c->duration_ns);
                    float offset=(float)((double)(asset->source_start_ns-c->source_in_ns)/c->duration_ns);
                    cui_begin_absolute_box(ui,cui_id_from(ui,name),(cui_box){.clip=true},(cui_rect){width*offset,4,fmaxf(1,width*span),clip_height-8});
                    cui_set_prepaint(ui,(cui_prepaint_options){.callback=paint_wave,.payload=&wave,.payload_bytes=sizeof wave,.command_budget=192,.deferred_layer=1});
                    cui_end_box(ui);
                }
                cui_begin_absolute_box(ui,cui_id_from(ui,id)^UINT64_C(0x6c6162656c),(cui_box){.clip=true},(cui_rect){6,clip_height-13,fmaxf(1,width-12),12});
                cui_text_ellipsis(ui,cui_id_from(ui,id)^UINT64_C(0x74657874),cui_string(m?m->label:"Missing media"),
                    (cui_text_ellipsis_options){.width=fmaxf(1,width-12),.font_size=9.5f,.color=t->text});
                cui_end_box(ui);cui_end_box(ui);
                if(body.activated&&completed_drag!=c->id) {
                    v->selected=chosen?0:c->id;cui_invalidate(ui);
                    if(v->selected&&!blocked&&!c->capture_locked&&!clip_asset(p,c)) {
                        char selected_id[32];snprintf(selected_id,sizeof selected_id,"%llu",(unsigned long long)c->id);
                        send(submit,user,"timeline","assets","focus",selected_id);
                    }
                }
                if(!editing_locked&&!c->capture_locked&&cui_pressed(ui,cui_id_from(ui,id))&&!v->drag.id&&pointer_valid) {
                    v->drag.id=c->id;v->drag.revision=p->revision;v->drag.generation=o.generation;
                    v->drag.x=pointer_x;v->drag.y=pointer_y;v->drag.mode=0;v->drag.moved=false;
                }
                if(chosen&&width>=20) {
                    char handle[48];snprintf(handle,sizeof handle,"timeline-trim-end-%llu",(unsigned long long)c->id);
                    cui_begin_absolute_box(ui,cui_id_from(ui,handle)^1,(cui_box){.background=t->primary},(cui_rect){width-6,0,6,clip_height});
                    cui_button_ex(ui,cui_id_from(ui,handle),t,cui_string(""),(cui_button_options){.width=6,.height=clip_height,.disabled=editing_locked||c->capture_locked,.accessibility_label=cui_string("Drag clip end to trim")});
                    if(cui_pressed(ui,cui_id_from(ui,handle))&&!v->drag.id&&pointer_valid) {
                        v->drag.id=c->id;v->drag.revision=p->revision;v->drag.generation=o.generation;
                        v->drag.x=pointer_x;v->drag.y=pointer_y;v->drag.mode=1;v->drag.moved=false;
                    }
                    cui_end_box(ui);
                }
                if(chosen&&width>=20) {
                    char handle[48];snprintf(handle,sizeof handle,"timeline-trim-start-%llu",(unsigned long long)c->id);
                    cui_begin_absolute_box(ui,cui_id_from(ui,handle)^1,(cui_box){.background=t->primary},(cui_rect){0,0,6,clip_height});
                    cui_button_ex(ui,cui_id_from(ui,handle),t,cui_string(""),(cui_button_options){.width=6,.height=clip_height,.disabled=editing_locked||c->capture_locked,.accessibility_label=cui_string("Drag clip start to trim")});
                    if(cui_pressed(ui,cui_id_from(ui,handle))&&!v->drag.id&&pointer_valid) {
                        v->drag.id=c->id;v->drag.revision=p->revision;v->drag.generation=o.generation;
                        v->drag.x=pointer_x;v->drag.y=pointer_y;v->drag.mode=2;v->drag.moved=false;
                    }
                    cui_end_box(ui);
                }
                cui_end_box(ui);
                if(v->drag.id==c->id&&v->drag.moved&&pointer_valid) {
                    float delta=pointer_x-v->drag.x;
                    cui_color ghost=t->primary;ghost.a=60;
                    cui_begin_absolute_box(ui,cui_id_from(ui,id)^UINT64_C(0x64726166),
                        (cui_box){.background=ghost,.border=t->primary,.border_width=1},
                        (cui_rect){v->drag.mode==1?x:x+delta,5,v->drag.mode==1?fmaxf(1,width+delta):v->drag.mode==2?fmaxf(1,width-delta):width,clip_height});
                    cui_end_box(ui);
                }
            }
            cui_end_box(ui); cui_end_box(ui);
        }
        cui_scrollable_end_ex(ui, scroll_id, t, &v->scroll, scroll_options);
    }
    cui_begin_box(ui, cui_id_from(ui, "timeline-edit-bar"), (cui_box){.width = -1, .height = 36,
        .row = true, .gap = 6, .padding = 4, .clip = true, .align_y = CUI_ALIGN_CENTER});
    const CastTimelineClip *clip = selected(p, v->selected);
    if (clip) {
        char id[32]; snprintf(id, sizeof id, "%llu", (unsigned long long)clip->id);
        bool locked = blocked || clip->capture_locked;
        if (button(ui, t, "timeline-split", "Split", "cast-cut", locked || p->playhead_ns <= clip->start_ns || p->playhead_ns >= clip->start_ns + clip->duration_ns)) {
            char pos[32]; snprintf(pos, sizeof pos, "%llu", (unsigned long long)p->playhead_ns);
            send(submit, user, "timeline", "split", id, pos);
        }
        if (button(ui, t, "timeline-remove", "Remove", "close", locked)) send(submit, user, "timeline", "remove", id, NULL);
        if (button(ui, t, "timeline-mute", clip->muted ? "Unmute" : "Mute", "cast-mic", locked)) {
            char value[48]; snprintf(value, sizeof value, "%.4f,%d", clip->gain, !clip->muted);
            send(submit, user, "timeline", "gain", id, value);
        }
        if (o.width > 700) {
            cui_push_id(ui, id);
            char current[80]; snprintf(current, sizeof current, "%llu,%llu", (unsigned long long)clip->source_in_ns, (unsigned long long)clip->duration_ns);
            bool submitted = false;
            const char *value = field(ui, t, "timeline-trim", "Source in,duration (ns)", current, 180, locked, &submitted);
            if (submitted) send(submit, user, "timeline", "trim", id, value);
            cui_pop_id(ui);
        }
    } else {
        bool submitted = false;
        const char *path = field(ui, t, "timeline-import-path", "Media file path", "", o.width > 600 ? 240 : 140, blocked, &submitted);
        if (button(ui, t, "timeline-import", o.width>700?"Insert clip":"Insert", "cast-folder", blocked || !path[0]) || submitted)
            send(submit, user, "timeline", "import", path, NULL);
    }
    if(button(ui,t,"timeline-browse",o.width>700?"Insert clip…":"Browse…","cast-folder",blocked))send(submit,user,"file","import",NULL,NULL);
    if (button(ui, t, "timeline-undo", "Undo", "undo", blocked || p->recording)) send(submit, user, "timeline", "undo", NULL, NULL);
    if (button(ui, t, "timeline-redo", "Redo", "redo", blocked || p->recording)) send(submit, user, "timeline", "redo", NULL, NULL);
    cui_end_box(ui);
    cui_end_box(ui);
}
