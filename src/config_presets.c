#include "config_presets.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define PRESET_FILE_MAX (1024 * 1024)
static atomic_uint sequence;
#ifdef CAST_TEST
static int64_t write_limit = -1;
void config_presets_test_write_limit(int64_t value)
{
    write_limit = value;
}
#endif
static int fail(char *e, size_t n, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(e, n, format, args);
    va_end(args);
    return -1;
}
static bool valid_name(const char *name)
{
    if (!name || !*name || strlen(name) > CAST_PRESET_NAME_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') &&
            *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}
static bool reserved(const char *name)
{
    return !strcmp(name, "save") || !strcmp(name, "delete") || !strcmp(name, "rename") ||
           !strcmp(name, "next") || !strcmp(name, "prev");
}
static int find(const Config *c, const char *name)
{
    for (int i = 0; i < c->preset_count; ++i) {
        if (!strcmp(c->presets[i].name, name)) {
            return i;
        }
    }
    return -1;
}
static int order_edit(Config *c, const char *old, const char *replacement, bool append, char *e,
                      size_t n)
{
    char original[sizeof c->preset_order], result[sizeof c->preset_order] = "", *save = NULL;
    strcpy(original, c->preset_order);
    bool found = false;
    for (char *part = strtok_r(original, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
        if (!strcmp(part, old)) {
            found = true;
            part = (char *)replacement;
        }
        if (!part || !*part) {
            continue;
        }
        size_t used = strlen(result), length = strlen(part);
        if (used + length + (used != 0) >= sizeof result) {
            return fail(e, n, "preset cycle order is full");
        }
        if (used) {
            strcat(result, ",");
        }
        strcat(result, part);
    }
    if (append && !found) {
        size_t used = strlen(result), length = strlen(replacement);
        if (used + length + (used != 0) >= sizeof result) {
            return fail(e, n, "preset cycle order is full");
        }
        if (used) {
            strcat(result, ",");
        }
        strcat(result, replacement);
    }
    strcpy(c->preset_order, result);
    return 0;
}
static void preset_write(FILE *f, const Preset *p)
{
    fprintf(f, "[preset.%s]\n", p->name);
    if (p->mask & 1) {
        fprintf(f, "layout = %s\n", p->layout);
    }
    if (p->mask & 2) {
        fprintf(f, "camera_shape = %s\n", p->camera_shape);
    }
    if (p->mask & 4) {
        fprintf(f, "camera_width_percent = %.17g\n", p->camera_width_percent);
    }
    if (p->mask & 8) {
        fprintf(f, "split_ratio = %.17g\n", p->split_ratio);
    }
    if (p->mask & 16) {
        fprintf(f, "camera_visible = %s\n", p->camera_visible ? "true" : "false");
    }
    if (p->mask & 32) {
        fprintf(f, "zoom_factor = %.17g\n", p->zoom_factor);
    }
    if (p->mask & 64) {
        fprintf(f, "camera_anchor = %s\n", p->camera_anchor);
    }
    if (p->mask & 128) {
        fprintf(f, "camera_aspect = %s\n", p->camera_aspect);
    }
}
static bool same_file(const struct stat *a, const struct stat *b)
{
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_size != b->st_size ||
        a->st_mode != b->st_mode) {
        return false;
    }
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
#endif
}
/* Create missing config directories only beneath an owned directory. Existing
 * parents stay unchanged; no symlink may stand in for a newly created child. */
static int open_directory(const char *path, unsigned depth, char *e, size_t n)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0 || errno != ENOENT) {
        return fd;
    }
    if (depth == 32 || strlen(path) >= PATH_MAX) {
        return fail(e, n, "config directory nesting exceeds limit");
    }
    char parent[PATH_MAX], name[PATH_MAX];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        strcpy(name, slash + 1);
        if (slash == parent) {
            slash[1] = 0;
        } else {
            *slash = 0;
        }
    } else {
        strcpy(name, parent);
        strcpy(parent, ".");
    }
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        return fail(e, n, "invalid config directory");
    }
    int ancestor = open_directory(parent, depth + 1, e, n);
    if (ancestor < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(ancestor, &st) || st.st_uid != geteuid()) {
        close(ancestor);
        return fail(e, n, "missing config directory requires an owned parent");
    }
    bool created = mkdirat(ancestor, name, 0700) == 0;
    if (!created && errno != EEXIST) {
        close(ancestor);
        return fail(e, n, "cannot create config directory");
    }
    fd = openat(ancestor, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (created && fchmod(fd, 0700)))) {
        close(fd);
        fd = -1;
    }
    close(ancestor);
    return fd;
}
static int store(const char *path, const Config *c, char *e, size_t n)
{
    if (!path || !*path || strlen(path) >= PATH_MAX) {
        return fail(e, n, "preset persistence requires a config path");
    }
    char parent[PATH_MAX], base[PATH_MAX];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        strcpy(base, slash + 1);
        if (slash == parent) {
            slash[1] = 0;
        } else {
            *slash = 0;
        }
    } else {
        strcpy(base, parent);
        strcpy(parent, ".");
    }
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) {
        return fail(e, n, "invalid config filename");
    }
    int directory = open_directory(parent, 0, e, n);
    if (directory < 0) {
        return fail(e, n, "cannot open config directory: %s", strerror(errno));
    }
    struct stat directory_stat;
    if (fstat(directory, &directory_stat) || directory_stat.st_uid != geteuid()) {
        close(directory);
        return fail(e, n, "preset config directory must be owned by this user");
    }
    int fd = openat(directory, base, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK), temp = -1;
    bool existed = fd >= 0;
    struct stat original = {0}, current = {0};
    char *input = NULL, *output = NULL;
    size_t length = 0, used = 0;
    char temporary[96] = "";
    int result = -1;
    if (fd < 0 && errno != ENOENT) {
        fail(e, n, "cannot read regular config: %s", strerror(errno));
        goto done;
    }
    if (existed) {
        if (fstat(fd, &original) || !S_ISREG(original.st_mode) || original.st_uid != geteuid() ||
            !(original.st_mode & S_IWUSR) || original.st_size < 0 ||
            original.st_size > PRESET_FILE_MAX) {
            fail(e, n, "config must be an owned writable regular file under 1 MiB");
            goto done;
        }
        length = (size_t)original.st_size;
    }
    input = calloc(length + 1, 1);
    if (!input) {
        fail(e, n, "out of memory reading config");
        goto done;
    }
    for (size_t at = 0; at < length;) {
        ssize_t got = read(fd, input + at, length - at);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            fail(e, n, "config changed or could not be read");
            goto done;
        }
        at += (size_t)got;
    }
    if (memchr(input, 0, length)) {
        fail(e, n, "config contains binary data");
        goto done;
    }
    FILE *memory = open_memstream(&output, &used);
    if (!memory) {
        fail(e, n, "out of memory serializing presets");
        goto done;
    }
    bool skip = false, composition = false, order_written = false;
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && input[end] != '\n') {
            ++end;
        }
        if (end < length) {
            ++end;
        }
        size_t first = at;
        while (first < end && (input[first] == ' ' || input[first] == '\t')) {
            ++first;
        }
        bool composition_header = false;
        if (first < end && input[first] == '[') {
            size_t close = first + 1;
            while (close < end && input[close] != ']') {
                ++close;
            }
            if (close < end) {
                size_t name = first + 1, last = close;
                while (name < last && isspace((unsigned char)input[name])) {
                    ++name;
                }
                while (last > name && isspace((unsigned char)input[last - 1])) {
                    --last;
                }
                skip = last - name >= 7 && !memcmp(input + name, "preset.", 7);
                composition = last - name == 11 && !memcmp(input + name, "composition", 11);
                composition_header = composition;
            }
        }
        bool order = false;
        if (composition && end - first >= 12 && !memcmp(input + first, "preset_order", 12)) {
            size_t key = first + 12;
            while (key < end && (input[key] == ' ' || input[key] == '\t')) {
                ++key;
            }
            order = key < end && (input[key] == '=' || input[key] == ':');
        }
        if (!skip && !order) {
            fwrite(input + at, 1, end - at, memory);
            if (composition_header && !order_written) {
                if (end && input[end - 1] != '\n') {
                    fputc('\n', memory);
                }
                fprintf(memory, "preset_order = %s\n", c->preset_order);
                order_written = true;
            }
        }
        at = end;
    }
    if (length && input[length - 1] != '\n') {
        fputc('\n', memory);
    }
    if (!order_written) {
        fprintf(memory, "[composition]\npreset_order = %s\n", c->preset_order);
    }
    static const char *builtin[] = {"coding", "demo", "conversation"};
    for (unsigned i = 0; i < 3; ++i) {
        if (find(c, builtin[i]) < 0) {
            fprintf(memory, "[preset.%s]\ndisabled = true\n", builtin[i]);
        }
    }
    for (int i = 0; i < c->preset_count; ++i) {
        preset_write(memory, &c->presets[i]);
    }
    bool failed = ferror(memory);
    if (fclose(memory)) {
        failed = true;
    }
    if (failed || used > PRESET_FILE_MAX) {
        fail(e, n, "preset config serialization exceeds limit");
        goto done;
    }
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        snprintf(temporary, sizeof temporary, ".cast-presets-%ld-%u", (long)getpid(),
                 atomic_fetch_add(&sequence, 1));
        temp = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      0600);
        if (temp >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (temp < 0) {
        temporary[0] = 0;
        fail(e, n, "cannot create atomic preset config: %s", strerror(errno));
        goto done;
    }
    if (fchmod(temp, 0600)) {
        fail(e, n, "cannot secure atomic preset config");
        goto done;
    }
    for (size_t at = 0; at < used;) {
        size_t count = used - at;
#ifdef CAST_TEST
        if (write_limit >= 0) {
            if (at >= (uint64_t)write_limit) {
                fail(e, n, "injected config write failure");
                goto done;
            }
            if (count > (uint64_t)write_limit - at) {
                count = (size_t)((uint64_t)write_limit - at);
            }
        }
#endif
        ssize_t wrote = write(temp, output + at, count);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            fail(e, n, "cannot write atomic preset config");
            goto done;
        }
        at += (size_t)wrote;
    }
    if (fsync(temp)) {
        fail(e, n, "cannot sync atomic preset config");
        goto done;
    }
    if (close(temp)) {
        temp = -1;
        fail(e, n, "cannot close atomic preset config");
        goto done;
    }
    temp = -1;
    int status = fstatat(directory, base, &current, AT_SYMLINK_NOFOLLOW);
    if ((existed && (status || !same_file(&original, &current))) ||
        (!existed && (!status || errno != ENOENT))) {
        fail(e, n, "config changed while saving presets; retry after reload");
        goto done;
    }
    if (existed ? renameat(directory, temporary, directory, base)
                : linkat(directory, temporary, directory, base, 0)) {
        fail(e, n, "cannot publish preset config: %s", strerror(errno));
        goto done;
    }
    if (!existed) {
        (void)unlinkat(directory, temporary, 0);
    }
    temporary[0] = 0;
    /* Publication is the commit point. Directory sync is best effort: a later
     * durability error must not report an unchanged file after a successful rename. */
    (void)fsync(directory);
    result = 0;
done:
    if (fd >= 0) {
        close(fd);
    }
    if (temp >= 0) {
        close(temp);
    }
    if (*temporary) {
        unlinkat(directory, temporary, 0);
    }
    close(directory);
    free(input);
    free(output);
    return result;
}
int config_presets_edit(const char *path, const Config *current, const char *action,
                        const char *name, const char *new_name, Config *out, char *e, size_t n)
{
    if (!current || !out || !action || !valid_name(name)) {
        return fail(e, n, "preset name must be 1-42 ASCII letters, digits, underscores or hyphens");
    }
    if (config_validate(current, e, n)) {
        return -1;
    }
    Config *c = malloc(sizeof *c);
    if (!c) {
        return fail(e, n, "out of memory editing presets");
    }
    *c = *current;
    int result = -1, index = find(c, name);
    if (!strcmp(action, "save")) {
        if (reserved(name)) {
            fail(e, n, "preset name is reserved");
            goto done;
        }
        if (index < 0) {
            if (c->preset_count == CAST_MAX_PRESETS) {
                fail(e, n, "preset capacity reached");
                goto done;
            }
            index = c->preset_count++;
        }
        Preset *p = &c->presets[index];
        memset(p, 0, sizeof *p);
        strcpy(p->name, name);
        p->mask = 255;
        strcpy(p->layout, c->layout);
        strcpy(p->camera_shape, c->shape);
        strcpy(p->camera_anchor, c->anchor);
        strcpy(p->camera_aspect, c->aspect);
        p->camera_width_percent = c->camera_width_percent;
        p->split_ratio = c->split_ratio;
        p->camera_visible = c->camera_visible;
        p->zoom_factor = c->zoom_factor;
        if (order_edit(c, name, name, true, e, n)) {
            goto done;
        }
    } else if (!strcmp(action, "delete")) {
        if (index < 0) {
            fail(e, n, "unknown preset %s", name);
            goto done;
        }
        memmove(c->presets + index, c->presets + index + 1,
                (size_t)(c->preset_count - index - 1) * sizeof *c->presets);
        memset(&c->presets[--c->preset_count], 0, sizeof *c->presets);
        if (order_edit(c, name, NULL, false, e, n)) {
            goto done;
        }
    } else if (!strcmp(action, "rename")) {
        if (index < 0 || !valid_name(new_name) || reserved(new_name) || find(c, new_name) >= 0) {
            fail(e, n, "rename requires an existing preset and unused valid name");
            goto done;
        }
        strcpy(c->presets[index].name, new_name);
        if (order_edit(c, name, new_name, false, e, n)) {
            goto done;
        }
    } else {
        fail(e, n, "unknown preset edit action");
        goto done;
    }
    for (int i = 0; i < c->preset_count; ++i) {
        if (!valid_name(c->presets[i].name)) {
            fail(e, n, "stored preset names must fit the 42-character INI limit");
            goto done;
        }
    }
    if (config_validate(c, e, n) || store(path, c, e, n)) {
        goto done;
    }
    *out = *c;
    result = 0;
done:
    free(c);
    return result;
}
