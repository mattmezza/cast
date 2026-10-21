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
    uint64_t selected, revision, generation;
    cui_scroll_state scroll, pan;
    float zoom;
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
static const char *field(cui_context *ui, const cui_theme *t, const char *name,
                         const char *label, const char *ack, float width, bool disabled,
                         bool *submitted)
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
    if ((strcmp(s->acknowledged, ack) && !focused) || (disabled && !s->disabled) || (s->focused && !focused))
        cui_edit_set_content(s->edit, (cui_edit_content){.text = cui_string(ack)});
    snprintf(s->acknowledged, sizeof s->acknowledged, "%s", ack);
    s->disabled = disabled; s->focused = focused;
    cui_input_result result = cui_input_ex(ui, id, t, cui_string(label), s->edit, &s->input,
        (cui_input_options){.width = width, .height = 28, .disabled = disabled});
    if (submitted) *submitted = result.submitted;
    return s->buffer;
}
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
void app_project_controls(cui_context *ui, const cui_theme *t, const CastProjectSnapshot *p,
                          bool blocked, AppTimelineSubmit submit, void *user)
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
        v->drag.id=0;
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
    uint64_t completed_drag=0;
    float pointer_x=0,pointer_y=0;
    bool pointer_valid=cui_pointer_position(ui,&pointer_x,&pointer_y);
    if(v->drag.id&&pointer_valid) {
        if(fabsf(pointer_x-v->drag.x)>4||fabsf(pointer_y-v->drag.y)>4)v->drag.moved=true;
        size_t event_count=0;const cui_event *events=cui_frame_events(ui,&event_count);
        for(size_t e=0;e<event_count;++e) {
            if(events[e].type==CUI_EVENT_CANCEL||(events[e].type==CUI_EVENT_KEY_DOWN&&events[e].key==CUI_KEY_ESCAPE)) {v->drag.id=0;break;}
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
                        int track=(int)c->track+(int)roundf((pointer_y-v->drag.y)/40);
                        if(track<0)track=0;
                        if(track>=CAST_TIMELINE_TRACK_MAX)track=CAST_TIMELINE_TRACK_MAX-1;
                        snprintf(value,sizeof value,"%d,%llu",track,(unsigned long long)start);
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
    if(cui_button_ex(ui,cui_id_from(ui,"timeline-zoom-fit"),t,cui_string("Fit"),(cui_button_options){.width=28,.height=22,.font_size=9,.disabled=editing_locked})) {v->zoom=1;v->pan.x=0;cui_invalidate(ui);}
    if(cui_button_ex(ui,cui_id_from(ui,"timeline-zoom-out"),t,cui_string("−"),(cui_button_options){.width=21,.height=22,.font_size=10,.disabled=editing_locked||v->zoom<=1})) {v->zoom=fmaxf(1,v->zoom/2);cui_invalidate(ui);}
    if(cui_button_ex(ui,cui_id_from(ui,"timeline-zoom-in"),t,cui_string("+"),(cui_button_options){.width=21,.height=22,.font_size=10,.disabled=editing_locked||v->zoom>=32})) {v->zoom=fminf(32,v->zoom*2);cui_invalidate(ui);}
    cui_end_box(ui);
    unsigned tick_count=o.width<500?3:6;
    for (unsigned i = 0; i < tick_count; ++i) {
        char label[32], id[32]; timecode(label, sizeof label, (uint64_t)(origin+visible*i/(tick_count-1)));
        snprintf(id, sizeof id, "timeline-tick-%u", i);
        if (cui_button_ex(ui, cui_id_from(ui, id), t, cui_string(label),
            (cui_button_options){.height = 22, .width = track_width / tick_count, .font_size = 9.5f,
                .variant = CUI_BUTTON_GHOST, .disabled = blocked || !p->open})) {
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
    cui_id scroll_id = cui_id_from(ui, "timeline-tracks");
    cui_scroll_options scroll_options = {.width = o.width, .height = lanes_height,
        .content_height = (tracks + 1) * 40, .content_width = o.width,
        .vertical = true, .hide_scrollbars = false};
    if (cui_scrollable_begin(ui, scroll_id, t, &v->scroll, scroll_options)) {
        for (unsigned track = 0; track <= tracks; ++track) {
            char lane_id[40], label[32]; snprintf(lane_id, sizeof lane_id, "timeline-track-%u", track);
            snprintf(label, sizeof label, "%s %u", track == 0 ? "TAKES" : "TRACK", track == 0 ? 1 : track);
            cui_begin_box(ui, cui_id_from(ui, lane_id), (cui_box){.width = -1, .height = 40, .row = true,
                .border = tokens.hair, .custom_border_edges = true, .border_edges = {.bottom = 1}});
            cui_begin_box(ui, 0, (cui_box){.width = 74, .fixed_width = true, .height = 40,
                .padding = 8, .align_y = CUI_ALIGN_CENTER, .border = tokens.hair,
                .custom_border_edges = true, .border_edges = {.right = 1}});
            cui_text(ui, cui_string(label), 10, t->muted); cui_end_box(ui);
            cui_begin_box(ui, cui_id_from(ui,lane_id)^UINT64_C(0x6c616e65), (cui_box){.width = -1, .height = 40, .clip = true});
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
                cui_begin_absolute_box(ui, cui_id_from(ui, id) ^ 1, (cui_box){.clip = true}, (cui_rect){x, 5, width, 30});
                if (cui_button_ex(ui, cui_id_from(ui, id), &colors,
                    cui_string(m ? m->label : "Missing media"), (cui_button_options){.width = width,
                        .height = 30, .font_size = 9.5f, .variant = CUI_BUTTON_OUTLINE,
                        .disabled = !o.connected, .toggle_semantics = true, .toggled = chosen}) && completed_drag!=c->id) {
                    v->selected = chosen ? 0 : c->id;
                    cui_invalidate(ui);
                }
                if(!editing_locked&&!c->capture_locked&&cui_pressed(ui,cui_id_from(ui,id))&&!v->drag.id&&pointer_valid) {
                    v->drag.id=c->id;v->drag.revision=p->revision;v->drag.generation=o.generation;
                    v->drag.x=pointer_x;v->drag.y=pointer_y;v->drag.mode=0;v->drag.moved=false;
                }
                if(chosen&&width>=20) {
                    char handle[48];snprintf(handle,sizeof handle,"timeline-trim-end-%llu",(unsigned long long)c->id);
                    cui_begin_absolute_box(ui,cui_id_from(ui,handle)^1,(cui_box){.background=t->primary},(cui_rect){width-6,0,6,30});
                    cui_button_ex(ui,cui_id_from(ui,handle),t,cui_string(""),(cui_button_options){.width=6,.height=30,.disabled=editing_locked||c->capture_locked,.accessibility_label=cui_string("Drag clip end to trim")});
                    if(cui_pressed(ui,cui_id_from(ui,handle))&&!v->drag.id&&pointer_valid) {
                        v->drag.id=c->id;v->drag.revision=p->revision;v->drag.generation=o.generation;
                        v->drag.x=pointer_x;v->drag.y=pointer_y;v->drag.mode=1;v->drag.moved=false;
                    }
                    cui_end_box(ui);
                }
                if(chosen&&width>=20) {
                    char handle[48];snprintf(handle,sizeof handle,"timeline-trim-start-%llu",(unsigned long long)c->id);
                    cui_begin_absolute_box(ui,cui_id_from(ui,handle)^1,(cui_box){.background=t->primary},(cui_rect){0,0,6,30});
                    cui_button_ex(ui,cui_id_from(ui,handle),t,cui_string(""),(cui_button_options){.width=6,.height=30,.disabled=editing_locked||c->capture_locked,.accessibility_label=cui_string("Drag clip start to trim")});
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
                        (cui_rect){v->drag.mode==1?x:x+delta,5,v->drag.mode==1?fmaxf(1,width+delta):v->drag.mode==2?fmaxf(1,width-delta):width,30});
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
        if (button(ui, t, "timeline-import", "Import", "cast-folder", blocked || !path[0]) || submitted)
            send(submit, user, "timeline", "import", path, NULL);
    }
    if(button(ui,t,"timeline-browse","Browse…","cast-folder",blocked))send(submit,user,"file","import",NULL,NULL);
    if (button(ui, t, "timeline-undo", "Undo", "undo", blocked || p->recording)) send(submit, user, "timeline", "undo", NULL, NULL);
    if (button(ui, t, "timeline-redo", "Redo", "redo", blocked || p->recording)) send(submit, user, "timeline", "redo", NULL, NULL);
    cui_end_box(ui);
    cui_end_box(ui);
}
