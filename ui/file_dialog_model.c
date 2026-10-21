#include "file_dialog.h"
#include <stdio.h>
#include <string.h>
static bool text(const char *s,size_t capacity,bool empty)
{
    const char *end=memchr(s,0,capacity);
    if(!end||(!empty&&end==s))return false;
    for(;s<end;s++)if((unsigned char)*s<32||(unsigned char)*s==127)return false;
    return true;
}
static bool extensions(const char *s,size_t capacity)
{
    if(!text(s,capacity,false))return false;
    if(!strcmp(s,"*"))return true;
    bool item=false;
    for(;*s;s++){
        unsigned char c=(unsigned char)*s;
        if(c==';'){if(!item)return false;item=false;}
        else if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))item=true;
        else return false;
    }
    return item;
}
bool file_dialog_request_valid(const FileDialogRequest *r,char *error,size_t size)
{
    const char *failure=NULL;
    if(!r||r->argc<1||r->argc>FILE_DIALOG_ARGS||r->path_index>=r->argc)failure="Invalid command template";
    else if(r->save&&r->folder)failure="Save and folder modes are mutually exclusive";
    else if(!text(r->title,sizeof r->title,true)||!text(r->default_path,sizeof r->default_path,true))failure="Invalid dialog title or default path";
    else if(r->filter_count>FILE_DIALOG_FILTERS)failure="Too many file filters";
    if(!failure)for(unsigned i=0;i<r->argc;i++)if(!text(r->args[i],sizeof r->args[i],i==r->path_index)){failure="Invalid command argument";break;}
    if(!failure)for(unsigned i=0;i<r->filter_count;i++)if(!text(r->filters[i].name,sizeof r->filters[i].name,false)||!extensions(r->filters[i].extensions,sizeof r->filters[i].extensions)){failure="Invalid file filter";break;}
    if(error&&size)snprintf(error,size,"%s",failure?failure:"");
    return !failure;
}
