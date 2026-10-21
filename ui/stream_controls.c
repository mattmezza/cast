/* SPDX-License-Identifier: MIT */
#include "stream_controls.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool one_of(const char *value, const char *const *values, size_t count)
{
    for(size_t i=0;i<count;++i)if(!strcmp(value,values[i]))return true;
    return false;
}
static bool integer(const char *value,int low,int high)
{
    if(!*value || strlen(value)>8)return false;
    for(const char *p=value;*p;++p)if(*p<'0'||*p>'9')return false;
    long number=strtol(value,NULL,10);
    return number>=low && number<=high;
}
bool app_stream_value_valid(AppStreamIntent intent,const char *value)
{
    if(!value || !*value)return false;
    size_t length=strlen(value);
    for(const unsigned char *p=(const unsigned char *)value;*p;++p)
        if(*p<32 || *p==127)return false;
    static const char *const services[]={"custom","twitch","youtube"};
    static const char *const presets[]={"ultrafast","superfast","veryfast","faster","fast","medium","slow","slower","veryslow"};
    switch(intent) {
    case APP_STREAM_ACTION:return !strcmp(value,"start")||!strcmp(value,"stop")||!strcmp(value,"pause")||!strcmp(value,"resume")||!strcmp(value,"status");
    case APP_STREAM_SERVICE:return one_of(value,services,3);
    case APP_STREAM_SERVER_URL: {
        size_t prefix=!strncmp(value,"rtmp://",7)?7:!strncmp(value,"rtmps://",8)?8:0;
        if(!prefix || length>=1024 || strpbrk(value,"@?#%\\ "))return false;
        const char *path=strchr(value+prefix,'/');
        return path && path>value+prefix && path[1]!=0;
    }
    case APP_STREAM_KEY_FILE:return length<4096 && value[0]=='/';
    case APP_STREAM_VIDEO_BITRATE:return integer(value,100,50000);
    case APP_STREAM_AUDIO_BITRATE:return integer(value,32,320);
    case APP_STREAM_ENCODER_PRESET:return one_of(value,presets,9);
    case APP_STREAM_RECONNECT_ATTEMPTS:return integer(value,0,10);
    case APP_STREAM_RECONNECT_INITIAL:return integer(value,100,10000);
    case APP_STREAM_RECONNECT_MAX:return integer(value,100,30000);
    }
    return false;
}
bool app_stream_action_allowed(const AppStreamSnapshot *s,bool blocked,const char *action)
{
    if(!s || !s->connected || blocked || !app_stream_value_valid(APP_STREAM_ACTION,action))return false;
    if(!strcmp(action,"status"))return true;
    if(!strcmp(action,"start"))return !s->active && (s->phase==APP_STREAM_STOPPED || s->phase==APP_STREAM_FAILED);
    if(!strcmp(action,"stop"))return s->active || s->phase==APP_STREAM_FAILED;
    if(s->phase!=APP_STREAM_STREAMING || !s->active)return false;
    return !strcmp(action,"resume") ? s->paused : !s->paused;
}
static void card(cui_context *ui,const cui_theme *t,const char *id,const char *title)
{
    cui_begin_box(ui,cui_id_from(ui,id),(cui_box){.width=-1,.gap=9,.padding=14,
        .background=t->surface,.border=t->border,.border_width=1,.radius=10});
    cui_text_ex(ui,cui_string(title),13,t->text,0,(cui_text_style){.weight=CUI_TEXT_WEIGHT_SEMIBOLD});
}
static void note(cui_context *ui,const cui_theme *t,const char *text)
{
    cui_text(ui,cui_string(text),11,t->muted);
}
typedef struct {
    cui_edit *edit;cui_input_state input;
    char buffer[4096],acknowledged[4096];
    uint64_t generation;bool connected,focused,locked;
} StreamField;
static void field_destroy(void *user){cui_edit_destroy(((StreamField *)user)->edit);}
static void field(cui_context *ui,const cui_theme *t,const AppStreamSnapshot *s,bool blocked,
                  const char *name,const char *label,AppStreamIntent intent,const char *current,
                  AppStreamSubmit submit,void *user)
{
    cui_id id=cui_id_from(ui,name);
    StreamField *state=cui_resource_get(ui,id,UINT64_C(0x7374726d6669656c),sizeof *state,NULL,field_destroy);
    if(!state)return;
    bool locked=blocked||!s->connected||s->active||s->phase==APP_STREAM_STOPPING;
    bool focused=cui_focused(ui)==id;
    if(!state->edit) {
        snprintf(state->buffer,sizeof state->buffer,"%s",current);
        state->edit=cui_edit_create((cui_edit_config){.buffer=state->buffer,.capacity=sizeof state->buffer});
    }
    if(!state->edit)return;
    bool reset=strcmp(current,state->acknowledged)||s->generation!=state->generation||
        s->connected!=state->connected||(state->focused&&!focused)||(!state->locked&&locked);
    size_t count=0;const cui_event *events=cui_frame_events(ui,&count);
    for(size_t i=0;i<count;++i)
        if(focused&&!cui_event_consumed(ui,i)&&events[i].type==CUI_EVENT_KEY_DOWN&&events[i].key==CUI_KEY_ESCAPE) {
            reset=true;cui_consume_event(ui,i);
        }
    if(reset)cui_edit_set_content(state->edit,(cui_edit_content){.text=cui_string(current)});
    snprintf(state->acknowledged,sizeof state->acknowledged,"%s",current);
    state->generation=s->generation;state->connected=s->connected;state->focused=focused;state->locked=locked;
    cui_text(ui,cui_string(label),11.5f,t->text);
    cui_input_result result=cui_input_ex(ui,id,t,cui_string(label),state->edit,&state->input,
        (cui_input_options){.width=-1,.height=34,.disabled=locked,
            .invalid=state->buffer[0]&&!app_stream_value_valid(intent,state->buffer)});
    if(result.submitted&&!locked&&submit&&app_stream_value_valid(intent,state->buffer)) {
        submit(user,intent,state->buffer);
        cui_edit_set_content(state->edit,(cui_edit_content){.text=cui_string(current)});
        cui_invalidate(ui);
    }
}
void app_stream_destination(cui_context *ui,const cui_theme *t,const AppStreamSnapshot *s,bool blocked,
                            AppStreamSubmit submit,void *user)
{
    card(ui,t,"stream-destination-card","Destination");
    const char *labels[]={"Custom","Twitch","YouTube"};const char *values[]={"custom","twitch","youtube"};
    cui_begin_box(ui,0,(cui_box){.width=-1,.row=true,.gap=4});
    for(unsigned i=0;i<3;++i) {
        char name[40];snprintf(name,sizeof name,"stream-service-%s",values[i]);
        if(cui_button_ex(ui,cui_id_from(ui,name),t,cui_string(labels[i]),(cui_button_options){
            .variant=CUI_BUTTON_SECONDARY,.width=-1,.height=30,.selected=!strcmp(s->service,values[i]),
            .disabled=blocked||!s->connected||s->active||s->phase==APP_STREAM_STOPPING})&&submit)
            submit(user,APP_STREAM_SERVICE,values[i]);
    }
    cui_end_box(ui);
    field(ui,t,s,blocked,"stream-server-url","Server URL",APP_STREAM_SERVER_URL,s->server_url,submit,user);
    field(ui,t,s,blocked,"stream-key-file","Stream-key file path",APP_STREAM_KEY_FILE,s->key_file,submit,user);
    note(ui,t,"Press Enter to apply a field. Stop the stream to edit its destination.");
    note(ui,t,"The key is read from its file at start. Its contents never appear here.");
    note(ui,t,"Start paused is always on. Revealing is a separate action.");
    cui_end_box(ui);
}
typedef struct {bool confirm;uint64_t generation;} StreamConfirmation;
static void action(cui_context *ui,const cui_theme *t,const AppStreamSnapshot *s,bool blocked,
                   const char *id,const char *label,const char *command,AppStreamSubmit submit,void *user)
{
    if(cui_button_ex(ui,cui_id_from(ui,id),t,cui_string(label),(cui_button_options){
        .variant=!strcmp(command,"start")||!strcmp(command,"resume")?CUI_BUTTON_PRIMARY:CUI_BUTTON_SECONDARY,
        .width=-1,.height=36,.disabled=!app_stream_action_allowed(s,blocked,command)})&&submit) {
        submit(user,APP_STREAM_ACTION,command);cui_invalidate(ui);
    }
}
void app_stream_output(cui_context *ui,const cui_theme *t,const AppStreamSnapshot *s,bool blocked,
                       AppStreamSubmit submit,void *user)
{
    cui_id id=cui_id_from(ui,"stream-confirmation-state");
    StreamConfirmation *confirmation=cui_resource_get(ui,id,UINT64_C(0x7374726d636f6e66),sizeof *confirmation,NULL,NULL);
    if(!confirmation)return;
    if(confirmation->generation!=s->generation||!s->connected||s->active)confirmation->confirm=false;
    confirmation->generation=s->generation;
    card(ui,t,"stream-output-card","Streaming");
    const char *phase[]={"Off","Connecting in solid pause…","Connected","Reconnecting…","Stopping…","Failed"};
    note(ui,t,!s->connected?"Not connected":s->phase>=APP_STREAM_STOPPED&&s->phase<=APP_STREAM_FAILED?phase[s->phase]:"Unavailable");
    if(s->phase==APP_STREAM_STREAMING)note(ui,t,s->paused?"Sending the pause screen and silent audio.":s->frozen?"Frozen presentation":s->blurred?"Blurred presentation":"Revealed to the destination");
    if(s->error[0])note(ui,t,s->error);
    if(!s->active&&(s->phase==APP_STREAM_STOPPED||s->phase==APP_STREAM_FAILED)) {
        if(!confirmation->confirm) {
            if(cui_button_ex(ui,cui_id_from(ui,"stream-go-live"),t,cui_string(s->phase==APP_STREAM_FAILED?"Retry connection…":"Go live…"),
                (cui_button_options){.variant=CUI_BUTTON_PRIMARY,.width=-1,.height=36,
                    .disabled=!app_stream_action_allowed(s,blocked,"start")||!s->server_url[0]||!s->key_file[0]})) {
                confirmation->confirm=true;cui_invalidate(ui);
            }
        } else {
            note(ui,t,"Connect to the stream? The destination receives solid pause first.");
            action(ui,t,s,blocked,"stream-connect-paused","Connect paused","start",submit,user);
            if(cui_button_ex(ui,cui_id_from(ui,"stream-confirm-cancel"),t,cui_string("Cancel"),
                (cui_button_options){.variant=CUI_BUTTON_SECONDARY,.width=-1,.height=32})) {
                confirmation->confirm=false;cui_invalidate(ui);
            }
        }
        if(s->phase==APP_STREAM_FAILED)action(ui,t,s,blocked,"stream-dismiss","Dismiss","stop",submit,user);
    } else if(s->active) {
        if(s->phase==APP_STREAM_STREAMING)
            action(ui,t,s,blocked,s->paused?"stream-reveal":"stream-pause",s->paused?"Reveal to viewers":"Pause",s->paused?"resume":"pause",submit,user);
        action(ui,t,s,blocked,"stream-stop",s->phase==APP_STREAM_CONNECTING?"Cancel connection":"Stop","stop",submit,user);
    }
    note(ui,t,"Service buffering can delay the public broadcast ending.");
    cui_end_box(ui);
    card(ui,t,"stream-network-card","Network");
    char text[128];
    double bitrate=isfinite(s->outgoing_bitrate_kbps)&&s->outgoing_bitrate_kbps>=0?s->outgoing_bitrate_kbps:0;
    snprintf(text,sizeof text,"Outgoing bitrate  %.0f kbps",bitrate);note(ui,t,text);
    snprintf(text,sizeof text,"Reconnect attempt  %u / %d",s->retry_attempt,s->reconnect_attempts);note(ui,t,text);
    snprintf(text,sizeof text,"Queue  %u frames · Dropped  %llu",s->queue_depth,(unsigned long long)s->dropped_frames);note(ui,t,text);
    action(ui,t,s,blocked,"stream-refresh","Refresh status","status",submit,user);
    note(ui,t,"Local packet statistics do not prove a viewer watched.");
    cui_end_box(ui);
}
void app_stream_encoding(cui_context *ui,const cui_theme *t,const AppStreamSnapshot *s,bool blocked,
                         AppStreamSubmit submit,void *user)
{
    card(ui,t,"stream-encoding-card","Quality and reconnect");
    char value[32];
#define NUMBER(name,label,intent,member) do {snprintf(value,sizeof value,"%d",s->member);field(ui,t,s,blocked,name,label,intent,value,submit,user);}while(0)
    NUMBER("stream-video-bitrate","Video bitrate · kbps",APP_STREAM_VIDEO_BITRATE,video_bitrate_kbps);
    NUMBER("stream-audio-bitrate","Audio bitrate · kbps",APP_STREAM_AUDIO_BITRATE,audio_bitrate_kbps);
    field(ui,t,s,blocked,"stream-encoder-preset","Encoder preset",APP_STREAM_ENCODER_PRESET,s->encoder_preset,submit,user);
    NUMBER("stream-reconnect-attempts","Reconnect attempts",APP_STREAM_RECONNECT_ATTEMPTS,reconnect_attempts);
    NUMBER("stream-reconnect-initial","Initial retry delay · ms",APP_STREAM_RECONNECT_INITIAL,reconnect_initial_ms);
    NUMBER("stream-reconnect-max","Maximum retry delay · ms",APP_STREAM_RECONNECT_MAX,reconnect_max_ms);
#undef NUMBER
    note(ui,t,"Keyframe interval  2 seconds · controlled by the encoder");
    note(ui,t,"Encoding changes apply after stopping the stream.");
    cui_end_box(ui);
}
