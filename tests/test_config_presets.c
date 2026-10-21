#include "config_presets.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(!fclose(f));
}
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n >= 0);
    rewind(f);
    char *s = calloc((size_t)n + 1, 1);
    assert(s);
    assert(fread(s, 1, (size_t)n, f) == (size_t)n);
    assert(!fclose(f));
    return s;
}
static const Preset *preset(const Config *c, const char *name)
{
    for (int i = 0; i < c->preset_count; ++i) {
        if (!strcmp(c->presets[i].name, name)) {
            return &c->presets[i];
        }
    }
    return NULL;
}
int main(void)
{
    char dir[] = "/tmp/cast-config-presets-XXXXXX";
    assert(mkdtemp(dir));
    char path[PATH_MAX], fresh[PATH_MAX], nested[PATH_MAX], error[1024];
    snprintf(path, sizeof path, "%s/cast.conf", dir);
    snprintf(nested, sizeof nested, "%s/config/cast", dir);
    snprintf(fresh, sizeof fresh, "%s/config/cast/cast.conf", dir);
    Config *c = malloc(sizeof *c), *out = malloc(sizeof *out), *before = malloc(sizeof *before),
           *loaded = malloc(sizeof *loaded);
    assert(c && out && before && loaded);
    config_defaults(c);
    *out = *c;
    const char *prefix = "# byte-preserved public settings\r\n[camera]\r\nwidth_percent=27\r\n";
    const char *suffix = "[audio]\r\nmic=false\r\ndesktop=false\r\n# keep this comment exactly\r\n";
    char original[1024];
    snprintf(original, sizeof original,
             "%s[composition]\npreset_order=coding,demo,conversation\n[preset.demo]\ncamera_width_"
             "percent=19\n%s",
             prefix, suffix);
    write_text(path, original);
    assert(!config_load(c, path, true, error, sizeof error));
    strcpy(c->layout, "split");
    strcpy(c->shape, "circle");
    strcpy(c->anchor, "top-left");
    strcpy(c->aspect, "4:3");
    c->camera_width_percent = 31;
    c->split_ratio = 42;
    c->camera_visible = false;
    c->zoom_factor = 2;
    assert(!config_presets_edit(path, c, "save", "workspace", NULL, out, error, sizeof error));
    *c = *out;
    const Preset *p = preset(c, "workspace");
    assert(p && p->mask == 255 && !strcmp(p->layout, "split") && p->split_ratio == 42 &&
           !p->camera_visible);
    char *saved = read_text(path);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        assert(!config_presets_edit(path, c, "save", "workspace", NULL, out, error, sizeof error));
        char *again = read_text(path);
        assert(!strcmp(saved, again));
        free(again);
    }
    assert(strstr(saved, prefix) && strstr(saved, suffix));
    assert(!strstr(saved, "socket_path") && !strstr(saved, "text_content"));
    free(saved);
    struct stat st;
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    assert(!config_load(loaded, path, true, error, sizeof error));
    p = preset(loaded, "workspace");
    assert(p && p->camera_width_percent == 31 && p->zoom_factor == 2);
    assert(!strcmp(loaded->layout, "overlay")); /* Other runtime changes were not persisted. */
    assert(!config_presets_edit(path, c, "rename", "workspace", "desk", out, error, sizeof error));
    *c = *out;
    assert(!preset(c, "workspace") && preset(c, "desk") && strstr(c->preset_order, "desk"));
    assert(!config_presets_edit(path, c, "delete", "coding", NULL, out, error, sizeof error));
    *c = *out;
    assert(!config_load(loaded, path, true, error, sizeof error));
    assert(!preset(loaded, "coding") && preset(loaded, "desk"));
    saved = read_text(path);
    assert(strstr(saved, "[preset.coding]\ndisabled = true\n"));
    free(saved);
    /* Every pre-publication failure leaves both runtime output and disk intact. */
    *before = *out;
    saved = read_text(path);
    config_presets_test_write_limit(11);
    assert(config_presets_edit(path, c, "save", "failure", NULL, out, error, sizeof error));
    config_presets_test_write_limit(-1);
    assert(!memcmp(out, before, sizeof *out));
    char *unchanged = read_text(path);
    assert(!strcmp(saved, unchanged));
    free(saved);
    free(unchanged);
    assert(config_presets_edit(path, c, "rename", "desk", "demo", out, error, sizeof error));
    assert(config_presets_edit(path, c, "save", "bad.name", NULL, out, error, sizeof error));
    assert(config_presets_edit(path, c, "save", "next", NULL, out, error, sizeof error));
    assert(!memcmp(out, before, sizeof *out));
    char long_name[44];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = 0;
    assert(config_presets_edit(path, c, "save", long_name, NULL, out, error, sizeof error));
    long_name[42] = 0;
    assert(!config_presets_edit(path, c, "save", long_name, NULL, out, error, sizeof error));
    assert(!config_load(loaded, path, true, error, sizeof error) && preset(loaded, long_name));
    assert(!chmod(path, 0400));
    assert(config_presets_edit(path, c, "save", "readonly", NULL, out, error, sizeof error));
    assert(!chmod(path, 0600));
    char link[PATH_MAX];
    snprintf(link, sizeof link, "%s/link", dir);
    assert(!symlink(path, link));
    assert(config_presets_edit(link, c, "save", "symlink", NULL, out, error, sizeof error));
    assert(!unlink(link));
    /* First launch creates only owned private config directories and preset data. */
    config_defaults(c);
    strcpy(c->layout, "screen");
    assert(!config_presets_edit(fresh, c, "save", "first", NULL, out, error, sizeof error));
    assert(!stat(nested, &st) && (st.st_mode & 0777) == 0700);
    assert(!stat(fresh, &st) && (st.st_mode & 0777) == 0600);
    assert(!config_load(loaded, fresh, true, error, sizeof error));
    assert(preset(loaded, "first") && !strcmp(preset(loaded, "first")->layout, "screen"));
    assert(!strcmp(loaded->layout, "overlay"));
    /* Tombstones cannot smuggle ordinary values, resurrect or disable loosely. */
    write_text(path,
               "[composition]\npreset_order=demo,conversation\n[preset.coding]\ndisabled=true\n");
    assert(!config_load(loaded, path, true, error, sizeof error) && !preset(loaded, "coding"));
    write_text(path, "[preset.coding]\ndisabled=false\n");
    assert(config_load(loaded, path, true, error, sizeof error));
    write_text(path, "[preset.coding]\nlayout=screen\ndisabled=true\n");
    assert(config_load(loaded, path, true, error, sizeof error));
    write_text(path, "[preset.coding]\ndisabled=true\nlayout=screen\n");
    assert(config_load(loaded, path, true, error, sizeof error));
    write_text(path, "[preset.legacy]\nlayout=screen\n");
    assert(!config_load(loaded, path, true, error, sizeof error) && preset(loaded, "coding") &&
           preset(loaded, "legacy"));
    assert(!unlink(path) && !unlink(fresh) && !rmdir(nested));
    snprintf(nested, sizeof nested, "%s/config", dir);
    assert(!rmdir(nested) && !rmdir(dir));
    free(c);
    free(out);
    free(before);
    free(loaded);
    puts("Preset CRUD: public framing roundtrip, tombstones, byte preservation, atomic failure and "
         "private first-launch config passed");
    return 0;
}
