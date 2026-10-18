/* Standalone security regression test: compile this translation unit (engine
 * included with an RFC 8032 test key) against sodium, curl, and libarchive. */
#define CAST_DESKTOP_UPDATE_KEYS_H
#define CAST_DESKTOP_UPDATE_PUBLIC_KEY_HEX "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
#include "../src/desktop_update.c"
#include <assert.h>
static unsigned char secret[64];
static void sign_manifest(unsigned char *out,size_t *len,const char *edition,const char *platform,const char *binary,const char *digest,uint64_t size) {
    int n=snprintf((char *)out,CAST_UPDATE_MANIFEST_MAX,"CAST-DESKTOP-UPDATE-1\nedition=%s\nplatform=%s\nversion=1.2.4\nreleased_at=1790000000\narchive_url=https://downloads.superscreencast.com/archive.tar.gz\narchive_sha256=%s\narchive_size=%llu\nbinary=%s\n",edition,platform,digest,(unsigned long long)size,binary);
    static const unsigned char domain[]="CAST-DESKTOP-UPDATE\0v1\0";
    unsigned char msg[CAST_UPDATE_MANIFEST_MAX+64],sig[64];memcpy(msg,domain,sizeof(domain)-1);memcpy(msg+sizeof(domain)-1,out,(size_t)n);
    crypto_sign_detached(sig,NULL,msg,sizeof(domain)-1+(size_t)n,secret);
    char hex[129];sodium_bin2hex(hex,sizeof(hex),sig,sizeof(sig));
    *len=(size_t)n+(size_t)snprintf((char *)out+n,CAST_UPDATE_MANIFEST_MAX-(size_t)n,"signature=%s\n",hex);
}
static void make_archive(const char *file,int attack) {
    struct archive *a=archive_write_new();assert(archive_write_set_format_pax_restricted(a)==ARCHIVE_OK);assert(archive_write_add_filter_gzip(a)==ARCHIVE_OK);assert(archive_write_open_filename(a,file)==ARCHIVE_OK);
    struct archive_entry *e=archive_entry_new();archive_entry_set_pathname(e,attack==1?"payload/../../escaped":"payload/cast");archive_entry_set_filetype(e,AE_IFREG);archive_entry_set_perm(e,0755);archive_entry_set_size(e,4);
    assert(archive_write_header(a,e)==ARCHIVE_OK);assert(archive_write_data(a,"cast",4)==4);archive_entry_free(e);
    if(attack>=2){e=archive_entry_new();archive_entry_set_pathname(e,"payload/link");archive_entry_set_filetype(e,AE_IFLNK);archive_entry_set_perm(e,0777);archive_entry_set_symlink(e,attack==2?"../../escaped":"cast");assert(archive_write_header(a,e)==ARCHIVE_OK);archive_entry_free(e);}
    assert(archive_write_close(a)==ARCHIVE_OK);archive_write_free(a);
}
static void hash_file(const char *p,char hex[65],uint64_t *size) {
    FILE *f=fopen(p,"rb");assert(f);unsigned char buf[4096],digest[32];size_t n;crypto_hash_sha256_state h;crypto_hash_sha256_init(&h);*size=0;
    while((n=fread(buf,1,sizeof(buf),f))){crypto_hash_sha256_update(&h,buf,n);*size+=n;}assert(!ferror(f));fclose(f);crypto_hash_sha256_final(&h,digest);sodium_bin2hex(hex,65,digest,32);
}
int main(void) {
    assert(sodium_init()>=0);unsigned char seed[32],public[32];size_t used;
    assert(!sodium_hex2bin(seed,32,"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",64,NULL,&used,NULL));crypto_sign_seed_keypair(public,secret,seed);
    unsigned char manifest[CAST_UPDATE_MANIFEST_MAX];size_t len;char err[256],zeros[65];memset(zeros,'0',64);zeros[64]=0;CastDesktopUpdate m;
    sign_manifest(manifest,&len,"community","linux-x86_64","cast",zeros,20);
    assert(!cast_update_parse_manifest(manifest,len,"community","linux-x86_64","1.2.3",&m,err,sizeof(err)));
    assert(cast_update_parse_manifest(manifest,len,"community","linux-x86_64","1.2.4",&m,err,sizeof(err))==1);
    assert(cast_update_parse_manifest(manifest,len,"pro","linux-x86_64","1.2.3",&m,err,sizeof(err))<0);
    assert(cast_update_parse_manifest(manifest,len,"community","macos-arm64","1.2.3",&m,err,sizeof(err))<0);
    manifest[30]^=1;assert(cast_update_parse_manifest(manifest,len,"community","linux-x86_64","1.2.3",&m,err,sizeof(err))<0);manifest[30]^=1;
    manifest[len-4]^=1;assert(cast_update_parse_manifest(manifest,len,"community","linux-x86_64","1.2.3",&m,err,sizeof(err))<0);
    sign_manifest(manifest,&len,"community","linux-x86_64","../cast",zeros,20);assert(cast_update_parse_manifest(manifest,len,"community","linux-x86_64","1.2.3",&m,err,sizeof(err))<0);
    char temp[]="/tmp/cast-update-test-XXXXXX";assert(mkdtemp(temp));char root[CAST_UPDATE_PATH_MAX];assert(realpath(temp,root));
    char arc[CAST_UPDATE_PATH_MAX],dir[CAST_UPDATE_PATH_MAX],target[CAST_UPDATE_PATH_MAX];assert(!pathcat(arc,sizeof(arc),root,"archive"));assert(!pathcat(dir,sizeof(dir),root,"unpack"));
    for(int attack=0;attack<4;attack++){make_archive(arc,attack);assert(!mkdir(dir,0700));int rc=cast_update_extract(arc,dir,"cast",err,sizeof(err));assert((attack==1||attack==2)?rc<0:rc==0);remove_tree(dir);}
    make_archive(arc,3);assert(!mkdir(dir,0700));assert(!cast_update_extract(arc,dir,"cast",err,sizeof(err)));
    char payload[CAST_UPDATE_PATH_MAX];assert(!pathcat(payload,sizeof(payload),dir,"payload"));assert(!pathcat(target,sizeof(target),root,"installed"));assert(!mkdir(target,0755));
    assert(!cast_update_install(payload,target,err,sizeof(err)));char exe[CAST_UPDATE_PATH_MAX];assert(!pathcat(exe,sizeof(exe),target,"cast"));assert(!access(exe,X_OK));
    assert(cast_update_install(payload,exe,err,sizeof(err))<0);assert(!access(exe,X_OK));
    char notcanonical[CAST_UPDATE_PATH_MAX];assert(!pathcat(notcanonical,sizeof(notcanonical),root,"alias"));assert(!symlink(target,notcanonical));assert(cast_update_install(payload,notcanonical,err,sizeof(err))<0);
    CastDesktopUpdateStage stage={0};strcpy(stage.directory,root);char digest[65];uint64_t size;hash_file(arc,digest,&size);sign_manifest(manifest,&len,"community","linux-x86_64","cast",digest,size);
    char mf[CAST_UPDATE_PATH_MAX];assert(!pathcat(mf,sizeof(mf),root,"manifest"));FILE *f=fopen(mf,"wb");assert(f&&fwrite(manifest,1,len,f)==len);assert(!fclose(f));
    assert(!cast_update_stage_verify(&stage,"community","linux-x86_64","1.2.3",&m,err,sizeof(err)));
    f=fopen(arc,"r+b");assert(f);assert(fputc('X',f)!=EOF);assert(!fclose(f));assert(cast_update_stage_verify(&stage,"community","linux-x86_64","1.2.3",&m,err,sizeof(err))<0);
    remove_tree(root);puts("desktop update security tests passed");return 0;
}
