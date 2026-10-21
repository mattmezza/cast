/* SPDX-License-Identifier: MIT */
#include "stream_controls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    cui_context *ui;cui_theme theme;AppStreamSnapshot stream;
    unsigned page,calls;float width;bool blocked;AppStreamIntent intent;char value[4096];
} Fixture;
static cui_size measure(void *user,cui_str text,float size)
{(void)user;return (cui_size){text.length*size*.6f,size*1.2f};}
static bool submit(void *user,AppStreamIntent intent,const char *value)
{
    Fixture *f=user;assert(!f->blocked);++f->calls;f->intent=intent;
    snprintf(f->value,sizeof f->value,"%s",value);return true;
}
static void frame(Fixture *f)
{
    assert(cui_begin_frame(f->ui,f->width,1400,.016f));
    cui_begin_box(f->ui,cui_id_from(f->ui,"root"),(cui_box){.width=-1,.gap=10,.padding=10});
    if(f->page==0)app_stream_output(f->ui,&f->theme,&f->stream,f->blocked,submit,f);
    else if(f->page==1)app_stream_destination(f->ui,&f->theme,&f->stream,f->blocked,submit,f);
    else app_stream_encoding(f->ui,&f->theme,&f->stream,f->blocked,submit,f);
    cui_end_box(f->ui);cui_draw_list draw=cui_end_frame(f->ui);assert(draw.count>0);
    if(cui_last_error(f->ui)!=CUI_OK){cui_diagnostic d;if(cui_first_diagnostic(f->ui,&d))fprintf(stderr,"%s %s\n",d.operation,d.message);}
    assert(cui_last_error(f->ui)==CUI_OK);
}
static void settle(Fixture *f){frame(f);frame(f);}
static void event(Fixture *f,cui_event e){assert(cui_queue_event(f->ui,e));frame(f);}
static void click(Fixture *f,const char *name)
{
    cui_rect r=cui_bounds(f->ui,cui_id_from(f->ui,name));
    assert(r.w>0&&r.h>0&&r.x>=0&&r.x+r.w<=f->width+1&&r.y+r.h<=1401);
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
    Fixture f={.theme=cui_theme_dark(),.width=340};f.ui=cui_create((cui_config){.width=340,.height=1400,.measure=measure});assert(f.ui);
    f.stream=(AppStreamSnapshot){.connected=true,.generation=1,.paused=true,.phase=APP_STREAM_STOPPED,
        .video_bitrate_kbps=2500,.audio_bitrate_kbps=128,.reconnect_attempts=3,.reconnect_initial_ms=500,.reconnect_max_ms=4000};
    strcpy(f.stream.server_url,"rtmp://127.0.0.1/app");strcpy(f.stream.key_file,"/private/stream.key");
    strcpy(f.stream.service,"custom");strcpy(f.stream.encoder_preset,"veryfast");
    assert(app_stream_value_valid(APP_STREAM_SERVER_URL,f.stream.server_url));
    assert(!app_stream_value_valid(APP_STREAM_SERVER_URL,"rtmp://user:secret@host/app"));
    assert(!app_stream_value_valid(APP_STREAM_SERVER_URL,"rtmp://host/app?TOKEN=secret"));
    assert(!app_stream_value_valid(APP_STREAM_KEY_FILE,"~/stream.key"));
    assert(!app_stream_value_valid(APP_STREAM_VIDEO_BITRATE,"50001"));
    assert(!app_stream_value_valid(APP_STREAM_VIDEO_BITRATE,"1e3"));
    assert(!app_stream_value_valid(APP_STREAM_RECONNECT_ATTEMPTS,"11"));
    assert(app_stream_value_valid(APP_STREAM_RECONNECT_ATTEMPTS,"0"));
    settle(&f);click(&f,"stream-go-live");assert(f.calls==0);
    click(&f,"stream-confirm-cancel");assert(f.calls==0);
    click(&f,"stream-go-live");click(&f,"stream-connect-paused");assert(f.calls==1&&f.intent==APP_STREAM_ACTION&&!strcmp(f.value,"start"));
    assert(!f.stream.active); /* Never optimistically mark a connection live. */
    f.blocked=true;settle(&f);click(&f,"stream-connect-paused");assert(f.calls==1);
    f.blocked=false;f.stream.active=true;f.stream.phase=APP_STREAM_CONNECTING;settle(&f);
    assert(!app_stream_action_allowed(&f.stream,false,"resume"));
    click(&f,"stream-stop");assert(f.calls==2&&!strcmp(f.value,"stop"));
    f.stream.phase=APP_STREAM_STREAMING;settle(&f);click(&f,"stream-reveal");assert(f.calls==3&&!strcmp(f.value,"resume"));
    f.stream.paused=false;settle(&f);click(&f,"stream-pause");assert(f.calls==4&&!strcmp(f.value,"pause"));
    f.stream.active=false;f.stream.phase=APP_STREAM_FAILED;settle(&f);click(&f,"stream-go-live");click(&f,"stream-connect-paused");assert(f.calls==5&&!strcmp(f.value,"start"));
    f.stream.connected=false;settle(&f);assert(!app_stream_action_allowed(&f.stream,false,"start"));
    f.stream.connected=true;++f.stream.generation;f.stream.phase=APP_STREAM_STOPPED;f.page=1;settle(&f);
    click(&f,"stream-service-youtube");assert(f.calls==6&&f.intent==APP_STREAM_SERVICE&&!strcmp(f.value,"youtube"));
    click(&f,"stream-server-url");key(&f,CUI_KEY_A,CUI_MOD_CTRL);
    event(&f,(cui_event){.type=CUI_EVENT_TEXT,.text="rtmps://localhost/app"});key(&f,CUI_KEY_ENTER,0);
    assert(f.calls==7&&f.intent==APP_STREAM_SERVER_URL&&!strcmp(f.value,"rtmps://localhost/app"));
    assert(!strcmp(f.stream.server_url,"rtmp://127.0.0.1/app"));
    click(&f,"stream-key-file");key(&f,CUI_KEY_A,CUI_MOD_CTRL);
    event(&f,(cui_event){.type=CUI_EVENT_TEXT,.text="/tmp/changed.key"});key(&f,CUI_KEY_ESCAPE,0);key(&f,CUI_KEY_ENTER,0);
    assert(strcmp(f.value,"/tmp/changed.key"));
    unsigned previous=f.calls;f.stream.active=true;settle(&f);click(&f,"stream-service-twitch");assert(f.calls==previous);
    f.page=2;settle(&f);click(&f,"stream-video-bitrate");key(&f,CUI_KEY_ENTER,0);assert(f.calls==previous);
    f.stream.active=false;settle(&f);click(&f,"stream-video-bitrate");key(&f,CUI_KEY_A,CUI_MOD_CTRL);
    event(&f,(cui_event){.type=CUI_EVENT_TEXT,.text="6000"});key(&f,CUI_KEY_ENTER,0);
    assert(f.calls==previous+1&&f.intent==APP_STREAM_VIDEO_BITRATE&&!strcmp(f.value,"6000"));
    for(unsigned width=260;width<=520;width+=260) {
        f.width=(float)width;
        for(f.page=0;f.page<3;++f.page)settle(&f);
    }
    cui_destroy(f.ui);puts("stream controls PASS: paused confirmation, lifecycle, acknowledged fields, locks and bounds");
}
