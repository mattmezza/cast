/* MIT licensed. Portal objects and PipeWire objects never escape this module. */
#include "cast.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SCREENCAST "org.freedesktop.portal.ScreenCast"
typedef struct {
 GDBusConnection *bus; GCancellable *cancel;
 char *session,*request; guint response_sub,closed_sub,owner_sub;
 int stage,pending; bool shutting,failed; uint32_t cursor_mode,source_types,node;
 uint64_t serial; char error[CAST_ERR]; Config config;
 struct pw_main_loop *loop; struct pw_context *context; struct pw_core *core;
 struct pw_stream *stream; struct spa_hook listener;
 struct spa_video_info_raw format; Frame latest; Cursor cursor;
} Wayland;
static void begin_request(Wayland *w,int stage);
static void fail(Wayland *w,const char *why) {
 w->failed=true; snprintf(w->error,sizeof w->error,"%s",why);
 frame_free(&w->latest); w->cursor.valid=false;
}
static void stop_pw(Wayland *w) {
 if(w->stream) pw_stream_destroy(w->stream);
 if(w->core) pw_core_disconnect(w->core);
 if(w->context) pw_context_destroy(w->context);
 if(w->loop) pw_main_loop_destroy(w->loop);
 w->stream=NULL;w->core=NULL;w->context=NULL;w->loop=NULL;
 frame_free(&w->latest); memset(&w->format,0,sizeof w->format);w->cursor.valid=false;
}
/* Accept only explicitly negotiated packed CPU formats. Never dereference DMA-BUF. */
static int copy_pixels(Wayland *w,const struct spa_data *d) {
 unsigned width=w->format.size.width,height=w->format.size.height;
 int channels=(w->format.format==SPA_VIDEO_FORMAT_RGB || w->format.format==SPA_VIDEO_FORMAT_BGR)?3:4;
 bool bgr=w->format.format==SPA_VIDEO_FORMAT_BGRA || w->format.format==SPA_VIDEO_FORMAT_BGRx || w->format.format==SPA_VIDEO_FORMAT_BGR;
 if(!d->data || !d->chunk || !width || !height || width>8192 || height>8192) return -1;
 if(d->type!=SPA_DATA_MemPtr && d->type!=SPA_DATA_MemFd) return -1;
 if(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) return -1;
 int stride=d->chunk->stride?d->chunk->stride:(int)width*channels;
 size_t abs_stride=(size_t)(stride<0?-(int64_t)stride:stride);
 size_t needed=(height-1)*abs_stride+(size_t)width*channels;
 if(abs_stride<(size_t)width*channels || d->chunk->offset>d->maxsize || needed>d->maxsize-d->chunk->offset || needed>d->chunk->size) return -1;
 if(frame_alloc(&w->latest,(int)width,(int)height)<0) return -1;
 const uint8_t *base=(const uint8_t*)d->data+d->chunk->offset;
 if(stride<0) base+=(height-1)*abs_stride;
 for(unsigned y=0;y<height;y++) {
  const uint8_t *src=base+(ptrdiff_t)y*stride;uint8_t *dst=w->latest.data+(size_t)y*w->latest.stride;
  for(unsigned x=0;x<width;x++,src+=channels,dst+=4) {dst[0]=src[bgr?2:0];dst[1]=src[1];dst[2]=src[bgr?0:2];dst[3]=255;}
 }
 w->latest.ts_ns=cast_now_ns();return 0;
}
static void on_process(void *data) {
 Wayland *w=data;struct pw_buffer *b,*newest=NULL;
 /* Keep only the newest frame, always return every dequeued buffer. */
 while((b=pw_stream_dequeue_buffer(w->stream))) {if(newest)pw_stream_queue_buffer(w->stream,newest);newest=b;}
 if(!newest)return;
 struct spa_buffer *buf=newest->buffer;
 if(!w->failed && buf->n_datas && copy_pixels(w,&buf->datas[0])==0) {
  struct spa_meta_cursor *m=spa_buffer_find_meta_data(buf,SPA_META_Cursor,sizeof *m);
  if(m) {w->cursor.valid=spa_meta_cursor_is_valid(m);w->cursor.x=m->position.x;w->cursor.y=m->position.y;w->cursor.ts_ns=w->latest.ts_ns;}
 } else {frame_free(&w->latest);snprintf(w->error,sizeof w->error,"Wayland stream delivered invalid/unmappable frame; waiting for valid CPU buffer");}
 pw_stream_queue_buffer(w->stream,newest);
}
static void on_format(void *data,uint32_t id,const struct spa_pod *param) {
 Wayland *w=data;if(id!=SPA_PARAM_Format)return;
 frame_free(&w->latest);w->cursor.valid=false;memset(&w->format,0,sizeof w->format);
 if(!param)return;
 if(spa_format_video_raw_parse(param,&w->format)<0) {fail(w,"Wayland stream format negotiation failed");return;}
 switch(w->format.format) {case SPA_VIDEO_FORMAT_RGB:case SPA_VIDEO_FORMAT_BGR:case SPA_VIDEO_FORMAT_RGBx:case SPA_VIDEO_FORMAT_BGRx:case SPA_VIDEO_FORMAT_RGBA:case SPA_VIDEO_FORMAT_BGRA:break;default:fail(w,"Wayland compositor did not negotiate a supported CPU pixel format");return;}
 uint8_t raw[1024];struct spa_pod_builder b=SPA_POD_BUILDER_INIT(raw,sizeof raw);const struct spa_pod *p[3];
 p[0]=spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_ParamBuffers,SPA_PARAM_Buffers,
  SPA_PARAM_BUFFERS_dataType,SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemPtr)|(1<<SPA_DATA_MemFd)));
 p[1]=spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_ParamMeta,SPA_PARAM_Meta,
  SPA_PARAM_META_type,SPA_POD_Id(SPA_META_Cursor),SPA_PARAM_META_size,SPA_POD_Int(sizeof(struct spa_meta_cursor)+sizeof(struct spa_meta_bitmap)+256*256*4));
 p[2]=spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_ParamMeta,SPA_PARAM_Meta,
  SPA_PARAM_META_type,SPA_POD_Id(SPA_META_Header),SPA_PARAM_META_size,SPA_POD_Int(sizeof(struct spa_meta_header)));
 pw_stream_update_params(w->stream,p,3);
}
static void on_stream_state(void *data,enum pw_stream_state old,enum pw_stream_state state,const char *error) {
 (void)old;Wayland *w=data;
 if(state==PW_STREAM_STATE_ERROR) fail(w,error?error:"Wayland stream error/permission revoked");
 if(state==PW_STREAM_STATE_UNCONNECTED && w->stage==4)fail(w,"Wayland stream disconnected; capture monitor requests fresh portal consent");
}
static const struct pw_stream_events stream_events={PW_VERSION_STREAM_EVENTS,.state_changed=on_stream_state,.param_changed=on_format,.process=on_process};
static int start_pw(Wayland *w,int fd) {
 pw_init(NULL,NULL);w->loop=pw_main_loop_new(NULL);
 if(!w->loop){close(fd);return -1;}
 w->context=pw_context_new(pw_main_loop_get_loop(w->loop),NULL,0);
 if(!w->context){close(fd);return -1;}
 w->core=pw_context_connect_fd(w->context,fd,NULL,0); /* owns fd even on failure */
 if(!w->core)return -1;
 struct pw_properties *props=pw_properties_new(PW_KEY_MEDIA_TYPE,"Video",PW_KEY_MEDIA_CATEGORY,"Capture",PW_KEY_MEDIA_ROLE,"Screen",NULL);
 if(w->serial)pw_properties_setf(props,PW_KEY_TARGET_OBJECT,"%llu",(unsigned long long)w->serial);
 w->stream=pw_stream_new(w->core,"cast-screen",props);if(!w->stream)return -1;
 pw_stream_add_listener(w->stream,&w->listener,&stream_events,w);
 uint8_t raw[1024];struct spa_pod_builder b=SPA_POD_BUILDER_INIT(raw,sizeof raw);
 const struct spa_pod *p=spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_Format,SPA_PARAM_EnumFormat,
 SPA_FORMAT_mediaType,SPA_POD_Id(SPA_MEDIA_TYPE_video),SPA_FORMAT_mediaSubtype,SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
 SPA_FORMAT_VIDEO_format,SPA_POD_CHOICE_ENUM_Id(7,SPA_VIDEO_FORMAT_BGRx,SPA_VIDEO_FORMAT_BGRx,SPA_VIDEO_FORMAT_RGBx,SPA_VIDEO_FORMAT_BGRA,SPA_VIDEO_FORMAT_RGBA,SPA_VIDEO_FORMAT_RGB,SPA_VIDEO_FORMAT_BGR),
 SPA_FORMAT_VIDEO_size,SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1920,1080),&SPA_RECTANGLE(1,1),&SPA_RECTANGLE(8192,8192)),
 SPA_FORMAT_VIDEO_framerate,SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(w->config.fps,1),&SPA_FRACTION(0,1),&SPA_FRACTION(240,1)));
 return pw_stream_connect(w->stream,PW_DIRECTION_INPUT,w->serial?PW_ID_ANY:w->node,PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS,&p,1);
}
static void remote_done(GObject *object,GAsyncResult *result,gpointer data) {
 Wayland *w=data;GError *error=NULL;GUnixFDList *fds=NULL;
 GVariant *reply=g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(object),&fds,result,&error);w->pending--;
 if(w->shutting)goto done;
 if(!reply){fail(w,error?error->message:"OpenPipeWireRemote failed");goto done;}
 gint index;g_variant_get(reply,"(h)",&index);int fd=g_unix_fd_list_get(fds,index,&error);
 if(fd<0 || start_pw(w,fd)<0) {fail(w,error?error->message:"Cannot connect portal PipeWire stream");stop_pw(w);}else w->stage=4;
 done:if(reply)g_variant_unref(reply);if(fds)g_object_unref(fds);if(error)g_error_free(error);
}
static void session_closed(GDBusConnection *bus,const gchar *sender,const gchar *path,const gchar *interface,const gchar *signal,GVariant *parameters,gpointer data) {
 (void)bus;(void)sender;(void)path;(void)interface;(void)signal;(void)parameters;
 fail(data,"Wayland portal session closed or permission revoked; capture monitor requests fresh consent");
}
static void owner_changed(GDBusConnection *bus,const gchar *sender,const gchar *path,const gchar *interface,const gchar *signal,GVariant *parameters,gpointer data) {
 (void)bus;(void)sender;(void)path;(void)interface;(void)signal;const char *name,*old,*owner;
 g_variant_get(parameters,"(&s&s&s)",&name,&old,&owner);(void)name;(void)old;
 if(!*owner)fail(data,"ScreenCast portal disappeared; restart cast after repairing the portal service");
}
static void response(GDBusConnection *bus,const gchar *sender,const gchar *path,const gchar *interface,const gchar *signal,GVariant *parameters,gpointer data) {
 (void)bus;(void)sender;(void)interface;(void)signal;Wayland *w=data;
 if(w->shutting || w->failed || !w->request || strcmp(path,w->request))return;
 guint code;GVariant *dict;g_variant_get(parameters,"(u@a{sv})",&code,&dict);
 if(code){fail(w,code==1?"ScreenCast consent cancelled; capture monitor requests fresh consent":"ScreenCast request denied/failed; check desktop portal backend");goto done;}
 if(w->stage==1) {
  const char *session=NULL;
  if(!g_variant_lookup(dict,"session_handle","&s",&session) || !g_variant_is_object_path(session)){fail(w,"Portal returned invalid session handle");goto done;}
  w->session=g_strdup(session);w->closed_sub=g_dbus_connection_signal_subscribe(w->bus,PORTAL,"org.freedesktop.portal.Session","Closed",w->session,NULL,G_DBUS_SIGNAL_FLAGS_NONE,session_closed,w,NULL);begin_request(w,2);
 }else if(w->stage==2)begin_request(w,3);
 else if(w->stage==3) {
  GVariant *streams=g_variant_lookup_value(dict,"streams",G_VARIANT_TYPE("a(ua{sv})"));
  if(!streams || g_variant_n_children(streams)!=1){if(streams)g_variant_unref(streams);fail(w,"Portal did not return exactly one selected stream");goto done;}
  GVariant *props;g_variant_get_child(streams,0,"(u@a{sv})",&w->node,&props);
  g_variant_lookup(props,"pipewire-serial","t",&w->serial);g_variant_unref(props);g_variant_unref(streams);
  w->pending++;g_dbus_connection_call_with_unix_fd_list(w->bus,PORTAL,PORTAL_PATH,SCREENCAST,"OpenPipeWireRemote",
   g_variant_new("(oa{sv})",w->session,NULL),G_VARIANT_TYPE("(h)"),G_DBUS_CALL_FLAGS_NONE,5000,NULL,w->cancel,remote_done,w);
 }
 done:g_variant_unref(dict);
}
static void request_done(GObject *object,GAsyncResult *result,gpointer data) {
 Wayland *w=data;GError *error=NULL;GVariant *reply=g_dbus_connection_call_finish(G_DBUS_CONNECTION(object),result,&error);w->pending--;
 if(!w->shutting && !reply)fail(w,error?error->message:"ScreenCast method failed");
 if(reply)g_variant_unref(reply);
 if(error)g_error_free(error);
}
static void begin_request(Wayland *w,int stage) {
 static unsigned sequence;char token[64];snprintf(token,sizeof token,"cast_%ld_%u",(long)getpid(),++sequence);
 char *sender=g_strdup(g_dbus_connection_get_unique_name(w->bus)+1);for(char *p=sender;*p;p++)if(*p=='.')*p='_';
 g_free(w->request);w->request=g_strdup_printf(PORTAL_PATH "/request/%s/%s",sender,token);g_free(sender);w->stage=stage;
 GVariantBuilder b;g_variant_builder_init(&b,G_VARIANT_TYPE_VARDICT);g_variant_builder_add(&b,"{sv}","handle_token",g_variant_new_string(token));
 const char *method;GVariant *args;
 if(stage==1){method="CreateSession";g_variant_builder_add(&b,"{sv}","session_handle_token",g_variant_new_string(token));args=g_variant_new("(a{sv})",&b);}
 else if(stage==2){method="SelectSources";g_variant_builder_add(&b,"{sv}","types",g_variant_new_uint32(w->source_types&1?1:2));g_variant_builder_add(&b,"{sv}","multiple",g_variant_new_boolean(FALSE));g_variant_builder_add(&b,"{sv}","cursor_mode",g_variant_new_uint32(w->cursor_mode));args=g_variant_new("(oa{sv})",w->session,&b);}
 else {method="Start";args=g_variant_new("(osa{sv})",w->session,"",&b);}
 w->pending++;g_dbus_connection_call(w->bus,PORTAL,PORTAL_PATH,SCREENCAST,method,args,G_VARIANT_TYPE("(o)"),G_DBUS_CALL_FLAGS_NONE,5000,w->cancel,request_done,w);
}
static GVariant *portal_properties(GDBusConnection *bus,GError **error) {
 return g_dbus_connection_call_sync(bus,PORTAL,PORTAL_PATH,"org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)",SCREENCAST),G_VARIANT_TYPE("(a{sv})"),G_DBUS_CALL_FLAGS_NONE,2000,NULL,error);
}
Platform *wayland_open(const Config *c,char *err,size_t n) {
 Wayland *w=calloc(1,sizeof *w);if(!w){snprintf(err,n,"Out of memory");return NULL;}w->config=*c;
 GError *error=NULL;w->bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&error);
 if(!w->bus){snprintf(err,n,"Session D-Bus unavailable: %s",error?error->message:"unknown error");if(error)g_error_free(error);free(w);return NULL;}
 GVariant *reply=portal_properties(w->bus,&error);if(!reply){snprintf(err,n,"ScreenCast portal unavailable: %s",error?error->message:"unknown error");if(error)g_error_free(error);g_object_unref(w->bus);free(w);return NULL;}
 GVariant *dict;g_variant_get(reply,"(@a{sv})",&dict);uint32_t modes=1;g_variant_lookup(dict,"AvailableCursorModes","u",&modes);g_variant_lookup(dict,"AvailableSourceTypes","u",&w->source_types);g_variant_unref(dict);g_variant_unref(reply);
 if(!(w->source_types&3)){snprintf(err,n,"Portal advertises no monitor/window capture");g_object_unref(w->bus);free(w);return NULL;}
 w->cursor_mode=modes&4?4:(c->cursor && modes&2?2:(modes&1?1:(modes&2?2:0)));
 if(!w->cursor_mode || (!c->cursor && w->cursor_mode==2)){snprintf(err,n,"Portal does not advertise the requested hidden cursor capability");g_object_unref(w->bus);free(w);return NULL;}
 if(w->cursor_mode!=4)w->config.zoom_follow=false;
 if(c->keys || c->clicks || c->preview || strcmp(c->capture_kind,"monitor") || c->monitor[0]) {snprintf(err,n,"Wayland requires keys/clicks/preview off, capture monitor, and empty monitor; selection is handled by portal consent");g_object_unref(w->bus);free(w);return NULL;}
 w->cancel=g_cancellable_new();w->response_sub=g_dbus_connection_signal_subscribe(w->bus,PORTAL,"org.freedesktop.portal.Request","Response",NULL,NULL,G_DBUS_SIGNAL_FLAGS_NONE,response,w,NULL);
 w->owner_sub=g_dbus_connection_signal_subscribe(w->bus,"org.freedesktop.DBus","org.freedesktop.DBus","NameOwnerChanged","/org/freedesktop/DBus",PORTAL,G_DBUS_SIGNAL_FLAGS_NONE,owner_changed,w,NULL);
 begin_request(w,1);return (Platform*)w;
}
void wayland_close(Platform *p) {
 Wayland *w=(Wayland*)p;if(!w)return;w->shutting=true;
 if(w->request)g_dbus_connection_call(w->bus,PORTAL,w->request,"org.freedesktop.portal.Request","Close",NULL,NULL,G_DBUS_CALL_FLAGS_NONE,1000,NULL,NULL,NULL);
 if(w->session)g_dbus_connection_call(w->bus,PORTAL,w->session,"org.freedesktop.portal.Session","Close",NULL,NULL,G_DBUS_CALL_FLAGS_NONE,1000,NULL,NULL,NULL);
 if(w->response_sub)g_dbus_connection_signal_unsubscribe(w->bus,w->response_sub);
 if(w->closed_sub)g_dbus_connection_signal_unsubscribe(w->bus,w->closed_sub);
 if(w->owner_sub)g_dbus_connection_signal_unsubscribe(w->bus,w->owner_sub);
 g_cancellable_cancel(w->cancel);
 /* Cancellation guarantees completion; drain callbacks before freeing their userdata. */
 while(w->pending)g_main_context_iteration(NULL,TRUE);
 stop_pw(w);g_free(w->session);g_free(w->request);g_object_unref(w->cancel);g_object_unref(w->bus);free(w);
}
Capabilities wayland_capabilities(Platform *p) {
 Wayland *w=(Wayland*)p;Capabilities c={.capture=true,.cursor_metadata=w->cursor_mode==4,.embedded_cursor=w->cursor_mode==2};
 snprintf(c.description,sizeof c.description,"portal consent; CPU PipeWire capture; cursor %s; global keys/clicks, native preview and interactive region/window selection unsupported",w->cursor_mode==4?"metadata":w->cursor_mode==2?"embedded":"hidden");return c;
}
int wayland_capture(Platform *p,Frame *frame,Cursor *cursor,char *err,size_t n) {
 Wayland *w=(Wayland*)p;
 for(int i=0;i<64 && g_main_context_pending(NULL);i++)g_main_context_iteration(NULL,FALSE);
 if(w->failed){stop_pw(w);snprintf(err,n,"%s",w->error);cursor->valid=false;return -1;}
 if(w->loop)pw_loop_iterate(pw_main_loop_get_loop(w->loop),0);
 if(w->failed || !w->latest.data){snprintf(err,n,"%s",w->failed?w->error:w->stage<4?"Wayland portal consent/selection pending":"Waiting for a valid Wayland frame");cursor->valid=false;return -1;}
 if(cast_now_ns()-w->latest.ts_ns>UINT64_C(2000000000)){snprintf(err,n,"Wayland stream stalled; emitting neutral content until frames return");cursor->valid=false;return -1;}
 *cursor=w->cursor;return frame_copy(frame,&w->latest);
}
int wayland_command(Platform *p,Config *c,int argc,char **argv,char *err,size_t n) {
 Wayland *w=(Wayland*)p;
 if(argc==2 && !strcmp(argv[0],"screen") && !strcmp(argv[1],"list")){snprintf(err,n,"portal (select the monitor/window in the desktop consent dialog)");return 0;}
 if(argc==2 && !strcmp(argv[0],"capture") && !strcmp(argv[1],"monitor")) {
  if(w->pending){snprintf(err,n,"Portal consent already pending; cancel in the portal dialog or quit cast");return -1;}
  if(w->session){g_dbus_connection_call(w->bus,PORTAL,w->session,"org.freedesktop.portal.Session","Close",NULL,NULL,G_DBUS_CALL_FLAGS_NONE,1000,NULL,NULL,NULL);g_free(w->session);w->session=NULL;}
  if(w->closed_sub){g_dbus_connection_signal_unsubscribe(w->bus,w->closed_sub);w->closed_sub=0;}
  stop_pw(w);w->failed=false;w->error[0]=0;w->serial=0;begin_request(w,1);snprintf(c->capture_kind,sizeof c->capture_kind,"monitor");snprintf(err,n,"Portal source selection requested; capture remains neutral pending consent");return 0;
 }
 snprintf(err,n,"Wayland command unsupported: %s; use capture monitor for portal source selection and compositor keybindings for cast controls",argc?argv[0]:"selection");return -1;
}
int wayland_reconfigure(Platform *p,const Config *c,char *err,size_t n) {
 Wayland *w=(Wayland*)p;
 if(c->keys || c->clicks || c->preview){snprintf(err,n,"Wayland global keys/clicks and native preview unsupported");return -1;}
 if(c->zoom_follow && w->cursor_mode!=4){snprintf(err,n,"Portal cursor metadata unavailable: zoom follow unsupported");return -1;}
 if(w->cursor_mode!=4 && c->cursor!=w->config.cursor){snprintf(err,n,"Changing embedded/hidden portal cursor mode requires restart and fresh consent");return -1;}
 if(strcmp(c->capture_kind,"monitor") || c->monitor[0]){snprintf(err,n,"Wayland source selection belongs to portal; use capture monitor");return -1;}
 w->config=*c;return 0;
}
void wayland_doctor(const Config *c,char *out,size_t n) {
 (void)c;GError *error=NULL;GDBusConnection *bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&error);
 if(!bus){snprintf(out,n,"Wayland: session bus unavailable: %s; run cast inside your graphical login",error?error->message:"unknown");if(error)g_error_free(error);return;}
 GVariant *reply=portal_properties(bus,&error);
 if(!reply)snprintf(out,n,"Wayland: ScreenCast portal unavailable: %s; install xdg-desktop-portal and your compositor's portal backend",error?error->message:"unknown");
 else {GVariant *dict;uint32_t modes=1,types=0,version=0;g_variant_get(reply,"(@a{sv})",&dict);g_variant_lookup(dict,"AvailableCursorModes","u",&modes);g_variant_lookup(dict,"AvailableSourceTypes","u",&types);g_variant_lookup(dict,"version","u",&version);snprintf(out,n,"Wayland: ScreenCast v%u source-types=0x%x cursor-modes=0x%x; capture requires interactive consent (doctor does not request it)",version,types,modes);g_variant_unref(dict);g_variant_unref(reply);}
 if(error)g_error_free(error);
 g_object_unref(bus);
}
