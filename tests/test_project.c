#include "project.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_text(const char *path,const char *text)
{
    FILE *file=fopen(path,"w"); assert(file); assert(fputs(text,file)>=0); assert(!fclose(file));
}
static void rejected(const char *path,const char *text,Config *config,CastTimeline *timeline)
{
    Config before=*config; CastTimeline previous=*timeline;
    CastProjectReport report={.missing_media_count=47}, old_report=report;
    char error[CAST_ERR]; write_text(path,text);
    assert(cast_project_load(path,config,timeline,&report,error,sizeof error));
    assert(!memcmp(&before,config,sizeof before));
    assert(!memcmp(&previous,timeline,sizeof previous));
    assert(!memcmp(&old_report,&report,sizeof report));
}
int main(void)
{
    char dir[]="/tmp/cast-project-test-XXXXXX"; assert(mkdtemp(dir));
    char path[PATH_MAX], media_path[PATH_MAX], autosave[PATH_MAX], error[CAST_ERR];
    snprintf(path,sizeof path,"%s/session.cast",dir);
    snprintf(media_path,sizeof media_path,"%s/take with spaces.mkv",dir);
    snprintf(autosave,sizeof autosave,"%s/session.recovery.cast",dir);
    write_text(media_path,"media placeholder; no decoding performed");
    Config source,loaded; config_defaults(&source); config_defaults(&loaded);
    strcpy(source.layout,"split"); source.split_ratio=37.5; source.mic_gain=1.75;
    strcpy(source.text_content,"Line one\nLine two café");
    strcpy(source.socket_path,"/secret-source-socket");
    strcpy(source.stream.server_url,"rtmps://hidden-host/private-route");
    strcpy(source.stream.key_file,"/secret-key-path");
    strcpy(source.licensing_file,"/secret-license-identity");
    strcpy(loaded.socket_path,"/local-socket-preserved");
    strcpy(loaded.stream.key_file,"/local-key-preserved");
    loaded.virtual_enabled=false;
    CastTimeline timeline,opened; timeline_init(&timeline); timeline_init(&opened);
    uint64_t media_id,clip_id;
    assert(!timeline_add_media(&timeline,media_path,CAST_TIMELINE_VIDEO,UINT64_C(10000000000),&media_id,error,sizeof error));
    assert(!timeline_add_clip(&timeline,media_id,0,0,UINT64_C(1000000000),UINT64_C(2000000000),&clip_id,error,sizeof error));
    timeline.clips[0].capture_locked=true;
    assert(!cast_project_save(path,&source,&timeline,error,sizeof error));
    struct stat st; assert(!stat(path,&st)); assert((st.st_mode&0777)==0600);
    FILE *file=fopen(path,"r"); assert(file); char contents[65536]; size_t bytes=fread(contents,1,sizeof contents-1,file); contents[bytes]=0; fclose(file);
    assert(strstr(contents,"CAST_PROJECT 1\n"));
    assert(!strstr(contents,"stream.key_file") && !strstr(contents,"stream.server_url"));
    assert(!strstr(contents,"licensing.file") && !strstr(contents,"ipc.socket"));
    assert(!strstr(contents,"output.enabled") && !strstr(contents,"transcription.enabled"));
    CastProjectReport report;
    assert(!cast_project_load(path,&loaded,&opened,&report,error,sizeof error));
    assert(!strcmp(loaded.layout,"split") && loaded.split_ratio==37.5 && loaded.mic_gain==1.75);
    assert(!strcmp(loaded.text_content,source.text_content));
    assert(!strcmp(loaded.socket_path,"/local-socket-preserved"));
    assert(!strcmp(loaded.stream.key_file,"/local-key-preserved") && !loaded.virtual_enabled);
    assert(loaded.preset_count==source.preset_count);
    assert(opened.media_count==1 && opened.clip_count==1 && !opened.clips[0].capture_locked);
    assert(opened.clips[0].source_in_ns==UINT64_C(1000000000));
    assert(!strcmp(opened.media[0].path,media_path) && !report.missing_media_count);
    assert(!unlink(media_path));
    assert(!cast_project_load(path,&loaded,&opened,&report,error,sizeof error));
    assert(report.missing_media_count==1 && report.missing_media_ids[0]==media_id && opened.media[0].missing);
    assert(!cast_project_autosave(autosave,&source,&timeline,error,sizeof error));
    Config restored; config_defaults(&restored);
    assert(!cast_project_load(autosave,&restored,&opened,NULL,error,sizeof error));
    assert(!strcmp(restored.layout,"split"));
    /* Failed validation never truncates the previously committed document. */
    source.width=0;
    assert(cast_project_save(autosave,&source,&timeline,error,sizeof error));
    assert(!cast_project_load(autosave,&restored,&opened,NULL,error,sizeof error));
    source.width=1920;
    assert(!unlink(path)); assert(!symlink(autosave,path));
    assert(cast_project_save(path,&source,&timeline,error,sizeof error));
    assert(cast_project_load(path,&restored,&opened,NULL,error,sizeof error));
    assert(!unlink(path));
    assert(!mkdir(path,0700));
    assert(cast_project_save(path,&source,&timeline,error,sizeof error));
    assert(cast_project_load(path,&restored,&opened,NULL,error,sizeof error));
    assert(!rmdir(path));
    rejected(path,"CAST_PROJECT 99\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\nS output.width 00\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\nS output.enabled 74727565\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\nS output.width 31393230\nS output.width 31393230\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 3 0\nM 1 0 10 78\nC 2 1 0 0 11 0 1 0\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 3 0\nM 1 0 10 78\nC 2 1 -1 0 1 0 1 0\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 3 0\nM 1 0 10 78\nC 2 1 0 0 1 0 nan 0\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\nEND\nTRAILING\n",&loaded,&opened);
    /* Preset records bypass INI parsing; final schema validation must reject
     * invalid composition data atomically, before a preset can be applied. */
    rejected(path,"CAST_PROJECT 1\nT 1 0\nS composition.preset_order -\nP 78 626164 - - - 0 0 0 0 1\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\nS composition.preset_order -\nP 78 - - - - 0 99 0 0 8\nEND\n",&loaded,&opened);
    rejected(path,"CAST_PROJECT 1\nT 1 0\nS composition.preset_order -\nP 78 - - - - 0 0 0 0 256\nEND\n",&loaded,&opened);
    /* Oversized regular input is rejected before parsing or allocation growth. */
    int fd=open(path,O_WRONLY|O_TRUNC); assert(fd>=0); assert(!ftruncate(fd,CAST_PROJECT_LIMIT+1)); close(fd);
    assert(cast_project_load(path,&loaded,&opened,NULL,error,sizeof error));
    assert(!unlink(path)); assert(!unlink(autosave)); assert(!rmdir(dir));
    puts("Project roundtrip, media references, recovery, malformed input and atomic safety passed");
    return 0;
}
