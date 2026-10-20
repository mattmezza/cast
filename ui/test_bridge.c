#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "daemon_bridge.h"
#include "../src/panel_transport.h"
static PanelSnapshot snap;
static int calls, mode, settings, setting_fail;
static char setting_key[64], setting_value[32];
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
{ (void)c; *s=snap; if ((mode==1 || mode==3) && (++calls%2)==0) { if(mode==1)s->privacy_epoch++;else s->daemon_generation++; } return mode!=2; }
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
 mode=0;bridge_poll(b,1,&live);assert(live.connected && live.has_frame && live.target==1);
 mode=4;bridge_poll(b,2,&live);assert(live.connected && !live.has_frame);
 mode=0;bridge_poll(b,1,&live);assert(live.has_frame);
 snap.connected=false;strcpy(snap.error,"panel protocol/build or ownership mismatch");bridge_poll(b,1,&live);assert(!live.connected && !live.has_frame);assert(!strcmp(live.error,snap.error));snap.error[0]=0;
 snap.connected=true;bridge_poll(b,2,&live);assert(live.has_frame);
 mode=2;bridge_poll(b,2,&live);assert(!live.has_frame);
 bridge_poll(b,3,&live);assert(!live.has_frame && !live.connected);
 /* Edits require a connected, idle queue and exact allowlisted values. */
 mode=0; snap.connected=true; char error[256];
 assert(bridge_edit(b,APP_EDIT_LAYOUT,"split",error,sizeof error)==0);
 assert(!strcmp(setting_key,"composition.layout") && !strcmp(setting_value,"split"));
 assert(settings==1);
 bridge_poll(b,0,&live); assert(live.pending && !live.edit_error[0]);
 assert(bridge_edit(b,APP_EDIT_FIT,"cover",error,sizeof error)<0 && settings==1);
 mode=2; bridge_poll(b,0,&live); assert(live.pending && !live.has_frame);
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
 bridge_close(b);bridge_poll(NULL,0,&live);assert(live.enabled && !live.connected && !live.has_frame);
}
