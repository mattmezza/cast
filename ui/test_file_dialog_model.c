/* Portable model validation: no SDL headers or libraries. */
#include "file_dialog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    char error[128];FileDialogRequest r={.argc=8,.path_index=7,.filter_count=1};
    for(unsigned i=0;i<7;i++)strcpy(r.args[i],"arg");
    strcpy(r.filters[0].name,"Media");strcpy(r.filters[0].extensions,"mp4;mkv;webm");
    assert(file_dialog_request_valid(&r,error,sizeof error));
    assert(!file_dialog_request_valid(NULL,error,sizeof error));
    r.argc=9;assert(!file_dialog_request_valid(&r,error,sizeof error));r.argc=8;
    r.path_index=8;assert(!file_dialog_request_valid(&r,error,sizeof error));r.path_index=7;
    memset(r.args[7],'x',4095);r.args[7][4095]=0;assert(file_dialog_request_valid(&r,error,sizeof error));
    r.args[7][4095]='x';assert(!file_dialog_request_valid(&r,error,sizeof error));r.args[7][4095]=0;
    r.args[7][8]='\t';assert(!file_dialog_request_valid(&r,error,sizeof error));r.args[7][8]='x';
    memset(r.title,'x',sizeof r.title);assert(!file_dialog_request_valid(&r,error,sizeof error));r.title[0]=0;
    r.filter_count=9;assert(!file_dialog_request_valid(&r,error,sizeof error));r.filter_count=1;
    const char *bad[]={";mp4","mp4;","mp4;;mkv","*.mp4","mp4;*","../mp4","",NULL};
    for(unsigned i=0;bad[i];i++){strcpy(r.filters[0].extensions,bad[i]);assert(!file_dialog_request_valid(&r,error,sizeof error));}
    strcpy(r.filters[0].extensions,"*");assert(file_dialog_request_valid(&r,error,sizeof error));
    r.save=r.folder=true;assert(!file_dialog_request_valid(&r,error,sizeof error));
    puts("file dialog model PASS: portable bounds, command placeholders, filter syntax and modes");
}
