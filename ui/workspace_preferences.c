#define _POSIX_C_SOURCE 200809L
#include "workspace_preferences.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define PREF_PATH 4096
static int fail(char *error,size_t size,const char *fmt,...)
{
    if(error&&size){va_list args;va_start(args,fmt);vsnprintf(error,size,fmt,args);va_end(args);}return -1;
}
void workspace_preferences_defaults(WorkspacePreferences *p)
{
    if(p)*p=(WorkspacePreferences){.theme=WORKSPACE_THEME_SYSTEM,.show_sources=true,.show_inspector=true,.timeline_record=true,.window_width=1360,.window_height=820};
}
bool workspace_preferences_valid(const WorkspacePreferences *p)
{
    return p&&p->theme>=WORKSPACE_THEME_SYSTEM&&p->theme<=WORKSPACE_THEME_DARK&&
        p->window_width>=320&&p->window_width<=16384&&p->window_height>=240&&p->window_height<=16384;
}
bool workspace_preferences_path(char *out,size_t size)
{
    if(!out||!size)return false;
    const char *home=getenv("HOME"),*base=NULL,*suffix=NULL;
#ifdef __APPLE__
    base=home;suffix="/Library/Application Support/cast/workspace.ini";
#else
    base=getenv("XDG_CONFIG_HOME");suffix="/cast/workspace.ini";
    if(!base||base[0]!='/'){base=home;suffix="/.config/cast/workspace.ini";}
#endif
    if(!base||base[0]!='/'){out[0]=0;return false;}
    int n=snprintf(out,size,"%s%s",base,suffix);
    if(n<0||(size_t)n>=size){out[0]=0;return false;}return true;
}
static bool path_valid(const char *path)
{
    if(!path||path[0]!='/')return false;
    size_t length=0;while(length<PREF_PATH&&path[length]){if((unsigned char)path[length]<32||(unsigned char)path[length]==127)return false;length++;}
    return length>1&&length<PREF_PATH&&path[length-1]!='/';
}
int workspace_preferences_load(const char *path,WorkspacePreferences *out,char *error,size_t size)
{
    if(error&&size)error[0]=0;
    if(!out||!path_valid(path))return fail(error,size,"Invalid preferences path");
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?1:fail(error,size,"Cannot open workspace preferences: %s",strerror(errno));
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_size>1024||(st.st_mode&0022)){
        close(fd);return fail(error,size,"Unsafe or oversized workspace preferences");
    }
    char data[1025];size_t used=0;
    while(used<sizeof data-1){ssize_t n=read(fd,data+used,sizeof data-1-used);if(n<0&&errno==EINTR)continue;if(n<0){close(fd);return fail(error,size,"Cannot read workspace preferences");}if(!n)break;used+=(size_t)n;}
    close(fd);data[used]=0;
    if(used==1024||memchr(data,0,used))return fail(error,size,"Invalid workspace preferences length");
    /* Bound each decimal run before scanf conversion (including malformed
     * input) so integer overflow is impossible. */
    unsigned digits=0;
    for(size_t i=0;i<used;i++){if(data[i]>='0'&&data[i]<='9'){if(++digits>5)return fail(error,size,"Oversized preferences number");}else digits=0;}
    unsigned version=0,theme=0,sources=0,inspector=0,camera=0,record=0,stream=0;int width=0,height=0,consumed=0;
    int fields=sscanf(data,"CAST_WORKSPACE %u\ntheme %u\nsources %u\ninspector %u\ntimeline %u %u %u\nwindow %d %d\nEND\n%n",&version,&theme,&sources,&inspector,&camera,&record,&stream,&width,&height,&consumed);
    WorkspacePreferences candidate={.theme=(WorkspaceTheme)theme,.show_sources=sources!=0,.show_inspector=inspector!=0,.timeline_camera=camera!=0,.timeline_record=record!=0,.timeline_stream=stream!=0,.window_width=width,.window_height=height};
    if(fields!=9||!consumed||(size_t)consumed!=used||version!=1||sources>1||inspector>1||camera>1||record>1||stream>1||!workspace_preferences_valid(&candidate))return fail(error,size,"Invalid or unsupported workspace preferences");
    *out=candidate;return 0;
}
static int parents(char *path)
{
    for(char *p=path+1;*p;p++)if(*p=='/'){
        *p=0;int rc=mkdir(path,0700);*p='/';if(rc&&errno!=EEXIST)return -1;
    }
    return 0;
}
int workspace_preferences_save(const char *path,const WorkspacePreferences *p,char *error,size_t size)
{
    if(error&&size)error[0]=0;
    if(!path_valid(path)||!workspace_preferences_valid(p))return fail(error,size,"Invalid workspace preferences or path");
    char directory[PREF_PATH];strcpy(directory,path);
    if(parents(directory))return fail(error,size,"Cannot create workspace preferences directory: %s",strerror(errno));
    char *name=strrchr(directory,'/');*name++=0;
    int dir=open(directory[0]?directory:"/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat st;
    if(dir<0)return fail(error,size,"Cannot open workspace preferences directory");
    if(fstat(dir,&st)||st.st_uid!=getuid()||(st.st_mode&0022)){close(dir);return fail(error,size,"Unsafe workspace preferences directory");}
    if(!fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)){
        if(!S_ISREG(st.st_mode)||st.st_uid!=getuid()){close(dir);return fail(error,size,"Unsafe workspace preferences destination");}
    }else if(errno!=ENOENT){close(dir);return fail(error,size,"Cannot inspect workspace preferences destination");}
    char temp[80];int fd=-1;
    /* O_EXCL prevents replacement attacks; bounded attempts, never a retry of
     * the actual user operation. Names contain no project or secret paths. */
    for(unsigned i=0;i<16;i++){
        snprintf(temp,sizeof temp,".workspace-%ld-%u.tmp",(long)getpid(),i);
        fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0||errno!=EEXIST)break;
    }
    if(fd<0){close(dir);return fail(error,size,"Cannot stage workspace preferences");}
    char data[512];int length=snprintf(data,sizeof data,"CAST_WORKSPACE 1\ntheme %u\nsources %u\ninspector %u\ntimeline %u %u %u\nwindow %d %d\nEND\n",(unsigned)p->theme,(unsigned)p->show_sources,(unsigned)p->show_inspector,(unsigned)p->timeline_camera,(unsigned)p->timeline_record,(unsigned)p->timeline_stream,p->window_width,p->window_height);
    size_t written=0;bool ok=length>0&&(size_t)length<sizeof data;
    while(ok&&written<(size_t)length){ssize_t n=write(fd,data+written,(size_t)length-written);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else written+=(size_t)n;}
    if(ok&&fsync(fd))ok=false;
    if(close(fd))ok=false;
    if(ok&&renameat(dir,temp,dir,name))ok=false;
    if(ok&&fsync(dir))ok=false;
    if(!ok)unlinkat(dir,temp,0);
    close(dir);return ok?0:fail(error,size,"Cannot commit workspace preferences");
}
