#include "file_dialog.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct FileDialogHost {
    atomic_uint references;
    SDL_Mutex *lock;
    bool closed,pending,ready;
    FileDialogRequest request;
    FileDialogResult result;
    SDL_DialogFileFilter filters[FILE_DIALOG_FILTERS];
    char error[256];
};
static void release(FileDialogHost *h)
{
    if(atomic_fetch_sub_explicit(&h->references,1,memory_order_acq_rel)==1){SDL_DestroyMutex(h->lock);free(h);}
}
static void SDLCALL completed(void *user,const char *const *files,int filter)
{
    (void)filter;FileDialogHost *h=user;SDL_LockMutex(h->lock);
    if(!h->closed){
        FileDialogResult *out=&h->result;memset(out,0,sizeof *out);out->generation=h->request.generation;
        if(!files){out->status=FILE_DIALOG_ERROR;snprintf(out->error,sizeof out->error,"%s",SDL_GetError());}
        else if(!files[0])out->status=FILE_DIALOG_CANCELLED;
        else {
            /* Never truncate or accept multiple selections. Reuse portable
             * validation on the completed template, including control bytes. */
            size_t length=0;
            while(length<FILE_DIALOG_PATH&&files[0][length])length++;
            if(length==FILE_DIALOG_PATH||files[1]){out->status=FILE_DIALOG_ERROR;snprintf(out->error,sizeof out->error,"Selected path too long or multiple paths returned");}
            else {
                memcpy(h->request.args[h->request.path_index],files[0],length+1);
                if(!length||!file_dialog_request_valid(&h->request,out->error,sizeof out->error)){
                    out->status=FILE_DIALOG_ERROR;
                    if(!out->error[0])snprintf(out->error,sizeof out->error,"Empty selected path");
                } else {out->status=FILE_DIALOG_SELECTED;out->argc=h->request.argc;memcpy(out->args,h->request.args,sizeof out->args);}
            }
        }
        h->ready=true;
    }
    SDL_UnlockMutex(h->lock);release(h);
}
FileDialogHost *file_dialog_create(void)
{
    FileDialogHost *h=calloc(1,sizeof *h);if(!h)return NULL;
    h->lock=SDL_CreateMutex();if(!h->lock){free(h);return NULL;}
    atomic_init(&h->references,1);return h;
}
const char *file_dialog_error(FileDialogHost *h){return h?h->error:"File dialog unavailable";}
bool file_dialog_busy(FileDialogHost *h)
{
    if(!h)return false;
    SDL_LockMutex(h->lock);bool busy=h->pending;SDL_UnlockMutex(h->lock);return busy;
}
bool file_dialog_start(FileDialogHost *h,const FileDialogRequest *request,SDL_Window *parent)
{
    if(!h)return false;
    if(file_dialog_busy(h)){snprintf(h->error,sizeof h->error,"A file dialog is already pending");return false;}
    if(!file_dialog_request_valid(request,h->error,sizeof h->error))return false;
    SDL_PropertiesID props=SDL_CreateProperties();
    if(!props){snprintf(h->error,sizeof h->error,"%s",SDL_GetError());return false;}
    h->request=*request;
    for(unsigned i=0;i<request->filter_count;i++)h->filters[i]=(SDL_DialogFileFilter){h->request.filters[i].name,h->request.filters[i].extensions};
    bool ok=SDL_SetPointerProperty(props,SDL_PROP_FILE_DIALOG_FILTERS_POINTER,h->filters)&&
        SDL_SetNumberProperty(props,SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,request->filter_count)&&
        SDL_SetBooleanProperty(props,SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,false)&&
        SDL_SetStringProperty(props,SDL_PROP_FILE_DIALOG_TITLE_STRING,request->title)&&
        SDL_SetStringProperty(props,SDL_PROP_FILE_DIALOG_LOCATION_STRING,request->default_path);
    if(parent)ok=ok&&SDL_SetPointerProperty(props,SDL_PROP_FILE_DIALOG_WINDOW_POINTER,parent);
    if(!ok){snprintf(h->error,sizeof h->error,"%s",SDL_GetError());SDL_DestroyProperties(props);return false;}
    SDL_LockMutex(h->lock);h->pending=true;h->ready=false;h->error[0]=0;SDL_UnlockMutex(h->lock);
    atomic_fetch_add_explicit(&h->references,1,memory_order_relaxed);
    SDL_ShowFileDialogWithProperties(request->folder?SDL_FILEDIALOG_OPENFOLDER:request->save?SDL_FILEDIALOG_SAVEFILE:SDL_FILEDIALOG_OPENFILE,completed,h,props);
    SDL_DestroyProperties(props);return true;
}
bool file_dialog_poll(FileDialogHost *h,uint64_t generation,bool connected,FileDialogResult *out)
{
    if(!h||!out)return false;
    SDL_LockMutex(h->lock);
    if(!h->ready){SDL_UnlockMutex(h->lock);return false;}
    *out=h->result;
    if(out->generation!=generation||!connected){memset(out,0,sizeof *out);out->status=FILE_DIALOG_STALE;out->generation=h->result.generation;}
    memset(&h->result,0,sizeof h->result);memset(&h->request,0,sizeof h->request);
    h->ready=h->pending=false;SDL_UnlockMutex(h->lock);return true;
}
void file_dialog_close(FileDialogHost *h)
{
    if(!h)return;
    SDL_LockMutex(h->lock);h->closed=true;SDL_UnlockMutex(h->lock);release(h);
}
