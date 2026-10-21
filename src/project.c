#include "project.h"
#include "edition_extensions.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const char *key; char type; size_t offset, size; } ProjectSetting;
/* Explicit persistence boundary: local routes, credentials, licensing and output
 * activation never enter the project. Keep this table independently reviewable. */
static const ProjectSetting settings[] = {
    {"output.width", 'I', offsetof(Config, width), sizeof(((Config *)0)->width)},
    {"output.height", 'I', offsetof(Config, height), sizeof(((Config *)0)->height)},
    {"output.fps", 'I', offsetof(Config, fps), sizeof(((Config *)0)->fps)},
    {"camera.visible", 'B', offsetof(Config, camera_visible), sizeof(((Config *)0)->camera_visible)},
    {"camera.shape", 'E', offsetof(Config, shape), sizeof(((Config *)0)->shape)},
    {"camera.anchor", 'E', offsetof(Config, anchor), sizeof(((Config *)0)->anchor)},
    {"camera.aspect", 'E', offsetof(Config, aspect), sizeof(((Config *)0)->aspect)},
    {"camera.width_percent", 'D', offsetof(Config, camera_width_percent), sizeof(((Config *)0)->camera_width_percent)},
    {"camera.margin", 'I', offsetof(Config, margin), sizeof(((Config *)0)->margin)},
    {"camera.radius", 'I', offsetof(Config, radius), sizeof(((Config *)0)->radius)},
    {"camera.border_width", 'I', offsetof(Config, border_width), sizeof(((Config *)0)->border_width)},
    {"camera.border_color", 'C', offsetof(Config, border_color), sizeof(((Config *)0)->border_color)},
    {"camera.background", 'E', offsetof(Config, camera_background), sizeof(((Config *)0)->camera_background)},
    {"camera.background_color", 'C', offsetof(Config, camera_background_color), sizeof(((Config *)0)->camera_background_color)},
    {"camera.background_blur_radius", 'I', offsetof(Config, camera_background_blur_radius), sizeof(((Config *)0)->camera_background_blur_radius)},
    {"camera.background_brightness", 'D', offsetof(Config, camera_background_brightness), sizeof(((Config *)0)->camera_background_brightness)},
    {"screen.width_percent", 'D', offsetof(Config, screen_width_percent), sizeof(((Config *)0)->screen_width_percent)},
    {"screen.margin", 'I', offsetof(Config, screen_margin), sizeof(((Config *)0)->screen_margin)},
    {"screen.radius", 'I', offsetof(Config, screen_radius), sizeof(((Config *)0)->screen_radius)},
    {"screen.border_width", 'I', offsetof(Config, screen_border_width), sizeof(((Config *)0)->screen_border_width)},
    {"screen.border_color", 'C', offsetof(Config, screen_border_color), sizeof(((Config *)0)->screen_border_color)},
    {"screen.background", 'E', offsetof(Config, screen_background), sizeof(((Config *)0)->screen_background)},
    {"screen.background_color", 'C', offsetof(Config, screen_background_color), sizeof(((Config *)0)->screen_background_color)},
    {"screen.background_blur_radius", 'I', offsetof(Config, screen_background_blur_radius), sizeof(((Config *)0)->screen_background_blur_radius)},
    {"screen.background_brightness", 'D', offsetof(Config, screen_background_brightness), sizeof(((Config *)0)->screen_background_brightness)},
    {"background.source", 'E', offsetof(Config, background_source), sizeof(((Config *)0)->background_source)},
    {"background.gradient_from", 'C', offsetof(Config, gradient_from), sizeof(((Config *)0)->gradient_from)},
    {"background.gradient_via", 'C', offsetof(Config, gradient_via), sizeof(((Config *)0)->gradient_via)},
    {"background.gradient_to", 'C', offsetof(Config, gradient_to), sizeof(((Config *)0)->gradient_to)},
    {"background.gradient_via_enabled", 'B', offsetof(Config, gradient_via_enabled), sizeof(((Config *)0)->gradient_via_enabled)},
    {"background.gradient_angle", 'D', offsetof(Config, gradient_angle), sizeof(((Config *)0)->gradient_angle)},
    {"background.gradient_waypoint", 'D', offsetof(Config, gradient_waypoint), sizeof(((Config *)0)->gradient_waypoint)},
    {"logo.enabled", 'B', offsetof(Config, logo_enabled), sizeof(((Config *)0)->logo_enabled)},
    {"logo.path", 'S', offsetof(Config, logo_path), sizeof(((Config *)0)->logo_path)},
    {"logo.anchor", 'E', offsetof(Config, logo_anchor), sizeof(((Config *)0)->logo_anchor)},
    {"logo.width_percent", 'D', offsetof(Config, logo_width_percent), sizeof(((Config *)0)->logo_width_percent)},
    {"logo.margin_x", 'I', offsetof(Config, logo_margin_x), sizeof(((Config *)0)->logo_margin_x)},
    {"logo.margin_y", 'I', offsetof(Config, logo_margin_y), sizeof(((Config *)0)->logo_margin_y)},
    {"logo.opacity", 'D', offsetof(Config, logo_opacity), sizeof(((Config *)0)->logo_opacity)},
    {"text.enabled", 'B', offsetof(Config, text_enabled), sizeof(((Config *)0)->text_enabled)},
    {"text.content", 'S', offsetof(Config, text_content), sizeof(((Config *)0)->text_content)},
    {"text.font", 'S', offsetof(Config, text_font), sizeof(((Config *)0)->text_font)},
    {"text.size", 'I', offsetof(Config, text_size), sizeof(((Config *)0)->text_size)},
    {"text.color", 'C', offsetof(Config, text_color), sizeof(((Config *)0)->text_color)},
    {"text.anchor", 'E', offsetof(Config, text_anchor), sizeof(((Config *)0)->text_anchor)},
    {"text.margin_x", 'I', offsetof(Config, text_margin_x), sizeof(((Config *)0)->text_margin_x)},
    {"text.margin_y", 'I', offsetof(Config, text_margin_y), sizeof(((Config *)0)->text_margin_y)},
    {"text.opacity", 'D', offsetof(Config, text_opacity), sizeof(((Config *)0)->text_opacity)},
    {"camera.x", 'I', offsetof(Config, camera_x), sizeof(((Config *)0)->camera_x)},
    {"camera.y", 'I', offsetof(Config, camera_y), sizeof(((Config *)0)->camera_y)},
    {"camera.crop_x", 'I', offsetof(Config, crop_x), sizeof(((Config *)0)->crop_x)},
    {"camera.crop_y", 'I', offsetof(Config, crop_y), sizeof(((Config *)0)->crop_y)},
    {"camera.mirror", 'B', offsetof(Config, mirror), sizeof(((Config *)0)->mirror)},
    {"camera.corner_order", 'S', offsetof(Config, corner_order), sizeof(((Config *)0)->corner_order)},
    {"composition.layout", 'E', offsetof(Config, layout), sizeof(((Config *)0)->layout)},
    {"composition.layout_order", 'S', offsetof(Config, layout_order), sizeof(((Config *)0)->layout_order)},
    {"composition.split_side", 'E', offsetof(Config, split_side), sizeof(((Config *)0)->split_side)},
    {"composition.split_ratio", 'D', offsetof(Config, split_ratio), sizeof(((Config *)0)->split_ratio)},
    {"composition.fit", 'E', offsetof(Config, fit), sizeof(((Config *)0)->fit)},
    {"composition.preset_order", 'S', offsetof(Config, preset_order), sizeof(((Config *)0)->preset_order)},
    {"zoom.factor", 'D', offsetof(Config, zoom_factor), sizeof(((Config *)0)->zoom_factor)},
    {"zoom.min", 'D', offsetof(Config, zoom_min), sizeof(((Config *)0)->zoom_min)},
    {"zoom.max", 'D', offsetof(Config, zoom_max), sizeof(((Config *)0)->zoom_max)},
    {"zoom.step", 'D', offsetof(Config, zoom_step), sizeof(((Config *)0)->zoom_step)},
    {"zoom.transition_ms", 'I', offsetof(Config, zoom_transition_ms), sizeof(((Config *)0)->zoom_transition_ms)},
    {"zoom.follow", 'B', offsetof(Config, zoom_follow), sizeof(((Config *)0)->zoom_follow)},
    {"zoom.deadzone", 'I', offsetof(Config, zoom_deadzone), sizeof(((Config *)0)->zoom_deadzone)},
    {"zoom.smoothing", 'D', offsetof(Config, zoom_smoothing), sizeof(((Config *)0)->zoom_smoothing)},
    {"cursor.enabled", 'B', offsetof(Config, cursor), sizeof(((Config *)0)->cursor)},
    {"cursor.highlight", 'B', offsetof(Config, cursor_highlight), sizeof(((Config *)0)->cursor_highlight)},
    {"cursor.size", 'I', offsetof(Config, cursor_size), sizeof(((Config *)0)->cursor_size)},
    {"cursor.color", 'C', offsetof(Config, cursor_color), sizeof(((Config *)0)->cursor_color)},
    {"clicks.enabled", 'B', offsetof(Config, clicks), sizeof(((Config *)0)->clicks)},
    {"clicks.duration_ms", 'I', offsetof(Config, click_duration_ms), sizeof(((Config *)0)->click_duration_ms)},
    {"clicks.radius", 'I', offsetof(Config, click_radius), sizeof(((Config *)0)->click_radius)},
    {"clicks.left_color", 'C', offsetof(Config, click_left_color), sizeof(((Config *)0)->click_left_color)},
    {"clicks.right_color", 'C', offsetof(Config, click_right_color), sizeof(((Config *)0)->click_right_color)},
    {"clicks.middle_color", 'C', offsetof(Config, click_middle_color), sizeof(((Config *)0)->click_middle_color)},
    {"clicks.middle", 'B', offsetof(Config, click_middle), sizeof(((Config *)0)->click_middle)},
    {"keys.enabled", 'B', offsetof(Config, keys), sizeof(((Config *)0)->keys)},
    {"keys.mode", 'E', offsetof(Config, keys_mode), sizeof(((Config *)0)->keys_mode)},
    {"keys.position", 'E', offsetof(Config, keys_position), sizeof(((Config *)0)->keys_position)},
    {"keys.font_size", 'I', offsetof(Config, keys_font_size), sizeof(((Config *)0)->keys_font_size)},
    {"keys.timeout_ms", 'I', offsetof(Config, keys_timeout_ms), sizeof(((Config *)0)->keys_timeout_ms)},
    {"keys.color", 'C', offsetof(Config, keys_color), sizeof(((Config *)0)->keys_color)},
    {"keys.background", 'C', offsetof(Config, keys_background), sizeof(((Config *)0)->keys_background)},
    {"keys.filter", 'S', offsetof(Config, keys_filter), sizeof(((Config *)0)->keys_filter)},
    {"keys.navigation", 'S', offsetof(Config, keys_navigation), sizeof(((Config *)0)->keys_navigation)},
    {"annotations.virtual_keys", 'B', offsetof(Config, annotations_virtual_keys), sizeof(((Config *)0)->annotations_virtual_keys)},
    {"annotations.virtual_clicks", 'B', offsetof(Config, annotations_virtual_clicks), sizeof(((Config *)0)->annotations_virtual_clicks)},
    {"annotations.record_keys", 'B', offsetof(Config, annotations_record_keys), sizeof(((Config *)0)->annotations_record_keys)},
    {"annotations.record_clicks", 'B', offsetof(Config, annotations_record_clicks), sizeof(((Config *)0)->annotations_record_clicks)},
    {"audio.mic_gain", 'D', offsetof(Config, mic_gain), sizeof(((Config *)0)->mic_gain)},
    {"audio.desktop_gain", 'D', offsetof(Config, desktop_gain), sizeof(((Config *)0)->desktop_gain)},
    {"record.container", 'S', offsetof(Config, record_container), sizeof(((Config *)0)->record_container)},
    {"record.video_codec", 'S', offsetof(Config, video_codec), sizeof(((Config *)0)->video_codec)},
    {"record.bitrate_kbps", 'I', offsetof(Config, record_bitrate_kbps), sizeof(((Config *)0)->record_bitrate_kbps)},
    {"record.rate_control", 'E', offsetof(Config, record_rate_control), sizeof(((Config *)0)->record_rate_control)},
    {"record.audio_codec", 'S', offsetof(Config, audio_codec), sizeof(((Config *)0)->audio_codec)},
    {"record.crf", 'I', offsetof(Config, record_crf), sizeof(((Config *)0)->record_crf)},
    {"record.preset", 'S', offsetof(Config, record_preset), sizeof(((Config *)0)->record_preset)},
    {"record.countdown", 'I', offsetof(Config, record_countdown), sizeof(((Config *)0)->record_countdown)},
    {"record.queue", 'I', offsetof(Config, record_queue), sizeof(((Config *)0)->record_queue)},
    {"stream.service", 'E', offsetof(Config, stream.service), sizeof(((Config *)0)->stream.service)},
    {"stream.video_encoder", 'S', offsetof(Config, stream.video_encoder), sizeof(((Config *)0)->stream.video_encoder)},
    {"stream.video_bitrate_kbps", 'I', offsetof(Config, stream.video_bitrate_kbps), sizeof(((Config *)0)->stream.video_bitrate_kbps)},
    {"stream.audio_bitrate_kbps", 'I', offsetof(Config, stream.audio_bitrate_kbps), sizeof(((Config *)0)->stream.audio_bitrate_kbps)},
    {"stream.encoder_preset", 'E', offsetof(Config, stream.encoder_preset), sizeof(((Config *)0)->stream.encoder_preset)},
    {"stream.queue_frames", 'I', offsetof(Config, stream.queue_frames), sizeof(((Config *)0)->stream.queue_frames)},
    {"stream.lag_ms", 'I', offsetof(Config, stream.lag_ms), sizeof(((Config *)0)->stream.lag_ms)},
    {"stream.connect_timeout_ms", 'I', offsetof(Config, stream.connect_timeout_ms), sizeof(((Config *)0)->stream.connect_timeout_ms)},
    {"stream.write_timeout_ms", 'I', offsetof(Config, stream.write_timeout_ms), sizeof(((Config *)0)->stream.write_timeout_ms)},
    {"stream.reconnect_attempts", 'I', offsetof(Config, stream.reconnect_attempts), sizeof(((Config *)0)->stream.reconnect_attempts)},
    {"stream.reconnect_initial_ms", 'I', offsetof(Config, stream.reconnect_initial_ms), sizeof(((Config *)0)->stream.reconnect_initial_ms)},
    {"stream.reconnect_max_ms", 'I', offsetof(Config, stream.reconnect_max_ms), sizeof(((Config *)0)->stream.reconnect_max_ms)},
    {"annotations.stream_keys", 'B', offsetof(Config, annotations_stream_keys), sizeof(((Config *)0)->annotations_stream_keys)},
    {"annotations.stream_clicks", 'B', offsetof(Config, annotations_stream_clicks), sizeof(((Config *)0)->annotations_stream_clicks)},
};
#define SETTING_COUNT (sizeof settings / sizeof settings[0])
static int fail(char *error, size_t size, const char *format, ...)
{
    va_list args; va_start(args, format); vsnprintf(error, size, format, args); va_end(args);
    return -1;
}
static bool extension_safe(const CastExtensionSetting *s)
{
    if (!s || (strcmp(s->section, "zoom") && strcmp(s->section, "cursor") &&
        strcmp(s->section, "notes") && strcmp(s->section, "subtitles") &&
        strcmp(s->section, "transcription"))) return false;
    if (!strcmp(s->key, "enabled") || !strcmp(s->key, "source") ||
        !strcmp(s->key, "device")) return false;
    return s->type != CAST_SETTING_PATH &&
        (s->type != CAST_SETTING_TEXT || !strcmp(s->key, "font"));
}
static const ProjectSetting *setting(const char *key)
{
    for (size_t i = 0; i < SETTING_COUNT; ++i)
        if (!strcmp(settings[i].key, key)) return &settings[i];
    return NULL;
}
static bool allowed(const char *key)
{
    if (setting(key)) return true;
    const char *dot = strchr(key, '.');
    if (!dot || (size_t)(dot - key) >= 64) return false;
    char section[64]; memcpy(section, key, (size_t)(dot-key)); section[dot-key] = 0;
    return extension_safe(cast_extension_setting_find(section, dot+1));
}
static void hex_write(FILE *file, const char *value)
{
    if (!*value) { fputc('-', file); return; }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        fprintf(file, "%02x", *p);
}
static int unhex(const char *text, char *value, size_t size)
{
    if (!strcmp(text, "-")) { value[0] = 0; return 0; }
    size_t length = strlen(text);
    if (length % 2 || length / 2 >= size) return -1;
    for (size_t i = 0; i < length; i += 2) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char)text[i+j];
            unsigned digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : 99;
            if (digit > 15) return -1;
            byte = byte * 16 + digit;
        }
        if (!byte) return -1; /* Embedded NUL must never truncate a setting/path. */
        value[i/2] = (char)byte;
    }
    value[length/2] = 0;
    return 0;
}
static int directory(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    char parent[PATH_MAX];
    size_t length = slash ? (size_t)(slash-path) : 0;
    if (length >= sizeof parent) return -1;
    if (!slash) strcpy(parent, ".");
    else if (!length) strcpy(parent, "/");
    else { memcpy(parent, path, length); parent[length] = 0; }
    char *resolved = realpath(parent, NULL);
    if (!resolved) return -1;
    bool fits = strlen(resolved) < size;
    if (fits) strcpy(out, resolved);
    free(resolved); return fits ? 0 : -1;
}
static int resolve_path(const char *parent, char *path, size_t size)
{
    if (!*path || path[0] == '/') return 0;
    char resolved[CAST_TIMELINE_PATH_MAX];
    int result = snprintf(resolved, sizeof resolved, "%s/%s", parent, path);
    if (result < 0 || (size_t)result >= sizeof resolved || (size_t)result >= size) return -1;
    strcpy(path, resolved); return 0;
}
static const char *portable_path(const char *parent, const char *path)
{
    size_t length = strlen(parent);
    return !strncmp(path, parent, length) && path[length] == '/' ? path + length + 1 : path;
}
static bool present(const char *path)
{
    struct stat st; return !stat(path, &st) && S_ISREG(st.st_mode);
}
static void setting_value(const ProjectSetting *s, const Config *config, char *out, size_t size)
{
    const unsigned char *p = (const unsigned char *)config + s->offset;
    switch (s->type) {
    case 'S': case 'E': snprintf(out,size,"%.*s", (int)s->size, (const char *)p); break;
    case 'B': snprintf(out,size,"%s", *(const bool *)p ? "true" : "false"); break;
    case 'I': snprintf(out,size,"%d", *(const int *)p); break;
    case 'D': snprintf(out,size,"%.17g", *(const double *)p); break;
    case 'C': snprintf(out,size,"#%06" PRIx32, *(const uint32_t *)p); break;
    default: out[0] = 0;
    }
}
int cast_project_save(const char *path, const Config *config, const CastTimeline *timeline,
                      char *error, size_t size)
{
    if (error && size) error[0]=0;
    if (!path || !*path || !config || !timeline) return fail(error,size,"project path/config/timeline required");
    if (config_validate(config,error,size) || timeline_validate(timeline,error,size)) return -1;
    struct stat st;
    if (!lstat(path,&st)) {
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != getuid())
            return fail(error,size,"project destination must be an owned regular file without links");
    } else if (errno != ENOENT) return fail(error,size,"cannot inspect project destination: %s",strerror(errno));
    char parent[PATH_MAX], temporary[PATH_MAX];
    if (directory(path,parent,sizeof parent) || snprintf(temporary,sizeof temporary,"%s/.cast-project-XXXXXX",parent) >= (int)sizeof temporary)
        return fail(error,size,"invalid project directory/path");
    int fd = mkstemp(temporary);
    if (fd < 0) return fail(error,size,"cannot create project temporary: %s",strerror(errno));
    if (fchmod(fd,0600)) { close(fd); unlink(temporary); return fail(error,size,"cannot protect project temporary"); }
    FILE *file = fdopen(fd,"w");
    if (!file) { close(fd); unlink(temporary); return fail(error,size,"cannot open project temporary"); }
    fprintf(file,"CAST_PROJECT %d\nT %" PRIu64 " %" PRIu64 "\n",CAST_PROJECT_VERSION,timeline->next_id,timeline->revision);
    char value[PATH_MAX+1];
    for (size_t i=0;i<SETTING_COUNT;++i) {
        setting_value(&settings[i],config,value,sizeof value);
        fprintf(file,"S %s ",settings[i].key);
        hex_write(file,!strcmp(settings[i].key,"logo.path") ? portable_path(parent,value) : value); fputc('\n',file);
    }
    size_t extension_count;
    const CastExtensionSetting *schema = cast_shared_extension_schema(&extension_count);
    for (size_t i=0;i<extension_count;++i) if (extension_safe(&schema[i])) {
        fprintf(file,"S %s.%s ",schema[i].section,schema[i].key);
        hex_write(file,config_extension_value(config,schema[i].section,schema[i].key)); fputc('\n',file);
    }
    for (int i=0;i<config->preset_count;++i) {
        const Preset *p=&config->presets[i];
        fputs("P ",file); hex_write(file,p->name); fputc(' ',file);
        hex_write(file,p->layout); fputc(' ',file); hex_write(file,p->camera_shape); fputc(' ',file);
        hex_write(file,p->camera_anchor); fputc(' ',file); hex_write(file,p->camera_aspect);
        fprintf(file," %.17g %.17g %.17g %u %u\n",p->camera_width_percent,p->split_ratio,
                p->zoom_factor,(unsigned)p->camera_visible,p->mask);
    }
    for (unsigned i=0;i<timeline->media_count;++i) {
        const CastTimelineMedia *m=&timeline->media[i];
        fprintf(file,"M %" PRIu64 " %u %" PRIu64 " ",m->id,(unsigned)m->kind,m->duration_ns);
        hex_write(file,portable_path(parent,m->path)); fputc('\n',file);
    }
    for (unsigned i=0;i<timeline->clip_count;++i) {
        const CastTimelineClip *c=&timeline->clips[i];
        fprintf(file,"C %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %u %.9g %u\n",
                c->id,c->media_id,c->start_ns,c->source_in_ns,c->duration_ns,c->track,(double)c->gain,(unsigned)c->muted);
    }
    fputs("END\n",file);
    bool failed = ferror(file) || fflush(file) || ftell(file) > CAST_PROJECT_LIMIT || fsync(fd);
    if (fclose(file)) failed=true;
    if (failed || rename(temporary,path)) { unlink(temporary); return fail(error,size,"project save failed before commit: %s",strerror(errno)); }
    /* The rename is already committed: directory sync is best effort on hosts
     * whose filesystems do not support it. Never claim the old file survived. */
    int parent_fd=open(parent,O_RDONLY|O_CLOEXEC);
    if (parent_fd>=0) { (void)fsync(parent_fd); close(parent_fd); }
    return 0;
}
static int number(const char *text, uint64_t *out)
{
    if (!text || *text<'0' || *text>'9') return -1;
    errno=0; char *end; unsigned long long value=strtoull(text,&end,10);
    if (errno || *end) return -1;
    *out=(uint64_t)value; return 0;
}
int cast_project_load(const char *path, Config *config, CastTimeline *timeline,
                      CastProjectReport *report, char *error, size_t size)
{
    if (error && size) error[0]=0;
    if (!path || !config || !timeline) return fail(error,size,"project path/config/timeline required");
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) return fail(error,size,"cannot open project: %s",strerror(errno));
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || (uint64_t)st.st_size>CAST_PROJECT_LIMIT) {
        close(fd); return fail(error,size,"project must be a bounded regular file");
    }
    size_t length=(size_t)st.st_size, used=0;
    char *buffer=malloc(length+1); Config *candidate=malloc(sizeof *candidate);
    CastTimeline *document=calloc(1,sizeof *document);
    if (!buffer || !candidate || !document) { close(fd); free(buffer); free(candidate); free(document); return fail(error,size,"out of memory loading project"); }
    *candidate=*config;
    candidate->preset_count=0;
    while (used<length) {
        ssize_t count=read(fd,buffer+used,length-used);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) break;
        used+=(size_t)count;
    }
    close(fd); buffer[length]=0;
    int result=-1;
    if (used!=length || memchr(buffer,0,length)) { fail(error,size,"project read failed or contains NUL"); goto done; }
    char parent[PATH_MAX];
    if (directory(path,parent,sizeof parent)) { fail(error,size,"project directory unavailable"); goto done; }
    char *line_state=NULL, *line=strtok_r(buffer,"\n",&line_state);
    if (!line || strcmp(line,"CAST_PROJECT 1")) { fail(error,size,"unsupported project version/header"); goto done; }
    bool metadata=false, ended=false;
    char seen[SETTING_COUNT+CAST_MAX_EXTENSION_SETTINGS][128]; unsigned seen_count=0;
    for (line=strtok_r(NULL,"\n",&line_state);line;line=strtok_r(NULL,"\n",&line_state)) {
        if (ended || strlen(line)>2*CAST_TIMELINE_PATH_MAX+256) { fail(error,size,"project trailing data or oversized record"); goto done; }
        char *words[12], *word_state=NULL; unsigned count=0;
        for (char *word=strtok_r(line," \t\r",&word_state);word;word=strtok_r(NULL," \t\r",&word_state)) {
            if (count>=12) { fail(error,size,"too many project record fields"); goto done; }
            words[count++]=word;
        }
        if (!count) continue;
        if (!strcmp(words[0],"END") && count==1) { ended=true; continue; }
        if (!strcmp(words[0],"S") && count==3) {
            if (!allowed(words[1]) || strlen(words[1])>=sizeof seen[0] || seen_count>=sizeof seen/sizeof seen[0]) { fail(error,size,"unknown or forbidden project setting"); goto done; }
            for (unsigned i=0;i<seen_count;++i) if (!strcmp(seen[i],words[1])) { fail(error,size,"duplicate project setting"); goto done; }
            strcpy(seen[seen_count++],words[1]);
            char value[PATH_MAX+1];
            if (unhex(words[2],value,sizeof value) ||
                config_set_value(candidate,words[1],value,error,size)) { if (!*error) fail(error,size,"invalid encoded project setting"); goto done; }
            continue;
        }
        if (!strcmp(words[0],"P") && count==11 && candidate->preset_count<CAST_MAX_PRESETS) {
            Preset *p=&candidate->presets[candidate->preset_count++]; uint64_t visible,mask;
            memset(p,0,sizeof *p);
            if (unhex(words[1],p->name,sizeof p->name) || unhex(words[2],p->layout,sizeof p->layout) ||
                unhex(words[3],p->camera_shape,sizeof p->camera_shape) ||
                unhex(words[4],p->camera_anchor,sizeof p->camera_anchor) ||
                unhex(words[5],p->camera_aspect,sizeof p->camera_aspect) ||
                number(words[9],&visible) || visible>1 || number(words[10],&mask) || mask>UINT_MAX) {
                fail(error,size,"invalid preset record"); goto done;
            }
            double *values[]={&p->camera_width_percent,&p->split_ratio,&p->zoom_factor};
            for (unsigned i=0;i<3;++i) {
                char *end; errno=0; *values[i]=strtod(words[6+i],&end);
                if (errno || *end || !isfinite(*values[i])) { fail(error,size,"invalid preset number"); goto done; }
            }
            p->camera_visible=visible!=0; p->mask=(unsigned)mask; continue;
        }
        if (!strcmp(words[0],"T") && count==3 && !metadata) {
            if (number(words[1],&document->next_id) || number(words[2],&document->revision)) { fail(error,size,"invalid timeline metadata"); goto done; }
            metadata=true; continue;
        }
        if (!strcmp(words[0],"M") && count==5 && document->media_count<CAST_TIMELINE_MEDIA_MAX) {
            CastTimelineMedia *m=&document->media[document->media_count++]; uint64_t kind;
            if (number(words[1],&m->id) || number(words[2],&kind) || kind>CAST_TIMELINE_IMAGE ||
                number(words[3],&m->duration_ns) || unhex(words[4],m->path,sizeof m->path)) { fail(error,size,"invalid media record"); goto done; }
            m->kind=(CastTimelineKind)kind; continue;
        }
        if (!strcmp(words[0],"C") && count==9 && document->clip_count<CAST_TIMELINE_CLIP_MAX) {
            CastTimelineClip *c=&document->clips[document->clip_count++]; uint64_t track,muted;
            char *end=NULL; errno=0; c->gain=strtof(words[7],&end);
            if (errno || !end || *end || !isfinite(c->gain) ||
                number(words[1],&c->id) || number(words[2],&c->media_id) ||
                number(words[3],&c->start_ns) || number(words[4],&c->source_in_ns) ||
                number(words[5],&c->duration_ns) || number(words[6],&track) || track>=CAST_TIMELINE_TRACK_MAX ||
                number(words[8],&muted) || muted>1) { fail(error,size,"invalid clip record"); goto done; }
            c->track=(unsigned)track; c->muted=muted!=0;
            /* Runtime capture locks never survive opening a document. */
            c->capture_locked=false; continue;
        }
        fail(error,size,"unknown, duplicate or excessive project record"); goto done;
    }
    if (!ended || !metadata) { fail(error,size,"incomplete project container"); goto done; }
    CastProjectReport loaded_report={0};
    for (unsigned i=0;i<document->media_count;++i) {
        CastTimelineMedia *m=&document->media[i];
        if (resolve_path(parent,m->path,sizeof m->path)) { fail(error,size,"media reference exceeds path bound"); goto done; }
        m->missing=!present(m->path);
        if (m->missing) loaded_report.missing_media_ids[loaded_report.missing_media_count++]=m->id;
    }
    if (resolve_path(parent,candidate->logo_path,sizeof candidate->logo_path)) { fail(error,size,"logo reference exceeds path bound"); goto done; }
    if (*candidate->logo_path && !present(candidate->logo_path)) loaded_report.missing_asset_count++;
    if (timeline_validate(document,error,size) || config_validate(candidate,error,size)) goto done;
    *config=*candidate; *timeline=*document; if (report) *report=loaded_report;
    result=0;
done:
    free(buffer); free(candidate); free(document); return result;
}
int cast_project_autosave(const char *path, const Config *config, const CastTimeline *timeline,
                          char *error, size_t size)
{
    return cast_project_save(path,config,timeline,error,size);
}
