/* SPDX-License-Identifier: MIT */
#include "timeline_controls.h"
#include "cast_theme.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    cui_context *ui;cui_theme theme;CastProjectSnapshot project;
    unsigned page,calls;float width;bool blocked;char verb[32],action[32],value[4096],parameter[128];
} Fixture;
static cui_size measure(void *user,cui_str text,float size)
{(void)user;return (cui_size){text.length*size*.6f,size*1.2f};}
static bool submit(void *user,unsigned argc,const char *const *argv)
{
    Fixture *f=user;assert(!f->blocked && argc>=2 && argc<=4);++f->calls;
    snprintf(f->verb,sizeof f->verb,"%s",argv[0]);
    snprintf(f->action,sizeof f->action,"%s",argv[1]);
    snprintf(f->value,sizeof f->value,"%s",argc>2?argv[2]:"");
    snprintf(f->parameter,sizeof f->parameter,"%s",argc>3?argv[3]:"");return true;
}
static void frame(Fixture *f)
{
    assert(cui_begin_frame(f->ui,f->width,700,.016f));
    cui_begin_box(f->ui,cui_id_from(f->ui,"root"),(cui_box){.width=-1,.height=-1});
    if(f->page)app_project_controls(f->ui,&f->theme,&f->project,f->blocked,submit,f);
    else app_timeline_dock(f->ui,&f->theme,&f->project,
        (AppTimelineOptions){.connected=true,.blocked=f->blocked,.width=f->width,.height=262},submit,f);
    cui_end_box(f->ui);cui_draw_list draw=cui_end_frame(f->ui);
    if(cui_last_error(f->ui)!=CUI_OK){cui_diagnostic d;if(cui_first_diagnostic(f->ui,&d))fprintf(stderr,"%s %s\n",d.operation,d.message);}
    assert(cui_last_error(f->ui)==CUI_OK);assert(draw.count>0);
}
static void settle(Fixture *f){frame(f);frame(f);}
static void event(Fixture *f,cui_event e){assert(cui_queue_event(f->ui,e));frame(f);}
static void click(Fixture *f,const char *name)
{
    cui_rect r=cui_bounds(f->ui,cui_id_from(f->ui,name));
    if(!(r.w>0&&r.h>0&&r.x>=0&&r.x+r.w<=f->width+1&&r.y+r.h<=701))fprintf(stderr,"%s: %.1f %.1f %.1f %.1f\n",name,r.x,r.y,r.w,r.h);
    assert(r.w>0&&r.h>0&&r.x>=0&&r.x+r.w<=f->width+1&&r.y+r.h<=701);
    event(f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=r.x+r.w/2,.y=r.y+r.h/2});
    event(f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=r.x+r.w/2,.y=r.y+r.h/2});settle(f);
}
static void key(Fixture *f,cui_key value,unsigned mods)
{
    event(f,(cui_event){.type=CUI_EVENT_KEY_DOWN,.key=value,.modifiers=mods});
    event(f,(cui_event){.type=CUI_EVENT_KEY_UP,.key=value,.modifiers=mods});settle(f);
}
int main(void)
{
    Fixture f={.theme=cast_theme(true),.width=800};
    f.ui=cui_create((cui_config){.width=800,.height=700,.measure=measure});assert(f.ui);
    f.project.open=true;f.project.duration_ns=UINT64_C(10000000000);
    f.project.playhead_ns=UINT64_C(5000000000);
    f.project.media_count=1;f.project.media[0].id=1;strcpy(f.project.media[0].label,"Interview.mp4");
    f.project.clip_count=1;f.project.clips[0]=(CastTimelineClip){.id=2,.media_id=1,.gain=1,.duration_ns=UINT64_C(10000000000)};
    f.project.asset_count=1;f.project.assets[0]=(CastProjectAssetView){.clip_id=2,.media_id=1,
        .generation=1,.span_ns=UINT64_C(10000000000),.pending=true};
    settle(&f);click(&f,"timeline-clip-2");click(&f,"timeline-split");
    assert(f.calls==1&&!strcmp(f.verb,"timeline")&&!strcmp(f.action,"split")&&!strcmp(f.value,"2"));
    assert(f.project.clip_count==1);
    f.project.clips[0].capture_locked=true;settle(&f);click(&f,"timeline-remove");assert(f.calls==1);
    f.project.clips[0].capture_locked=false;settle(&f);click(&f,"timeline-mute");assert(f.calls==2&&!strcmp(f.action,"gain"));
    click(&f,"timeline-clip-2");click(&f,"timeline-import-path");
    event(&f,(cui_event){.type=CUI_EVENT_TEXT,.text="/tmp/interview.mp4"});key(&f,CUI_KEY_ENTER,0);
    assert(f.calls==3&&!strcmp(f.action,"import")&&!strcmp(f.value,"/tmp/interview.mp4"));
    f.blocked=true;settle(&f);click(&f,"timeline-record");assert(f.calls==3);
    f.blocked=false;f.page=1;strcpy(f.project.path,"/tmp/talk.castproject");settle(&f);
    click(&f,"project-save");assert(f.calls==4&&!strcmp(f.verb,"project")&&!strcmp(f.action,"save"));
    f.project.busy=true;settle(&f);click(&f,"project-open");assert(f.calls==4);
    f.project.busy=false;
    for(unsigned width=260;width<=1040;width*=2){f.width=(float)width;for(f.page=0;f.page<2;++f.page)settle(&f);}
    f.page=0;f.width=800;settle(&f);
    unsigned seek_before=f.calls;click(&f,"timeline-tick-1");
    assert(f.calls==seek_before+1&&!strcmp(f.action,"seek")&&f.project.playhead_ns==UINT64_C(5000000000));
    unsigned before=f.calls;
    float original_width=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2")).w;
    click(&f,"timeline-zoom-in");assert(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2")).w>original_width);
    float unpanned=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2")).x;
    cui_focus(f.ui,cui_scrollbar_id(cui_id_from(f.ui,"timeline-pan"),false));key(&f,CUI_KEY_END,0);
    assert(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2")).x<unpanned);
    click(&f,"timeline-zoom-out");click(&f,"timeline-zoom-fit");
    assert(f.calls==before);
    click(&f,"timeline-clip-2");
    if(!cui_bounds(f.ui,cui_id_from(f.ui,"timeline-trim-end-2")).w)click(&f,"timeline-clip-2");
    cui_rect clip_bounds=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2"));
    float x=clip_bounds.x+30,y=clip_bounds.y+15;
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=x,.y=y});
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_MOVE,.x=x+40,.y=y});
    assert(f.calls==before);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=x+40,.y=y});settle(&f);
    assert(f.calls==before+1&&!strcmp(f.action,"move")&&!strcmp(f.value,"2"));
    assert(f.project.clips[0].start_ns==0);
    cui_rect handle=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-trim-end-2"));assert(handle.w>0);
    x=handle.x+handle.w/2;y=handle.y+handle.h/2;
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=x,.y=y});
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_MOVE,.x=x-40,.y=y});assert(f.calls==before+1);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=x-40,.y=y});settle(&f);
    assert(f.calls==before+2&&!strcmp(f.action,"trim")&&!strcmp(f.value,"2"));
    assert(f.project.clips[0].duration_ns==UINT64_C(10000000000));
    cui_rect left=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-trim-start-2"));assert(left.w>0);
    x=left.x+left.w/2;y=left.y+left.h/2;
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=x,.y=y});
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_MOVE,.x=x+40,.y=y});assert(f.calls==before+2);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=x+40,.y=y});settle(&f);
    assert(f.calls==before+3&&!strcmp(f.action,"trim-left")&&!strcmp(f.value,"2"));
    assert(f.project.clips[0].start_ns==0&&f.project.clips[0].source_in_ns==0);
    clip_bounds=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2"));x=clip_bounds.x+30;y=clip_bounds.y+15;
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=x,.y=y});
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_MOVE,.x=x+60,.y=y});
    f.blocked=true;settle(&f);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=x+60,.y=y});assert(f.calls==before+3);
    f.blocked=false;settle(&f);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=x,.y=y});
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_MOVE,.x=x+60,.y=y});
    ++f.project.revision;settle(&f);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=x+60,.y=y});assert(f.calls==before+3);
    f.project.media[0].kind=CAST_TIMELINE_AUDIO;settle(&f);
    assert(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-track-0")).h==34);
    assert(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2")).h==24);
    clip_bounds=cui_bounds(f.ui,cui_id_from(f.ui,"timeline-clip-2"));x=clip_bounds.x+30;y=clip_bounds.y+12;
    unsigned audio_before=f.calls;
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_DOWN,.x=x,.y=y});
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_MOVE,.x=x,.y=y+34});assert(f.calls==audio_before);
    event(&f,(cui_event){.type=CUI_EVENT_POINTER_UP,.x=x,.y=y+34});settle(&f);
    assert(f.calls==audio_before+1&&!strcmp(f.action,"move")&&!strncmp(f.parameter,"1,",2));
    for(unsigned width=260;width<=1040;width*=2){f.width=(float)width;settle(&f);}
    f.width=800;f.project.asset_count=1;
    CastProjectAssetView *asset=&f.project.assets[0];
    *asset=(CastProjectAssetView){.clip_id=2,.media_id=1,.revision=f.project.revision,
        .generation=1,.span_ns=UINT64_C(10000000000),.thumbnail_ready=true,.waveform_ready=true,.bins=256};
    for(unsigned i=0;i<sizeof asset->rgba;i+=4){asset->rgba[i]=255;asset->rgba[i+3]=255;}
    for(unsigned i=0;i<256;++i)asset->peak[i]=i/255.f;
    unsigned asset_before=f.calls;settle(&f);
    assert(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-thumbnail-2")).w>0);
    assert(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-waveform-2")).w>0);
    assert(f.calls==asset_before);
    ++asset->revision;settle(&f);
    assert(!cui_bounds(f.ui,cui_id_from(f.ui,"timeline-thumbnail-2")).w);
    asset->revision=f.project.revision;f.project.clips[0].capture_locked=true;settle(&f);
    assert(!cui_bounds(f.ui,cui_id_from(f.ui,"timeline-waveform-2")).w);
    f.project.clips[0].capture_locked=false;asset->waveform_ready=false;asset->thumbnail_ready=false;asset->pending=true;settle(&f);
    assert(!cui_bounds(f.ui,cui_id_from(f.ui,"timeline-thumbnail-2")).w);
    assert(f.calls==asset_before);
    f.project.asset_count=0;settle(&f);
    if(cui_bounds(f.ui,cui_id_from(f.ui,"timeline-trim-end-2")).w)click(&f,"timeline-clip-2");
    unsigned focus_before=f.calls;click(&f,"timeline-clip-2");
    assert(f.calls==focus_before+1&&!strcmp(f.action,"assets")&&!strcmp(f.value,"focus")&&!strcmp(f.parameter,"2"));
    settle(&f);settle(&f);assert(f.calls==focus_before+1);
    cui_destroy(f.ui);puts("Timeline UI: clip selection, split, import, acknowledgement, capture locks and project controls passed");
}
