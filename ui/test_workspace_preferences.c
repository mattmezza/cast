#define _POSIX_C_SOURCE 200809L
#include "workspace_preferences.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void put(const char *path,const char *data)
{FILE *f=fopen(path,"w");assert(f);assert(fputs(data,f)>=0);assert(!fclose(f));}
int main(void)
{
    const char *temporary=getenv("TMPDIR");if(!temporary||!*temporary)temporary="/tmp";
    char directory[4000];int count=snprintf(directory,sizeof directory,"%s/cast-workspace-pref-XXXXXX",temporary);
    assert(count>0&&(size_t)count<sizeof directory&&mkdtemp(directory));
    char path[4096],error[256];snprintf(path,sizeof path,"%s/local/workspace.ini",directory);
    WorkspacePreferences p,before;workspace_preferences_defaults(&p);before=p;
    assert(workspace_preferences_valid(&p));
    assert(workspace_preferences_load(path,&p,error,sizeof error)==1&&!memcmp(&p,&before,sizeof p));
    p.theme=WORKSPACE_THEME_DARK;p.show_sources=false;p.timeline_stream=true;p.window_width=1280;
    assert(!workspace_preferences_save(path,&p,error,sizeof error));
    struct stat st;assert(!stat(path,&st)&&(st.st_mode&0777)==0600);
    WorkspacePreferences loaded;workspace_preferences_defaults(&loaded);
    assert(!workspace_preferences_load(path,&loaded,error,sizeof error));
    assert(loaded.theme==WORKSPACE_THEME_DARK&&!loaded.show_sources&&loaded.timeline_stream&&loaded.window_width==1280);
    p.theme=WORKSPACE_THEME_LIGHT;assert(!workspace_preferences_save(path,&p,error,sizeof error));
    assert(!workspace_preferences_load(path,&loaded,error,sizeof error)&&loaded.theme==WORKSPACE_THEME_LIGHT);
    before=loaded;
    const char *bad[]={"", "CAST_WORKSPACE 99\n", "CAST_WORKSPACE 1\ntheme 999999999999999999999999999\n", "CAST_WORKSPACE 1\ntheme 0\nsources 2\ninspector 1\ntimeline 0 1 0\nwindow 1440 900\nEND\n", "CAST_WORKSPACE 1\ntheme 0\nsources 1\ninspector 1\ntimeline 0 1 0\nwindow 20 900\nEND\n", "CAST_WORKSPACE 1\ntheme 0\nsources 1\ninspector 1\ntimeline 0 1 0\nwindow 1440 900\nEND\nstream_key secret\n",NULL};
    for(unsigned i=0;bad[i];i++){put(path,bad[i]);assert(workspace_preferences_load(path,&loaded,error,sizeof error)==-1);assert(!memcmp(&loaded,&before,sizeof loaded));}
    assert(!workspace_preferences_save(path,&p,error,sizeof error));
    p.window_width=1;assert(workspace_preferences_save(path,&p,error,sizeof error)==-1);
    assert(!workspace_preferences_load(path,&loaded,error,sizeof error)&&loaded.window_width==1280);
    char target[4096];snprintf(target,sizeof target,"%s/protected",directory);put(target,"unchanged");
    assert(!unlink(path)&&!symlink(target,path));
    workspace_preferences_defaults(&p);assert(workspace_preferences_save(path,&p,error,sizeof error)==-1);
    assert(workspace_preferences_load(path,&p,error,sizeof error)==-1);
    char data[64];FILE *f=fopen(target,"r");assert(f&&fgets(data,sizeof data,f));fclose(f);assert(!strcmp(data,"unchanged"));
    assert(!unlink(path));assert(!mkfifo(path,0600));assert(workspace_preferences_load(path,&p,error,sizeof error)==-1);assert(!unlink(path));
    /* Resolver isolated from the user's real environment, no read/write. */
    assert(!setenv("XDG_CONFIG_HOME",directory,1));assert(!setenv("HOME",directory,1));
    assert(workspace_preferences_path(path,sizeof path)&&strstr(path,"/cast/workspace.ini"));
    assert(!workspace_preferences_path(data,3));
    snprintf(path,sizeof path,"%s/local",directory);assert(!rmdir(path));assert(!unlink(target));assert(!rmdir(directory));
    puts("workspace preferences PASS: isolated durable roundtrip, defaults, atomic0600, strict format, symlink/FIFO safety");
}
