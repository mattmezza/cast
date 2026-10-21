#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "daemon_bridge.h"
#include "../src/panel_transport.h"
static PanelSnapshot snap;
static int calls, mode, settings, setting_fail;
static char setting_key[64], setting_value[4096];
int panel_client_command(PanelClient *client, int argc, const char *const *argv, char *error, size_t size)
{
    (void)client; (void)error; (void)size;
    assert(argc >= 2 && argc <= 4);
    ++settings;
    snprintf(setting_key, sizeof setting_key, "%s %s", argv[0], argv[1]);
    snprintf(setting_value, sizeof setting_value, "%.31s", argc >= 3 ? argv[argc-1] : "");
    if (setting_fail) return -1;
    ++snap.command_queued;
    return 0;
}
int panel_client_setting(PanelClient *client, const char *key, const char *value, char *error, size_t size)
{
    (void)client;
    ++settings;
    snprintf(setting_key, sizeof setting_key, "%s", key);
    snprintf(setting_value, sizeof setting_value, "%s", value);
    if (setting_fail) { snprintf(error, size, "secret-value"); return -1; }
    ++snap.command_queued;
    return 0;
}
PanelClient *panel_client_open(const Config *c, uint64_t w, char *e, size_t n)
{ (void)c;(void)w;(void)e;(void)n;return (PanelClient *)1; }
void panel_client_close(PanelClient *c) { (void)c; }
bool panel_client_snapshot(PanelClient *c, PanelSnapshot *s)
{
    (void)c;
    *s=snap;
    int phase=++calls;
    if ((mode==1 || mode==3) && phase%2==0) {
        if(mode==1)s->privacy_epoch++;else s->daemon_generation++;
    }
    return mode!=2 && !(mode==5 && phase%2==0);
}

int panel_client_frame(PanelClient *c,int t,Frame *f,char *e,size_t n)
{ (void)c;assert(t==3);(void)e;(void)n; if(mode==4)return 0; if(!f->data)f->data=malloc(16);f->width=f->height=2;f->stride=8;return 1; }
void frame_free(Frame *f) { free(f->data);memset(f,0,sizeof *f); }
int main(void)
{
 Config c={0}; AppLive live; DaemonBridge *b=bridge_open(&c,0,NULL,0);
 snap.connected=true;snap.daemon_generation=1;snap.privacy_epoch=1;
 strcpy(snap.config.stream.server_url,"rtmp://secret-key");
 bridge_poll(b,0,&live); assert(live.connected && live.has_frame && live.frame.pixels);
 assert(!strstr(live.error,"secret"));
 strcpy(snap.composition_error,"Camera unavailable; waiting for fresh webcam frames.");
 strcpy(snap.state.last_error,"rtmp://secret-key");
 bridge_poll(b,0,&live);assert(live.has_frame && !strcmp(live.error,snap.composition_error));
 snap.composition_error[0]=0;bridge_poll(b,0,&live);assert(!live.error[0]);
 mode=1;calls=0;bridge_poll(b,0,&live);assert(!live.has_frame);
 mode=3;calls=0;bridge_poll(b,0,&live);assert(!live.has_frame);
 mode=2;bridge_poll(b,0,&live);assert(live.snapshot_busy && !live.connected && !live.has_frame);
 mode=0;bridge_poll(b,1,&live);assert(live.connected && live.has_frame && live.target==1);
 mode=4;bridge_poll(b,2,&live);assert(live.connected && !live.has_frame);
 mode=0;bridge_poll(b,1,&live);assert(live.has_frame);
 snap.connected=false;strcpy(snap.error,"panel protocol/build or ownership mismatch");bridge_poll(b,1,&live);assert(!live.connected && !live.has_frame);assert(!strcmp(live.error,snap.error));snap.error[0]=0;
 snap.connected=true;bridge_poll(b,2,&live);assert(live.has_frame);
 mode=2;bridge_poll(b,2,&live);
 assert(live.connected && live.snapshot_busy && !live.has_frame && !live.frame.pixels);
 assert(live.generation==snap.daemon_generation);
 mode=5;calls=0;bridge_poll(b,2,&live);
 assert(live.connected && live.snapshot_busy && !live.has_frame && !live.frame.pixels);
 mode=0;bridge_poll(b,2,&live);assert(live.connected && !live.snapshot_busy && live.has_frame);
 snap.connected=false;bridge_poll(b,2,&live);assert(!live.connected && !live.snapshot_busy);
 mode=2;bridge_poll(b,2,&live);assert(!live.connected && live.snapshot_busy && !live.has_frame);
 mode=0;snap.connected=true;
 bridge_poll(b,3,&live);assert(!live.has_frame && !live.connected);
 /* Edits require a connected, idle queue and exact allowlisted values. */
 mode=0; snap.connected=true; char error[256];
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"split",error,sizeof error)==0);
 assert(!strcmp(setting_key,"composition.layout") && !strcmp(setting_value,"split"));
 assert(settings==1);
 bridge_poll(b,0,&live); assert(live.pending && !live.edit_error[0]);
 assert(bridge_edit(b,APP_EDIT_FIT,"cover",error,sizeof error)<0 && settings==1);
 mode=2; bridge_poll(b,0,&live); assert(live.pending && live.connected && live.snapshot_busy && !live.has_frame && !live.frame.pixels);
 mode=0; snap.command_completed=snap.command_queued;
 strcpy(snap.config.layout,"split"); bridge_poll(b,0,&live);
 assert(!live.pending && !live.edit_error[0] && !strcmp(live.layout,"split"));
 assert(bridge_edit(b,APP_EDIT_FIT,"cover",error,sizeof error)==0);
 assert(!strcmp(setting_key,"composition.fit"));
 snap.command_completed=snap.command_queued; snap.command_failed=true;
 strcpy(snap.last_reply,"secret-value"); bridge_poll(b,0,&live);
 assert(!live.pending && live.edit_error[0] && !strstr(live.edit_error,"secret-value"));
 snap.command_failed=false;
 const char *invalid_sizes[]={"", "0", "101", "nan", "inf", "1e2", " 20", "20;quit", "1.2.3"};
 int old_settings=settings;
 for(size_t i=0;i<sizeof invalid_sizes/sizeof *invalid_sizes;++i)
     assert(bridge_edit(b,APP_EDIT_SIZE,invalid_sizes[i],error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_ANCHOR,"center",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"webcam",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_FIT,"stretch",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_NONE,"cover",error,sizeof error)<0);
 assert(settings==old_settings);
 bridge_poll(b,0,&live); assert(live.edit_error[0]);
 assert(bridge_edit(b,APP_EDIT_SIZE,"22.5",error,sizeof error)==0);
 assert(!strcmp(setting_key,"camera.width_percent"));
 snap.connected=false;bridge_poll(b,0,&live);assert(!live.pending && live.edit_error[0]);
 snap.connected=true;snap.command_completed=snap.command_queued;
 assert(bridge_edit(b,APP_EDIT_ANCHOR,"top",error,sizeof error)==0);
 assert(!strcmp(setting_key,"camera.anchor"));
 ++snap.daemon_generation;bridge_poll(b,0,&live);assert(!live.pending && live.edit_error[0]);
 old_settings=settings;bridge_poll(b,0,&live);assert(settings==old_settings);
 snap.command_completed=snap.command_queued;setting_fail=1;
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"overlay",error,sizeof error)<0);
 assert(!strstr(error,"secret-value"));setting_fail=0;
 mode=2;old_settings=settings;
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"overlay",error,sizeof error)<0 && settings==old_settings);
 mode=0;snap.connected=false;
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"overlay",error,sizeof error)<0 && settings==old_settings);
 snap.connected=true;++snap.command_queued;
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"overlay",error,sizeof error)<0 && settings==old_settings);
 /* A failed first discovery must still match the current picker generation. */
 snap.command_completed=snap.command_queued;strcpy(snap.config.backend,"xorg");
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 bridge_poll(b,0,&live);
 assert(live.devices.loading && live.devices.kind==APP_DEVICE_SCREEN && live.devices.generation==live.generation);
 mode=2;bridge_poll(b,0,&live);
 assert(live.connected && live.snapshot_busy && live.pending && live.devices.loading);
 assert(live.devices.generation==live.generation && !live.has_frame);
 mode=0;
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 snap.device_reply_failed=true;bridge_poll(b,0,&live);
 assert(!live.devices.loading && live.devices.error[0] && live.devices.kind==APP_DEVICE_SCREEN);
 assert(live.devices.generation==live.generation);
 snap.device_reply_failed=false;
 /* Discovery replies are durable and must match the exact acknowledged request. */
 snap.command_completed=snap.command_queued;strcpy(snap.config.backend,"xorg");
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 bridge_poll(b,0,&live);assert(live.pending && live.devices.loading);
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"* DP-1 1920x1080+0+0 primary\n  HDMI-1 1280x720+1920+0\n");
 strcpy(snap.config.monitor,"DP-1");bridge_poll(b,0,&live);
 assert(!live.pending && !live.devices.loading && live.devices.count==2);
 assert(live.can_select_monitor && !live.can_reselect_portal && strstr(live.screen_name,"DP-1"));
 assert(bridge_source(b,APP_DEVICE_SCREEN,"unlisted",error,sizeof error)<0);
 assert(!bridge_source(b,APP_DEVICE_SCREEN,"HDMI-1",error,sizeof error));
 assert(!strcmp(setting_key,"screen select") && !strcmp(setting_value,"HDMI-1"));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"/dev/video0 USB Camera formats: MJPG YUYV\n");
 strcpy(snap.config.camera_device,"/dev/video0");snap.config.camera_enabled=true;
 bridge_poll(b,0,&live);assert(live.devices.count==1 && !strcmp(live.camera_name,"USB Camera"));
 assert(!bridge_source(b,APP_DEVICE_CAMERA,"/dev/video0",error,sizeof error));
 assert(!strcmp(setting_key,"camera device"));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_source(b,APP_DEVICE_CAMERA,"",error,sizeof error));
 assert(!strcmp(setting_key,"camera.enabled") && !strcmp(setting_value,"false"));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued-1;
 bridge_poll(b,0,&live);assert(!live.pending && live.devices.error[0] && !live.devices.count);
 assert(bridge_source(b,APP_DEVICE_SCREEN,"DP-1",error,sizeof error)<0);
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"* DP-1 malformed\n");bridge_poll(b,0,&live);assert(live.devices.error[0]);
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"* DP-1 1920x1080+0+0");bridge_poll(b,0,&live);assert(live.devices.error[0]);
 strcpy(snap.config.backend,"macos");
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"123 (main): 2560x1440\n456: 1920x1080\n");bridge_poll(b,0,&live);
 assert(live.devices.count==2 && !strcmp(live.devices.choices[0].id,"123"));
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"AppleCamera:123 Built-in Camera\n");bridge_poll(b,0,&live);
 assert(live.devices.count==1 && !strcmp(live.devices.choices[0].id,"AppleCamera:123"));
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 ++snap.daemon_generation;bridge_poll(b,0,&live);assert(!live.pending && !live.devices.count);
 snap.command_completed=snap.command_queued;
 assert(bridge_source(b,APP_DEVICE_CAMERA,"AppleCamera:123",error,sizeof error)<0);
 strcpy(snap.config.backend,"wayland");bridge_poll(b,0,&live);
 assert(live.can_reselect_portal && !live.can_select_monitor);
 assert(bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error)<0);
 assert(!bridge_source(b,APP_DEVICE_SCREEN,"",error,sizeof error));
 assert(!strcmp(setting_key,"capture monitor"));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 strcpy(snap.config.backend,"macos");
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 size_t uid_size=sizeof snap.config.camera_device-2;
 if(uid_size>2000)uid_size=2000;
 memset(snap.device_reply,'a',uid_size);strcpy(snap.device_reply+uid_size," Long UID Camera\n");
 bridge_poll(b,0,&live);assert(live.devices.count==1 && strlen(live.devices.choices[0].id)==uid_size);
 assert(!bridge_source(b,APP_DEVICE_CAMERA,live.devices.choices[0].id,error,sizeof error));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 memset(snap.device_reply,'x',sizeof snap.device_reply);
 bridge_poll(b,0,&live);assert(!live.devices.count && live.devices.error[0]);
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"123: 100x100\n123: 100x100\n");
 bridge_poll(b,0,&live);assert(!live.devices.count && live.devices.error[0]);
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 size_t used=0;
 for(unsigned i=0;i<APP_DEVICE_MAX+1;++i)
     used+=(size_t)snprintf(snap.device_reply+used,sizeof snap.device_reply-used,"%u: 100x100\n",i+1);
 bridge_poll(b,0,&live);assert(!live.devices.count && live.devices.error[0]);
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.connected=false;bridge_poll(b,0,&live);assert(!live.pending && !live.devices.count);
 /* Startup labels keep acknowledged source IDs; only a valid list proves absence. */
 snap.connected=true;snap.command_completed=snap.command_queued;++snap.daemon_generation;
 snap.config.camera_enabled=false;strcpy(snap.config.monitor,"main");
 bridge_poll(b,0,&live);assert(!strcmp(live.screen_name,"Default display") && !strcmp(live.camera_name,"No camera"));
 strcpy(snap.config.monitor,"not-listed");strcpy(snap.config.camera_device,"device-id");
 snap.config.camera_enabled=true;bridge_poll(b,0,&live);
 assert(!strcmp(live.screen_name,"not-listed") && !strcmp(live.camera_name,"device-id"));
 assert(!bridge_devices(b,APP_DEVICE_SCREEN,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"123: 100x100\n");bridge_poll(b,0,&live);
 assert(strstr(live.screen_name,"not-listed") && strstr(live.screen_name,"unavailable"));
 strcpy(snap.config.backend,"xorg");
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"No accessible V4L2 capture devices. Check device ACLs with getfacl /dev/videoN.\n");
 bridge_poll(b,0,&live);assert(!live.devices.count && !live.devices.error[0]);
 assert(strstr(live.camera_name,"device-id") && strstr(live.camera_name,"unavailable"));
 assert(!bridge_devices(b,APP_DEVICE_CAMERA,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"No accessible V4L2 capture devices. trailing garbage\n");
 bridge_poll(b,0,&live);assert(live.devices.error[0]);
 strcpy(snap.config.monitor,"DP-1\nunsafe");bridge_poll(b,0,&live);
 assert(!strchr(live.screen_name,'\n') && strchr(live.screen_name,'?'));
 /* Look edits are bounded, allowlisted, and never optimistically applied. */
 snap.connected=true;snap.command_completed=snap.command_queued;++snap.daemon_generation;
 snap.config.zoom_min=1;snap.config.zoom_max=8;snap.config.zoom_factor=2;
 strcpy(snap.config.background_source,"screen");strcpy(snap.config.screen_background,"gradient");
 strcpy(snap.config.camera_background,"solid");snap.config.screen_background_color=0x123456;
 snap.config.gradient_from=0xabcdef;snap.config.gradient_to=0x010203;
 snap.config.gradient_angle=135;snap.config.gradient_waypoint=50;snap.config.gradient_via_enabled=true;
 bridge_poll(b,0,&live);
 assert(!strcmp(live.background_source,"screen") && !strcmp(live.screen_background,"gradient"));
 assert(live.screen_background_color==0x123456 && live.gradient_from==0xabcdef);
 assert(live.gradient_angle==135 && live.gradient_via_enabled && live.zoom_factor==2 && live.zoom_max==8);
 assert(!live.pro && !live.cinematic_zoom_available && !live.can_zoom_follow);
 int previous=settings;
 const char *bad_numbers[]={"", ".", "nan", "inf", "2e0", " 2", "2 ", "-2", "+2", "2;quit", "1.2.3", "21"};
 for(unsigned i=0;i<sizeof bad_numbers/sizeof *bad_numbers;++i)
     assert(bridge_edit(b,APP_EDIT_ZOOM_FACTOR,bad_numbers[i],error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_ZOOM_FACTOR,"9",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_ZOOM_MIN,"3",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_ZOOM_MAX,"1",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_ZOOM_FOLLOW,"true",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_GRADIENT_FROM,"#12345g",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_GRADIENT_FROM,"#12345678",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_SCREEN_BACKGROUND,"image",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_BACKGROUND_SOURCE,"wallpaper",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_GRADIENT_ANGLE,"361",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_GRADIENT_WAYPOINT,"0",error,sizeof error)<0);
 assert(settings==previous);
 struct { AppEdit edit; const char *value,*key; } edits[]={
  {APP_EDIT_BACKGROUND_SOURCE,"camera","background.source"},
  {APP_EDIT_SCREEN_BACKGROUND,"blurred","screen.background"},
  {APP_EDIT_CAMERA_BACKGROUND,"solid","camera.background"},
  {APP_EDIT_SCREEN_BACKGROUND_COLOR,"#Ff0011","screen.background_color"},
  {APP_EDIT_CAMERA_BACKGROUND_COLOR,"#ffffff","camera.background_color"},
  {APP_EDIT_GRADIENT_FROM,"#123456","background.gradient_from"},
  {APP_EDIT_GRADIENT_VIA,"#abcdef","background.gradient_via"},
  {APP_EDIT_GRADIENT_TO,"#000000","background.gradient_to"},
  {APP_EDIT_GRADIENT_VIA_ENABLED,"false","background.gradient_via_enabled"},
  {APP_EDIT_GRADIENT_ANGLE,"360","background.gradient_angle"},
  {APP_EDIT_GRADIENT_WAYPOINT,"99","background.gradient_waypoint"},
  {APP_EDIT_ZOOM_MIN,"1","zoom.min"},{APP_EDIT_ZOOM_MAX,"20","zoom.max"},
  {APP_EDIT_ZOOM_FACTOR,"8","zoom set"},{APP_EDIT_ZOOM_FOLLOW,"false","zoom follow"}};
 for(unsigned i=0;i<sizeof edits/sizeof *edits;++i) {
     assert(!bridge_edit(b,edits[i].edit,edits[i].value,error,sizeof error));
     assert(!strcmp(setting_key,edits[i].key));
     assert(bridge_edit(b,APP_EDIT_SCREEN_BACKGROUND,"solid",error,sizeof error)<0);
     bridge_poll(b,0,&live);assert(live.pending && live.zoom_factor==2);
     snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);assert(!live.pending);
 }
 snap.capabilities.cursor_metadata=true;
 assert(!bridge_edit(b,APP_EDIT_ZOOM_FOLLOW,"true",error,sizeof error));
 assert(!strcmp(setting_key,"zoom follow") && !strcmp(setting_value,"on"));
 snap.command_completed=snap.command_queued;snap.command_failed=true;
 bridge_poll(b,0,&live);assert(!live.pending && live.edit_error[0] && !live.zoom_follow);
 snap.command_failed=false;
 strcpy(snap.edition.features[0].id,"cinematic_zoom");snap.edition.features[0].active=true;
 snap.cinematic_active=true;bridge_poll(b,0,&live);
 assert(live.can_zoom_follow && live.cinematic_zoom_available && live.cinematic_zoom_active);
 assert(!bridge_edit(b,APP_EDIT_GRADIENT_FROM,"#123456",error,sizeof error));
 ++snap.daemon_generation;bridge_poll(b,0,&live);assert(!live.pending && live.edit_error[0]);
 snap.command_completed=snap.command_queued;
 assert(!bridge_edit(b,APP_EDIT_ZOOM_FACTOR,"2",error,sizeof error));
 snap.connected=false;bridge_poll(b,0,&live);
 assert(!live.pending && live.edit_error[0] && !live.cinematic_zoom_available && !live.zoom_factor);
 assert(bridge_edit(b,APP_EDIT_ZOOM_FACTOR,"2",error,sizeof error)<0);
 /* Recording lifecycle and audio routing use acknowledged daemon commands. */
 snap.connected=true;snap.command_completed=snap.command_queued;snap.state.recording=false;
 strcpy(snap.config.backend,"xorg");snap.countdown=false;snap.finalizing=false;
 snap.config.mic_gain=.8;snap.config.desktop_gain=1.25;
 strcpy(snap.audio_status,"{\"available\":true,\"mic_ready\":false,\"desktop_ready\":true,\"virtual_ready\":false,\"virtual_silent\":true}");
 strcpy(snap.state.record_path,"/tmp/session.mkv");snap.duration_ns=123456789;
 bridge_poll(b,0,&live);
 assert(live.mic_gain==80 && live.desktop_gain==125 && live.audio_available && live.desktop_ready && !live.mic_ready && live.virtual_silent);
 assert(live.record_duration_ns==123456789 && !strcmp(live.record_path,"/tmp/session.mkv"));
 assert(bridge_edit(b,APP_EDIT_RECORD_ACTION,"cut",error,sizeof error)<0);
 assert(!bridge_edit(b,APP_EDIT_RECORD_ACTION,"start",error,sizeof error));
 assert(!strcmp(setting_key,"record start"));bridge_poll(b,0,&live);assert(!live.recording && live.pending);
 snap.command_completed=snap.command_queued;snap.state.recording=true;bridge_poll(b,0,&live);assert(live.recording && !live.pending);
 assert(bridge_edit(b,APP_EDIT_RECORD_ACTION,"start",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_RECORD_CONTAINER,"mp4",error,sizeof error)<0);
 const char *actions[]={"pause","cut","resume","stop"};
 for(unsigned i=0;i<4;++i) {
   assert(!bridge_edit(b,APP_EDIT_RECORD_ACTION,actions[i],error,sizeof error));
   snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 }
 snap.state.recording=false;snap.countdown=true;
 assert(!bridge_edit(b,APP_EDIT_RECORD_ACTION,"cancel",error,sizeof error));
 snap.command_completed=snap.command_queued;snap.countdown=false;bridge_poll(b,0,&live);
 struct {AppEdit edit;const char *value,*key;} audio_edits[]={
  {APP_EDIT_RECORD_DIRECTORY,"/tmp/a long folder name with spaces for recordings","record.directory"},
  {APP_EDIT_RECORD_CONTAINER,"mp4","record.container"},
  {APP_EDIT_RECORD_VIDEO_CODEC,"auto","record.video_codec"},
  {APP_EDIT_RECORD_AUDIO_CODEC,"aac","record.audio_codec"},
  {APP_EDIT_RECORD_CRF,"51","record.crf"},
  {APP_EDIT_RECORD_COUNTDOWN,"60","record.countdown"},
  {APP_EDIT_AUDIO_MIC,"true","audio mic"},{APP_EDIT_AUDIO_DESKTOP,"false","audio desktop"},
  {APP_EDIT_AUDIO_VIRTUAL,"true","audio virtual"},
  {APP_EDIT_AUDIO_MIC_GAIN,"80","audio mic"},{APP_EDIT_AUDIO_DESKTOP_GAIN,"400","audio desktop"}};
 for(unsigned i=0;i<sizeof audio_edits/sizeof *audio_edits;++i) {
   assert(!bridge_edit(b,audio_edits[i].edit,audio_edits[i].value,error,sizeof error));
   assert(!strcmp(setting_key,audio_edits[i].key));
   if(audio_edits[i].edit==APP_EDIT_AUDIO_MIC_GAIN)assert(!strcmp(setting_value,"80%"));
   snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 }
 previous=settings;
 assert(bridge_edit(b,APP_EDIT_RECORD_ACTION,"overwrite",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_RECORD_CRF,"1.5",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_RECORD_COUNTDOWN,"61",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_AUDIO_MIC_GAIN,"401",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_AUDIO_MIC_GAIN,"80%",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_RECORD_DIRECTORY,"",error,sizeof error)<0);
 assert(bridge_edit(b,APP_EDIT_RECORD_DIRECTORY,"/tmp/unsafe\nfolder",error,sizeof error)<0);
 char oversized[4097];memset(oversized,'x',sizeof oversized-1);oversized[sizeof oversized-1]=0;
 assert(bridge_edit(b,APP_EDIT_RECORD_DIRECTORY,oversized,error,sizeof error)<0);
 assert(settings==previous);
 char longest_directory[4096];memset(longest_directory,'d',sizeof longest_directory-1);longest_directory[4095]=0;
 assert(!bridge_edit(b,APP_EDIT_RECORD_DIRECTORY,longest_directory,error,sizeof error));
 assert(strlen(setting_value)==4095 && !strcmp(setting_value,longest_directory));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_edit(b,APP_EDIT_AUDIO_MIC_GAIN,"200",error,sizeof error));
 bridge_poll(b,0,&live);assert(live.pending && live.mic_gain==80);
 snap.command_completed=snap.command_queued;snap.command_failed=true;
 bridge_poll(b,0,&live);assert(!live.pending && live.edit_error[0] && live.mic_gain==80);
 snap.command_failed=false;
 assert(!bridge_devices(b,APP_DEVICE_MIC,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"alsa_input.usb [id=42 serial=112 class=Audio/Source] USB Microphone (default mic)\nalsa_output.pci [id=43 serial=113 class=Audio/Sink] Speakers\n");
 bridge_poll(b,0,&live);assert(live.devices.count==1 && !strcmp(live.devices.choices[0].id,"alsa_input.usb"));
 assert(!bridge_source(b,APP_DEVICE_MIC,"alsa_input.usb",error,sizeof error));
 assert(!strcmp(setting_key,"audio mic") && !strcmp(setting_value,"alsa_input.usb"));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(bridge_source(b,APP_DEVICE_MIC,"invented",error,sizeof error)<0);
 assert(!bridge_devices(b,APP_DEVICE_DESKTOP,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 bridge_poll(b,0,&live);assert(live.devices.count==2);
 assert(!bridge_source(b,APP_DEVICE_DESKTOP,"alsa_output.pci",error,sizeof error));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_devices(b,APP_DEVICE_MIC,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"malformed\n");bridge_poll(b,0,&live);assert(live.devices.error[0] && !live.devices.count);
 strcpy(snap.config.backend,"macos");bridge_poll(b,0,&live);assert(!live.can_virtual_audio);
 assert(bridge_edit(b,APP_EDIT_AUDIO_VIRTUAL,"true",error,sizeof error)<0);
 assert(!bridge_devices(b,APP_DEVICE_MIC,error,sizeof error));
 snap.command_completed=snap.command_queued;snap.device_reply_id=snap.command_queued;
 strcpy(snap.device_reply,"default [microphone] System Settings > Sound input\ndefault [desktop] ScreenCaptureKit system audio; virtual audio unavailable\n");
 bridge_poll(b,0,&live);assert(live.devices.count==1 && !strcmp(live.devices.choices[0].id,"default"));
 assert(!bridge_source(b,APP_DEVICE_MIC,"default",error,sizeof error));
 snap.command_completed=snap.command_queued;bridge_poll(b,0,&live);
 assert(!bridge_devices(b,APP_DEVICE_DESKTOP,error,sizeof error));
 ++snap.daemon_generation;bridge_poll(b,0,&live);assert(!live.pending && !live.devices.count);
 snap.command_completed=snap.command_queued;
 assert(bridge_source(b,APP_DEVICE_MIC,"default",error,sizeof error)<0);
 bridge_close(b);bridge_poll(NULL,0,&live);assert(live.enabled && !live.connected && !live.has_frame);
}
