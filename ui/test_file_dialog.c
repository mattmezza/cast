/* SDL native launch is replaced below: no video init, desktop or file picker. */
#include "file_dialog.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static SDL_DialogFileCallback callback;
static void *callback_user;
static SDL_FileDialogType launch_type;
static unsigned launches;
void SDLCALL SDL_ShowFileDialogWithProperties(SDL_FileDialogType type,SDL_DialogFileCallback fn,void *user,SDL_PropertiesID props)
{
    assert(!callback);callback=fn;callback_user=user;launch_type=type;launches++;
    assert(!SDL_GetBooleanProperty(props,SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,true));
    assert(!strcmp(SDL_GetStringProperty(props,SDL_PROP_FILE_DIALOG_TITLE_STRING,""),"Project"));
    assert(SDL_GetNumberProperty(props,SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,0)==1);
    const SDL_DialogFileFilter *filters=SDL_GetPointerProperty(props,SDL_PROP_FILE_DIALOG_FILTERS_POINTER,NULL);
    assert(filters&&!strcmp(filters[0].pattern,"castproject"));
}
static void finish(const char *const *paths)
{
    SDL_DialogFileCallback fn=callback;void *user=callback_user;assert(fn);callback=NULL;callback_user=NULL;fn(user,paths,0);
}
static int SDLCALL worker(void *paths)
{finish(paths);return 0;}
static FileDialogRequest request(void)
{
    FileDialogRequest r={.argc=3,.path_index=2,.generation=42,.filter_count=1};
    strcpy(r.args[0],"project");strcpy(r.args[1],"open");strcpy(r.title,"Project");
    strcpy(r.filters[0].name,"Cast project");strcpy(r.filters[0].extensions,"castproject");return r;
}
int main(void)
{
    FileDialogRequest r=request();char error[256];assert(file_dialog_request_valid(&r,error,sizeof error));
    FileDialogRequest invalid=r;invalid.argc=9;assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;invalid.path_index=3;assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;invalid.save=invalid.folder=true;assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;memset(invalid.args[0],'x',sizeof invalid.args[0]);assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;strcpy(invalid.filters[0].extensions,"*.mp4");assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;strcpy(invalid.filters[0].extensions,"mp4;");assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;strcpy(invalid.filters[0].extensions,"mp4;mkv;webm");assert(file_dialog_request_valid(&invalid,error,sizeof error));
    invalid=r;strcpy(invalid.args[1],"open\n");assert(!file_dialog_request_valid(&invalid,error,sizeof error));
    FileDialogHost *host=file_dialog_create();assert(host&&!file_dialog_busy(host));
    FileDialogResult result;assert(!file_dialog_poll(host,42,true,&result));
    assert(file_dialog_start(host,&r,NULL)&&file_dialog_busy(host)&&launch_type==SDL_FILEDIALOG_OPENFILE);
    assert(!file_dialog_start(host,&r,NULL)&&launches==1);
    const char *chosen[]={"/tmp/世界 project.castproject",NULL};
    SDL_Thread *thread=SDL_CreateThread(worker,"dialog-test",chosen);assert(thread);SDL_WaitThread(thread,NULL);
    assert(file_dialog_busy(host)); /* Completion remains serialized until poll. */
    assert(!file_dialog_start(host,&r,NULL)&&launches==1);
    assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_SELECTED&&result.argc==3);
    assert(!strcmp(result.args[2],chosen[0])&&!strcmp(result.args[0],"project"));
    assert(!file_dialog_busy(host)&&!file_dialog_poll(host,42,true,&result));
    r.save=true;assert(file_dialog_start(host,&r,NULL)&&launch_type==SDL_FILEDIALOG_SAVEFILE);
    const char *cancelled[]={NULL};finish(cancelled);assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_CANCELLED&&result.argc==0);
    r.save=false;r.folder=true;assert(file_dialog_start(host,&r,NULL)&&launch_type==SDL_FILEDIALOG_OPENFOLDER);
    finish(chosen);assert(file_dialog_poll(host,43,true,&result)&&result.status==FILE_DIALOG_STALE&&result.argc==0&&!result.args[2][0]);
    assert(file_dialog_start(host,&r,NULL));finish(chosen);assert(file_dialog_poll(host,42,false,&result)&&result.status==FILE_DIALOG_STALE&&result.argc==0);
    assert(file_dialog_start(host,&r,NULL));SDL_SetError("test dialog failed");finish(NULL);
    assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_ERROR&&strstr(result.error,"test dialog failed"));
    char long_path[FILE_DIALOG_PATH+1];memset(long_path,'x',sizeof long_path);long_path[sizeof long_path-1]=0;
    const char *too_long[]={long_path,NULL};assert(file_dialog_start(host,&r,NULL));finish(too_long);
    assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_ERROR&&result.argc==0);
    long_path[FILE_DIALOG_PATH-1]=0;assert(file_dialog_start(host,&r,NULL));finish(too_long);
    assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_SELECTED&&strlen(result.args[2])==4095);
    const char *control[]={"/tmp/bad\npath",NULL};assert(file_dialog_start(host,&r,NULL));finish(control);
    assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_ERROR&&result.argc==0);
    const char *multiple[]={"/tmp/first","/tmp/second",NULL};assert(file_dialog_start(host,&r,NULL));finish(multiple);
    assert(file_dialog_poll(host,42,true,&result)&&result.status==FILE_DIALOG_ERROR&&result.argc==0);
    /* Closing while a native dialog is pending must not retain AppState. */
    assert(file_dialog_start(host,&r,NULL));
    file_dialog_close(host);finish(chosen); /* Late callback owns final reference. */
    host=file_dialog_create();assert(host);file_dialog_close(host);
    puts("file dialogs PASS: bounded templates, serialization, selected/cancel/error/stale, late close; no native launch");
    return 0;
}
