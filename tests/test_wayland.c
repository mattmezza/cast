/* Exercise private portal buffer handling without a compositor or permissions. */
#include <assert.h>
#include "../src/wayland.c"
int main(void) {
 Wayland w={0};w.format.size=SPA_RECTANGLE(2,2);w.format.format=SPA_VIDEO_FORMAT_BGRx;
 uint8_t pixels[]={3,2,1,0,6,5,4,0,9,8,7,0,12,11,10,0};
 struct spa_chunk chunk={.offset=0,.size=16,.stride=8};
 struct spa_data d={.type=SPA_DATA_MemPtr,.data=pixels,.maxsize=16,.chunk=&chunk};
 assert(copy_pixels(&w,&d)==0);assert(w.latest.data[0]==1 && w.latest.data[1]==2 && w.latest.data[2]==3 && w.latest.data[3]==255);
 chunk.stride=-8;assert(copy_pixels(&w,&d)==0);assert(w.latest.data[0]==7);
 chunk.stride=8;chunk.offset=1;assert(copy_pixels(&w,&d)<0);chunk.offset=0;
 chunk.size=15;assert(copy_pixels(&w,&d)<0);chunk.size=16;
 d.type=SPA_DATA_DmaBuf;assert(copy_pixels(&w,&d)<0);d.type=SPA_DATA_MemPtr;
 chunk.flags=SPA_CHUNK_FLAG_CORRUPTED;assert(copy_pixels(&w,&d)<0);chunk.flags=0;
 w.cursor.valid=true;session_closed(NULL,NULL,NULL,NULL,NULL,NULL,&w);
 assert(w.failed && !w.latest.data && !w.cursor.valid);
 assert(strstr(w.error,"revoked"));
 w.failed=false;owner_changed(NULL,NULL,NULL,NULL,NULL,g_variant_new("(sss)",PORTAL,":1.2",""),&w);assert(w.failed);
 w.cursor_mode=4;Capabilities cap=wayland_capabilities((Platform*)&w);
 assert(cap.capture && cap.cursor_metadata && !cap.embedded_cursor && !cap.input && !cap.preview);
 w.cursor_mode=2;cap=wayland_capabilities((Platform*)&w);assert(cap.embedded_cursor && !cap.cursor_metadata);
 Config c={0};char error[1024];c.zoom_follow=true;assert(wayland_reconfigure((Platform*)&w,&c,error,sizeof error)<0);
 char *args[]={"capture","region","select"};assert(wayland_command((Platform*)&w,&c,3,args,error,sizeof error)<0);
 puts("Wayland buffer, revocation and capability tests passed");return 0;
}
