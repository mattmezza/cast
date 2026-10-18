#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "desktop_update.h"
#include "desktop_update_keys.h"
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <sodium.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ARCHIVE ((uint64_t)2 * 1024 * 1024 * 1024)
#define MAX_EXPANDED ((uint64_t)8 * 1024 * 1024 * 1024)
#define MAX_ENTRIES 100000
static int fail(char *e, size_t n, const char *fmt, ...) __attribute__((format(printf,3,4)));
static int fail(char *e, size_t n, const char *fmt, ...) {
    if (e && n) { va_list ap; va_start(ap,fmt); vsnprintf(e,n,fmt,ap); va_end(ap); }
    return -1;
}
static int pathcat(char *out,size_t n,const char *a,const char *b) {
    int z=snprintf(out,n,"%s/%s",a,b); return z<0 || (size_t)z>=n ? -1:0;
}
static int valid_context(const char *edition,const char *platform) {
    return edition && platform && (!strcmp(edition,"community") || !strcmp(edition,"pro")) &&
      (!strcmp(platform,"macos-arm64") || !strcmp(platform,"macos-x86_64") || !strcmp(platform,"linux-x86_64") || !strcmp(platform,"linux-arm64"));
}
static int version(const char *s,unsigned long v[3]) {
    for (int i=0;i<3;i++) {
        if (*s<'0'||*s>'9') return -1;
        const char *start=s; errno=0; char *end; v[i]=strtoul(s,&end,10);
        if (errno || v[i]>UINT32_MAX || (end-start>1 && *start=='0')) return -1;
        s=end; if(i<2) { if(*s++!='.')return -1; } else if(*s)return -1;
    } return 0;
}
static int safe_relative(const char *s) {
    if(!s || !*s || *s=='/' || strchr(s,'\\'))return 0;
    const char *p=s;
    while(*p) { const char *q=strchr(p,'/'); size_t n=q?(size_t)(q-p):strlen(p);
        if(!n || (n==1 && *p=='.') || (n==2 && p[0]=='.' && p[1]=='.'))return 0;
        p=q?q+1:p+n;
    } return 1;
}
static int https_url(const char *s) {
    if(strncmp(s,"https://",8) || !s[8] || s[8]=='/' || strchr(s,'@') || strchr(s,'#'))return 0;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)if(*p<=32||*p>=127||*p=='\\')return 0;
    return 1;
}
int cast_update_parse_manifest(const unsigned char *data,size_t len,const char *edition,
 const char *platform,const char *current,CastDesktopUpdate *out,char *err,size_t en) {
    if(!data||!out||!current||!valid_context(edition,platform)||!len||len>CAST_UPDATE_MANIFEST_MAX||memchr(data,0,len))return fail(err,en,"Invalid update manifest");
    char buf[CAST_UPDATE_MANIFEST_MAX+1]; memcpy(buf,data,len);buf[len]=0;
    /* Exact field order eliminates duplicate/ambiguous fields and signs original bytes. */
    const char *keys[]={"CAST-DESKTOP-UPDATE-1","edition=","platform=","version=","released_at=","archive_url=","archive_sha256=","archive_size=","binary=","signature="};
    char *vals[10]; char *p=buf; size_t signed_len=0;
    for(int i=0;i<10;i++) {
        char *end=strchr(p,'\n'); if(!end)return fail(err,en,"Truncated update manifest");
        *end=0;
        if(i==0) { if(strcmp(p,keys[i]))return fail(err,en,"Unknown update manifest format"); vals[i]=p; }
        else { size_t k=strlen(keys[i]);if(strncmp(p,keys[i],k))return fail(err,en,"Unexpected manifest field");vals[i]=p+k; }
        if(i==9)signed_len=(size_t)(p-buf);
        p=end+1;
    }
    if(*p || strcmp(vals[1],edition)||strcmp(vals[2],platform))return fail(err,en,"Update edition/platform mismatch");
    unsigned char pk[32],sig[64];size_t used;
    if(sodium_init()<0 || sodium_hex2bin(pk,sizeof(pk),CAST_DESKTOP_UPDATE_PUBLIC_KEY_HEX,strlen(CAST_DESKTOP_UPDATE_PUBLIC_KEY_HEX),NULL,&used,NULL)||used!=32 || strlen(vals[9])!=128 || sodium_hex2bin(sig,sizeof(sig),vals[9],128,NULL,&used,NULL)||used!=64)return fail(err,en,"Invalid update signature/key");
    static const unsigned char domain[]="CAST-DESKTOP-UPDATE\0v1\0";
    unsigned char message[sizeof(domain)-1+CAST_UPDATE_MANIFEST_MAX];
    memcpy(message,domain,sizeof(domain)-1);memcpy(message+sizeof(domain)-1,data,signed_len);
    if(crypto_sign_verify_detached(sig,message,sizeof(domain)-1+signed_len,pk))return fail(err,en,"Update signature verification failed");
    CastDesktopUpdate m={0};
#define FIELD(dst,src) do { if(strlen(src)>=sizeof(dst))return fail(err,en,"Manifest field too long");strcpy(dst,src); } while(0)
    FIELD(m.edition,vals[1]);FIELD(m.platform,vals[2]);FIELD(m.version,vals[3]);FIELD(m.archive_url,vals[5]);FIELD(m.archive_sha256,vals[6]);FIELD(m.binary,vals[8]);
#undef FIELD
    unsigned long next[3],old[3];if(version(m.version,next)||version(current,old))return fail(err,en,"Invalid update version");
    char *end; errno=0;m.released_at=strtoll(vals[4],&end,10);
    if(errno||!*vals[4]||*end||m.released_at<=0)return fail(err,en,"Invalid release timestamp");
    for(const char *q=vals[4];*q;q++)if(*q<'0'||*q>'9')return fail(err,en,"Invalid release timestamp");
    errno=0;m.archive_size=strtoull(vals[7],&end,10);
    if(errno||!*vals[7]||*end||!m.archive_size||m.archive_size>MAX_ARCHIVE)return fail(err,en,"Invalid archive size");
    for(const char *q=vals[7];*q;q++)if(*q<'0'||*q>'9')return fail(err,en,"Invalid archive size");
    unsigned char hash[32];if(strlen(m.archive_sha256)!=64||sodium_hex2bin(hash,32,m.archive_sha256,64,NULL,&used,NULL)||used!=32||!https_url(m.archive_url)||!safe_relative(m.binary))return fail(err,en,"Invalid archive metadata");
    /* Executable location is fixed by platform/edition; a signed manifest cannot
       turn validation into a check of an unrelated file. */
    const char *expected=!strncmp(platform,"macos-",6)?(!strcmp(edition,"pro")?"Contents/MacOS/cast-pro":"Contents/MacOS/cast"):(!strcmp(edition,"pro")?"cast-pro":"cast");
    if(strcmp(m.binary,expected))return fail(err,en,"Unexpected executable path");
    memcpy(m.manifest,data,len);m.manifest_size=len;
    *out=m;
    for(int i=0;i<3;i++){if(next[i]>old[i])return 0;if(next[i]<old[i])return 1;}return 1;
}

static _Thread_local const atomic_bool *cancel_flag;
void cast_update_cancel_scope(const atomic_bool *flag) { cancel_flag=flag; }
static int cancelled(void) { return cancel_flag && atomic_load_explicit(cancel_flag,memory_order_relaxed); }
static int transfer_progress(void *unused,curl_off_t total,curl_off_t now,curl_off_t utotal,curl_off_t unow) {
    (void)unused;(void)total;(void)now;(void)utotal;(void)unow;return cancelled();
}
typedef struct { unsigned char *buf; size_t used,limit; FILE *file; } Download;
static size_t receive(void *ptr,size_t a,size_t b,void *ctx) {
    if(cancelled())return 0;
    Download *d=ctx;if(a && b>SIZE_MAX/a)return 0;size_t n=a*b;
    if(n>d->limit-d->used)return 0;
    if(d->file) {if(fwrite(ptr,1,n,d->file)!=n)return 0;}else memcpy(d->buf+d->used,ptr,n);
    d->used+=n;return n;
}
static pthread_once_t curl_once=PTHREAD_ONCE_INIT;
static CURLcode curl_init_result=CURLE_FAILED_INIT;
static void initialize_curl(void) { curl_init_result=curl_global_init(CURL_GLOBAL_DEFAULT); }
static int download(const char *url,Download *d,int channel,char *err,size_t en) {
    if(cancelled())return fail(err,en,"Update cancelled");
    if(!https_url(url))return fail(err,en,"Update download requires HTTPS");
    pthread_once(&curl_once,initialize_curl);
    if(curl_init_result!=CURLE_OK)return fail(err,en,"Cannot initialize update networking");
    CURL *c=curl_easy_init();if(!c)return fail(err,en,"Cannot initialize update download");
    curl_easy_setopt(c,CURLOPT_URL,url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(c,CURLOPT_PROTOCOLS,(long)CURLPROTO_HTTPS);curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS,(long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(c,CURLOPT_MAXREDIRS,3L);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,channel?30L:600L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,1024L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,30L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,d);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,transfer_progress);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"Cast-desktop-updater/1");
    CURLcode r=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(c);
    if(channel&&status==404)return 2;
    return r==CURLE_OK && status==200 ? 0:fail(err,en,"Update download failed (%s, HTTP %ld)",curl_easy_strerror(r),status);
}
int cast_update_check(const char *edition,const char *platform,const char *current,CastDesktopUpdate *out,char *err,size_t en) {
    if(!valid_context(edition,platform))return fail(err,en,"Unsupported update platform");
    char url[256];snprintf(url,sizeof(url),"https://downloads.superscreencast.com/cast/desktop/%s/%s/stable.manifest",edition,platform);
    unsigned char buf[CAST_UPDATE_MANIFEST_MAX];Download d={buf,0,sizeof(buf),NULL};
    int result=download(url,&d,1,err,en);if(result)return result;
    return cast_update_parse_manifest(buf,d.used,edition,platform,current,out,err,en);
}
static void remove_tree(const char *path) {
    struct stat st;if(lstat(path,&st))return;
    if(S_ISDIR(st.st_mode)){DIR *d=opendir(path);if(!d)return;struct dirent *v;while((v=readdir(d))) {
        if(!strcmp(v->d_name,".")||!strcmp(v->d_name,".."))continue;
        char child[CAST_UPDATE_PATH_MAX];if(!pathcat(child,sizeof(child),path,v->d_name))remove_tree(child);
    }closedir(d);rmdir(path);}else unlink(path);
}
void cast_update_stage_cleanup(CastDesktopUpdateStage *s) { if(s && *s->directory){remove_tree(s->directory);memset(s,0,sizeof(*s));} }
static int private_dir(const char *path) {struct stat st;return !lstat(path,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==getuid()&&!(st.st_mode&0077);}
static int inside(const char *root,const char *path) {size_t n=strlen(root);return !strncmp(root,path,n)&&path[n]=='/';}
static int ensure_parents(const char *root,const char *relative) {
    char path[CAST_UPDATE_PATH_MAX];if(pathcat(path,sizeof(path),root,relative))return -1;
    char *p=path+strlen(root)+1;
    while((p=strchr(p,'/'))) { *p=0;struct stat st;
        if(mkdir(path,0700)&&errno!=EEXIST)return -1;
        if(lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid())return -1;
        *p++='/';
    }return 0;
}
int cast_update_extract(const char *file,const char *dir,const char *binary,char *err,size_t en) {
    if(!private_dir(dir)||!safe_relative(binary))return fail(err,en,"Unsafe update staging directory");
    DIR *empty=opendir(dir);if(!empty)return fail(err,en,"Cannot read staging directory");struct dirent *de;int nonempty=0;
    while((de=readdir(empty)))if(strcmp(de->d_name,".")&&strcmp(de->d_name,".."))nonempty=1;closedir(empty);
    if(nonempty)return fail(err,en,"Update staging directory must be empty");
    struct archive *a=archive_read_new();archive_read_support_filter_gzip(a);archive_read_support_format_tar(a);archive_read_support_format_zip(a);
    if(archive_read_open_filename(a,file,65536)!=ARCHIVE_OK){archive_read_free(a);return fail(err,en,"Cannot open update archive");}
    int rc=-1,r;uint64_t expanded=0;unsigned count=0;struct archive_entry *ent;
    char **links=NULL,**targets=NULL;size_t nlinks=0;
    while((r=archive_read_next_header(a,&ent))==ARCHIVE_OK) {
        if(cancelled())goto bad;
        const char *raw=archive_entry_pathname(ent);char rel[CAST_UPDATE_PATH_MAX],dest[CAST_UPDATE_PATH_MAX];
        if(!raw||strlen(raw)>=sizeof(rel))goto bad;strcpy(rel,raw);size_t n=strlen(rel);if(n && rel[n-1]=='/')rel[--n]=0;
        if(!safe_relative(rel)||(strcmp(rel,"payload")&&strncmp(rel,"payload/",8))||++count>MAX_ENTRIES||archive_entry_hardlink(ent))goto bad;
        if(pathcat(dest,sizeof(dest),dir,rel)||ensure_parents(dir,rel))goto bad;
        mode_t kind=archive_entry_filetype(ent);struct stat st;
        if(kind==AE_IFDIR) {if(mkdir(dest,0700)&&errno!=EEXIST)goto bad;if(lstat(dest,&st)||!S_ISDIR(st.st_mode))goto bad;continue;}
        if(!lstat(dest,&st)||errno!=ENOENT)goto bad;
        if(kind==AE_IFLNK) {
            const char *target=archive_entry_symlink(ent);
            if(!target||!*target||*target=='/'||strchr(target,'\\')||strlen(target)>=CAST_UPDATE_PATH_MAX)goto bad;
            /* Links are created only after regular extraction; no entry can write through them. */
            char **nl=realloc(links,(nlinks+1)*sizeof(*links));if(!nl)goto bad;links=nl;
            char **nt=realloc(targets,(nlinks+1)*sizeof(*targets));if(!nt)goto bad;targets=nt;
            links[nlinks]=strdup(dest);targets[nlinks]=strdup(target);
            if(!links[nlinks]||!targets[nlinks]){free(links[nlinks]);free(targets[nlinks]);goto bad;}nlinks++;continue;
        }
        if(kind!=AE_IFREG||archive_entry_size(ent)<0||(uint64_t)archive_entry_size(ent)>MAX_EXPANDED-expanded)goto bad;
        int fd=open(dest,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)goto bad;
        char buf[65536];la_ssize_t got;int io=0;uint64_t actual=0;
        while((got=archive_read_data(a,buf,sizeof(buf)))>0) {
            if(cancelled()){io=1;break;}
            actual+=(uint64_t)got;if(actual>MAX_EXPANDED-expanded){io=1;break;}
            size_t off=0;while(off<(size_t)got){ssize_t w=write(fd,buf+off,(size_t)got-off);if(w<=0){io=1;break;}off+=(size_t)w;}if(io)break;
        }
        if(got<0||actual!=(uint64_t)archive_entry_size(ent)||fchmod(fd,archive_entry_perm(ent)&0111?0755:0644)||fsync(fd))io=1;
        if(close(fd))io=1;if(io)goto bad;expanded+=actual;
    }
    if(r!=ARCHIVE_EOF)goto bad;
    for(size_t i=0;i<nlinks;i++)if(symlink(targets[i],links[i]))goto bad;
    struct stat st;
    char payload[CAST_UPDATE_PATH_MAX],canonical[CAST_UPDATE_PATH_MAX],exe[CAST_UPDATE_PATH_MAX];
    if(pathcat(payload,sizeof(payload),dir,"payload")||!realpath(payload,canonical))goto bad;
    for(size_t i=0;i<nlinks;i++){char resolved[CAST_UPDATE_PATH_MAX];if(!realpath(links[i],resolved)||!inside(canonical,resolved))goto bad;}
    if(pathcat(exe,sizeof(exe),payload,binary)||lstat(exe,&st)||!S_ISREG(st.st_mode)||!(st.st_mode&0111))goto bad;
    rc=0;
bad:
    for(size_t i=0;i<nlinks;i++){free(links[i]);free(targets[i]);}free(links);free(targets);archive_read_free(a);
    if(rc) { char p[CAST_UPDATE_PATH_MAX];if(!pathcat(p,sizeof(p),dir,"payload"))remove_tree(p);return fail(err,en,"Unsafe or incomplete update archive"); }return 0;
}
int cast_update_stage(const CastDesktopUpdate *m,CastDesktopUpdateStage *s,char *err,size_t en) {
    if(!m||!s||!m->archive_size||m->archive_size>MAX_ARCHIVE||!m->manifest_size||m->manifest_size>CAST_UPDATE_MANIFEST_MAX)return fail(err,en,"Invalid update metadata");
    memset(s,0,sizeof(*s));
    /* mkdtemp beneath the OS temporary root is unpredictable, owner-only, and
       short-lived; no persistent trust is placed in cache contents. */
    snprintf(s->directory,sizeof(s->directory),"/tmp/cast-update-%lu-XXXXXX",(unsigned long)getuid());
    if(!mkdtemp(s->directory)){memset(s,0,sizeof(*s));return fail(err,en,"Cannot create update staging directory");}
    char file[CAST_UPDATE_PATH_MAX],extract[CAST_UPDATE_PATH_MAX];pathcat(file,sizeof(file),s->directory,"archive");pathcat(extract,sizeof(extract),s->directory,"unpacked");
    FILE *f=fopen(file,"wb");if(!f){cast_update_stage_cleanup(s);return fail(err,en,"Cannot create update download");}
    Download d={NULL,0,(size_t)m->archive_size,f};int rc=download(m->archive_url,&d,0,err,en);if(fclose(f)&&!rc)rc=fail(err,en,"Cannot finish update download");
    if(!rc&&d.used!=m->archive_size)rc=fail(err,en,"Update archive size mismatch");
    if(!rc){crypto_hash_sha256_state state;crypto_hash_sha256_init(&state);f=fopen(file,"rb");if(!f)rc=fail(err,en,"Cannot verify update download");else{
        unsigned char buf[65536],digest[32],expected[32];size_t n;while((n=fread(buf,1,sizeof(buf),f))){if(cancelled()){rc=fail(err,en,"Update cancelled");break;}crypto_hash_sha256_update(&state,buf,n);}
        if(ferror(f))rc=fail(err,en,"Cannot read update download");fclose(f);crypto_hash_sha256_final(&state,digest);size_t used;
        if(strlen(m->archive_sha256)!=64||sodium_hex2bin(expected,32,m->archive_sha256,64,NULL,&used,NULL)||used!=32||sodium_memcmp(digest,expected,32))rc=fail(err,en,"Update archive checksum mismatch");
    }}
    if(!rc&&(mkdir(extract,0700)||cast_update_extract(file,extract,m->binary,err,en)))rc=-1;
    if(rc){cast_update_stage_cleanup(s);return -1;}
    char manifest[CAST_UPDATE_PATH_MAX];pathcat(manifest,sizeof(manifest),s->directory,"manifest");
    f=fopen(manifest,"wb");if(!f){cast_update_stage_cleanup(s);return fail(err,en,"Cannot preserve update manifest");}
    int written=fwrite(m->manifest,1,m->manifest_size,f)==m->manifest_size;int closed=fclose(f);
    if(!written||closed){cast_update_stage_cleanup(s);return fail(err,en,"Cannot preserve update manifest");}
    pathcat(s->payload,sizeof(s->payload),extract,"payload");return 0;
}
static int copy_tree(const char *root,const char *src,const char *dst,char *err,size_t en) {
    struct stat st;if(lstat(src,&st)||st.st_uid!=getuid())return fail(err,en,"Update payload ownership changed");
    if(S_ISLNK(st.st_mode)) {
        char resolved[CAST_UPDATE_PATH_MAX],link[CAST_UPDATE_PATH_MAX];
        ssize_t n=readlink(src,link,sizeof(link)-1);
        if(n<=0||(size_t)n>=sizeof(link)-1||!realpath(src,resolved)||!inside(root,resolved))return fail(err,en,"Unsafe update symlink");
        link[n]=0;if(link[0]=='/'||symlink(link,dst))return fail(err,en,"Cannot copy update symlink");return 0;
    }
    if(S_ISREG(st.st_mode)) {
        int in=open(src,O_RDONLY|O_NOFOLLOW);if(in<0)return fail(err,en,"Cannot read update payload");
        int out=open(dst,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,st.st_mode&0111?0755:0644);
        if(out<0){close(in);return fail(err,en,"Cannot create installed update");}
        char buf[65536];ssize_t n;int rc=0;
        while((n=read(in,buf,sizeof(buf)))>0){size_t off=0;while(off<(size_t)n){ssize_t w=write(out,buf+off,(size_t)n-off);if(w<=0){rc=-1;break;}off+=(size_t)w;}if(rc)break;}
        if(n<0||fsync(out))rc=-1;close(in);if(close(out))rc=-1;
        return rc?fail(err,en,"Cannot copy installed update"):0;
    }
    if(!S_ISDIR(st.st_mode))return fail(err,en,"Unsupported update payload entry");
    if(mkdir(dst,0755))return fail(err,en,"Cannot create installed update directory");
    DIR *d=opendir(src);if(!d)return fail(err,en,"Cannot read update directory");struct dirent *v;int rc=0;
    while((v=readdir(d))){if(!strcmp(v->d_name,".")||!strcmp(v->d_name,".."))continue;
        char a[CAST_UPDATE_PATH_MAX],b[CAST_UPDATE_PATH_MAX];if(pathcat(a,sizeof(a),src,v->d_name)||pathcat(b,sizeof(b),dst,v->d_name)||copy_tree(root,a,b,err,en)){rc=-1;break;}}
    closedir(d);return rc;
}
int cast_update_install(const char *payload,const char *target,char *err,size_t en) {
    if(!payload||!target||target[0]!='/'||strlen(target)>=CAST_UPDATE_PATH_MAX)return fail(err,en,"Invalid update installation target");
    char source[CAST_UPDATE_PATH_MAX],canonical[CAST_UPDATE_PATH_MAX],parent[CAST_UPDATE_PATH_MAX];struct stat st;
    if(!realpath(payload,source)||lstat(source,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid())return fail(err,en,"Unsafe update payload");
    if(!realpath(target,canonical)||strcmp(target,canonical)||lstat(target,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid())return fail(err,en,"Update target must be a user-owned canonical runtime directory");
    strcpy(parent,target);char *slash=strrchr(parent,'/');if(!slash||slash==parent)return fail(err,en,"Unsafe update target parent");*slash=0;
    if(lstat(parent,&st)||!S_ISDIR(st.st_mode)||(st.st_uid!=getuid()&&st.st_uid!=0)||(st.st_mode&0002)||access(parent,W_OK))return fail(err,en,"Update target parent must be private and user-writable");
    if(!strcmp(source,target)||inside(target,source)||inside(source,target))return fail(err,en,"Update source overlaps destination");
    char pending[CAST_UPDATE_PATH_MAX],backup[CAST_UPDATE_PATH_MAX],newroot[CAST_UPDATE_PATH_MAX];
    if(pathcat(pending,sizeof(pending),parent,".cast-update-new-XXXXXX")||!mkdtemp(pending))return fail(err,en,"Cannot prepare update installation");
    if(pathcat(newroot,sizeof(newroot),pending,"runtime")||copy_tree(source,source,newroot,err,en)){remove_tree(pending);return -1;}
    if(pathcat(backup,sizeof(backup),parent,".cast-update-previous-XXXXXX")||!mkdtemp(backup)){remove_tree(pending);return fail(err,en,"Cannot reserve update rollback location");}
    if(rmdir(backup)||rename(target,backup)){remove_tree(pending);remove_tree(backup);return fail(err,en,"Cannot preserve previous installation");}
    if(rename(newroot,target)) {
        int saved=errno;int rolled_back=rename(backup,target);remove_tree(pending);
        return fail(err,en,rolled_back?"Update install failed and rollback requires recovery from %s":"Update install failed; previous installation restored (%s)",rolled_back?backup:strerror(saved));
    }
    int fd=open(parent,O_RDONLY);if(fd>=0){(void)fsync(fd);close(fd);}
    remove_tree(pending);return 0;
}

int cast_update_stage_verify(CastDesktopUpdateStage *s,const char *edition,const char *platform,
 const char *current,CastDesktopUpdate *out,char *err,size_t en) {
    if(!s||!out||!private_dir(s->directory))return fail(err,en,"Unsafe update staging directory");
    char manifest[CAST_UPDATE_PATH_MAX],archive[CAST_UPDATE_PATH_MAX],verified[CAST_UPDATE_PATH_MAX];
    if(pathcat(manifest,sizeof(manifest),s->directory,"manifest")||pathcat(archive,sizeof(archive),s->directory,"archive")||pathcat(verified,sizeof(verified),s->directory,"verified-XXXXXX"))return fail(err,en,"Staging path too long");
    unsigned char buf[CAST_UPDATE_MANIFEST_MAX+1];int fd=open(manifest,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return fail(err,en,"Cannot read staged update manifest");
    ssize_t total=0,n;while(total<(ssize_t)sizeof(buf)&&(n=read(fd,buf+total,sizeof(buf)-(size_t)total))>0)total+=n;close(fd);
    if(total<=0||total>CAST_UPDATE_MANIFEST_MAX)return fail(err,en,"Invalid staged manifest size");
    int rc=cast_update_parse_manifest(buf,(size_t)total,edition,platform,current,out,err,en);if(rc)return rc<0?-1:fail(err,en,"Staged update is no longer newer");
    fd=open(archive,O_RDONLY|O_NOFOLLOW);if(fd<0)return fail(err,en,"Cannot read staged update archive");
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(uint64_t)st.st_size!=out->archive_size){close(fd);return fail(err,en,"Staged archive size or ownership mismatch");}
    crypto_hash_sha256_state hash;crypto_hash_sha256_init(&hash);unsigned char block[65536],digest[32],expected[32];
    while((n=read(fd,block,sizeof(block)))>0)crypto_hash_sha256_update(&hash,block,(size_t)n);close(fd);
    if(n<0)return fail(err,en,"Cannot hash staged archive");crypto_hash_sha256_final(&hash,digest);size_t used;
    if(sodium_hex2bin(expected,32,out->archive_sha256,64,NULL,&used,NULL)||used!=32||sodium_memcmp(digest,expected,32))return fail(err,en,"Staged archive checksum mismatch");
    if(!mkdtemp(verified))return fail(err,en,"Cannot prepare verified update payload");
    if(cast_update_extract(archive,verified,out->binary,err,en)){remove_tree(verified);return -1;}
    pathcat(s->payload,sizeof(s->payload),verified,"payload");return 0;
}
